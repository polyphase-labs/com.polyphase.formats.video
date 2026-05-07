#include "Backends/ThpVideoDecoder.h"

#include "Log.h"

#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace VideoPlayerAddon
{

namespace
{
    inline uint32_t ReadBE32(const uint8_t* p)
    {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
               uint32_t(p[2]) <<  8 | uint32_t(p[3]);
    }
    inline float ReadBEFloat(const uint8_t* p)
    {
        const uint32_t v = ReadBE32(p);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    inline bool MagicIsThp(const uint8_t* p)
    {
        return p[0] == 'T' && p[1] == 'H' && p[2] == 'P' && p[3] == '\0';
    }

    // Sample-memory invariant: the engine's streaming-audio API takes int16_t
    // PCM samples as a byte stream in **little-endian** order on every target
    // (e.g. ASND on Wii is configured with VOICE_*_LE; PortAudio / NDSP on PC
    // & 3DS treat int16 as host-endian which is also LE on those hosts). PCV1
    // already satisfies this — its samples were emitted by the cooker on x86
    // (LE) and pass through untouched at runtime.
    //
    // THP/N3MV samples are reconstructed at runtime by the software ADPCM
    // decoder and written via plain `int16_t` stores, which on Wii / GameCube
    // (PowerPC, big-endian) end up as BE bytes in memory. Submitting BE bytes
    // to ASND with the LE flag makes the DSP byte-swap them, producing wildly
    // pitched / clipped noise. This helper writes an int16 sample to memory
    // in LE order regardless of host endianness, restoring the LE invariant.
    inline void WriteSampleLE(int16_t* dst, int16_t v)
    {
#if PLATFORM_DOLPHIN
        // PowerPC = big-endian. Manual byte order so the in-memory bytes are LE.
        const uint16_t u = static_cast<uint16_t>(v);
        uint8_t* b = reinterpret_cast<uint8_t*>(dst);
        b[0] = static_cast<uint8_t>(u & 0xFF);
        b[1] = static_cast<uint8_t>(u >> 8);
#else
        // x86 / ARM little-endian platforms — direct store already produces LE.
        *dst = v;
#endif
    }
} // namespace

ThpVideoDecoder::~ThpVideoDecoder()
{
    Close();
}

bool ThpVideoDecoder::ReadAt(size_t offset, size_t size, void* dst)
{
    if (mSrc != nullptr)
    {
        if (offset + size > mSrcSize) return false;
        std::memcpy(dst, mSrc + offset, size);
        return true;
    }
    if (mFile != nullptr)
    {
        if (offset + size > mFileSize) return false;
        if (fseek(mFile, long(offset), SEEK_SET) != 0) return false;
        return fread(dst, 1, size, mFile) == size;
    }
    return false;
}

uint32_t ThpVideoDecoder::ReadBE32At(size_t offset)
{
    uint8_t buf[4] = {};
    ReadAt(offset, 4, buf);
    return ReadBE32(buf);
}

float ThpVideoDecoder::ReadBEFloatAt(size_t offset)
{
    uint8_t buf[4] = {};
    ReadAt(offset, 4, buf);
    return ReadBEFloat(buf);
}

bool ThpVideoDecoder::Open(const char* path)
{
    Close();
    if (path == nullptr || path[0] == '\0') return false;

    FILE* f = fopen(path, "rb");
    if (f == nullptr)
    {
        LogError("ThpVideoDecoder::Open: fopen failed for '%s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x40)
    {
        fclose(f);
        return false;
    }

    // Sniff magic before committing.
    uint8_t magic[4] = {};
    if (fread(magic, 1, 4, f) != 4 || !MagicIsThp(magic))
    {
        fclose(f);
        return false;
    }

    mFile     = f;
    mFileSize = size_t(sz);

    if (!ParseHeaderAndComponents()) { Close(); return false; }
    if (!BuildFrameTable())          { Close(); return false; }
    return true;
}

bool ThpVideoDecoder::OpenMemory(const uint8_t* data, size_t size, const char* /*codecHint*/)
{
    Close();

    if (data == nullptr || size < 0x40) return false;
    if (!MagicIsThp(data)) return false;

    mSrc = data;
    mSrcSize = size;

    if (!ParseHeaderAndComponents()) { Close(); return false; }
    if (!BuildFrameTable())          { Close(); return false; }

    return true;
}

bool ThpVideoDecoder::ParseHeaderAndComponents()
{
    const size_t totalSize = (mSrc != nullptr) ? mSrcSize : mFileSize;
    if (totalSize < 0x40) return false;

    /*const uint32_t version          = ReadBE32At(0x04);*/
    /*const uint32_t maxBufferSize    = ReadBE32At(0x08);*/
    mFps                              = ReadBEFloatAt(0x10);
    mNumFrames                        = ReadBE32At(0x14);
    /*const uint32_t firstFrameSize   = ReadBE32At(0x18);*/
    /*const uint32_t totalDataSize    = ReadBE32At(0x1C);*/
    const uint32_t componentDataOff   = ReadBE32At(0x20);
    /*const uint32_t offsetsOff       = ReadBE32At(0x24);*/
    mFirstFrameOffset                 = ReadBE32At(0x28);
    /*const uint32_t lastFrameOff     = ReadBE32At(0x2C);*/

    if (mNumFrames == 0) return false;
    if (componentDataOff + 20 > totalSize) return false;
    if (mFirstFrameOffset >= totalSize) return false;

    // Components table: 4-byte numComponents + 16 bytes of types + 4 pad.
    uint8_t compHeader[24] = {};
    if (!ReadAt(componentDataOff, 24, compHeader)) return false;
    const uint32_t numComponents = ReadBE32(compHeader);
    if (numComponents == 0 || numComponents > 16) return false;

    size_t infoOffset = componentDataOff + 4 + 16 + 4;
    for (uint32_t i = 0; i < numComponents; ++i)
    {
        const uint8_t componentType = compHeader[4 + i];
        if (componentType == 0)
        {
            if (infoOffset + 8 > totalSize) return false;
            mWidth  = ReadBE32At(infoOffset + 0);
            mHeight = ReadBE32At(infoOffset + 4);
            infoOffset += 8;
        }
        else if (componentType == 1)
        {
            if (infoOffset + 12 > totalSize) return false;
            mAudioNumChannels  = ReadBE32At(infoOffset + 0);
            mAudioSampleRate   = ReadBE32At(infoOffset + 4);
            mAudioTotalSamples = ReadBE32At(infoOffset + 8);
            infoOffset += 12;
        }
        else
        {
            return false;
        }
    }

    if (mWidth == 0 || mHeight == 0) return false;
    if (mFps <= 0.0f || mFps > 240.0f) return false;
    if (mAudioNumChannels > 2) return false;

    mChCache.assign(mAudioNumChannels, {});
    return true;
}

bool ThpVideoDecoder::BuildFrameTable()
{
    mFrames.clear();
    mFrames.reserve(mNumFrames);

    const size_t totalSize = (mSrc != nullptr) ? mSrcSize : mFileSize;
    size_t cursor = mFirstFrameOffset;

    for (uint32_t i = 0; i < mNumFrames; ++i)
    {
        if (cursor + 8 > totalSize) return false;

        FrameRecord rec;
        rec.frameStart   = cursor;
        const uint32_t nextFrameSize = ReadBE32At(cursor);
        /*const uint32_t prevFrameSize = ReadBE32At(cursor + 4);*/
        cursor += 8;

        // Video chunk.
        if (cursor + 4 > totalSize) return false;
        rec.videoSize   = ReadBE32At(cursor);
        cursor += 4;
        rec.videoOffset = uint32_t(cursor);
        if (cursor + rec.videoSize > totalSize) return false;
        cursor += rec.videoSize;

        // Audio chunk (if audio component exists).
        rec.audioSamples = 0;
        if (mAudioNumChannels > 0)
        {
            if (cursor + 8 > totalSize) return false;
            const uint32_t audioSize     = ReadBE32At(cursor);
            const uint32_t numSamples    = ReadBE32At(cursor + 4);
            cursor += 8;
            const uint32_t channelDataBytes = audioSize - 8;
            const uint32_t bytesPerChannel  = channelDataBytes / mAudioNumChannels;
            const uint32_t headerBytes      = uint32_t(sizeof(DspChannelHeader));
            if (bytesPerChannel < headerBytes) return false;
            const uint32_t adpcmBytes = bytesPerChannel - headerBytes;

            for (uint32_t ch = 0; ch < mAudioNumChannels; ++ch)
            {
                if (cursor + bytesPerChannel > totalSize) return false;
                FrameRecord::ChannelSlice slice;
                slice.offset = uint32_t(cursor + headerBytes);
                slice.size   = adpcmBytes;
                rec.audio.push_back(slice);
                cursor += bytesPerChannel;
            }
            rec.audioSamples = numSamples;
        }

        mFrames.push_back(std::move(rec));

        if (i + 1 < mNumFrames && nextFrameSize == 0) return false;
        if (i + 1 == mNumFrames) break;
    }

    // Cache per-channel header from the FIRST audio block.
    if (mAudioNumChannels > 0 && !mFrames.empty())
    {
        const FrameRecord& first = mFrames.front();
        if (first.audio.size() == mAudioNumChannels)
        {
            for (uint32_t ch = 0; ch < mAudioNumChannels; ++ch)
            {
                const size_t hdrOffset = first.audio[ch].offset - sizeof(DspChannelHeader);
                DspChannelHeader hdr = {};
                ReadAt(hdrOffset, sizeof(hdr), &hdr);
                mChCache[ch] = DspHeaderFromBE(hdr);
            }
        }
    }

    // Initialize per-channel running state to whatever the cached headers say is
    // the start-of-stream history (typically 0,0 for non-loop content).
    mChYn1.assign(mAudioNumChannels, 0);
    mChYn2.assign(mAudioNumChannels, 0);
    for (uint32_t ch = 0; ch < mAudioNumChannels; ++ch)
    {
        mChYn1[ch] = mChCache[ch].yn1;
        mChYn2[ch] = mChCache[ch].yn2;
    }
    return true;
}

void ThpVideoDecoder::Close()
{
    if (mFramePixels != nullptr)
    {
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
    }
    mSrc = nullptr;
    mSrcSize = 0;
    if (mFile != nullptr)
    {
        fclose(mFile);
        mFile = nullptr;
    }
    mFileSize = 0;
    mFrameReadBuf.clear();
    mFrameReadBuf.shrink_to_fit();
    mAudioReadBuf.clear();
    mAudioReadBuf.shrink_to_fit();
    mWidth = mHeight = 0;
    mFps = 0.0f;
    mNumFrames = 0;
    mFirstFrameOffset = 0;
    mAudioNumChannels = 0;
    mAudioSampleRate = 0;
    mAudioTotalSamples = 0;
    mFrames.clear();
    mFrames.shrink_to_fit();
    mChCache.clear();
    mChCache.shrink_to_fit();
    mChYn1.clear();
    mChYn2.clear();
    mNextFrame = 0;
    mAudioFrameCursor = 0;
    mAudioPcmScratch.clear();
    mAudioPcmScratch.shrink_to_fit();
}

bool ThpVideoDecoder::DecodeNextFrame(DecodedFrame& outFrame)
{
    if (mSrc == nullptr && mFile == nullptr) return false;

    if (mNextFrame >= mNumFrames)
    {
        outFrame = {};
        outFrame.endOfStream = true;
        return true;
    }

    if (mFramePixels != nullptr)
    {
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
    }

    const FrameRecord& rec = mFrames[mNextFrame];
    int decW = 0, decH = 0, decCh = 0;
    if (mSrc != nullptr)
    {
        mFramePixels = stbi_load_from_memory(mSrc + rec.videoOffset, int(rec.videoSize),
                                             &decW, &decH, &decCh, 4);
    }
    else
    {
        // Streaming mode: pull this frame's JPEG bytes off disk into scratch.
        if (mFrameReadBuf.size() < rec.videoSize) mFrameReadBuf.resize(rec.videoSize);
        if (!ReadAt(rec.videoOffset, rec.videoSize, mFrameReadBuf.data()))
        {
            LogError("ThpVideoDecoder: ReadAt failed for video frame %u", mNextFrame);
            return false;
        }
        mFramePixels = stbi_load_from_memory(mFrameReadBuf.data(), int(rec.videoSize),
                                             &decW, &decH, &decCh, 4);
    }
    if (mFramePixels == nullptr)
    {
        LogError("ThpVideoDecoder: stbi_load_from_memory failed on frame %u", mNextFrame);
        return false;
    }
    if (uint32_t(decW) != mWidth || uint32_t(decH) != mHeight)
    {
        LogError("ThpVideoDecoder: frame %u dims %dx%d != header %ux%u",
                 mNextFrame, decW, decH, mWidth, mHeight);
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
        return false;
    }

    outFrame.pixels      = mFramePixels;
    outFrame.byteSize    = size_t(mWidth) * size_t(mHeight) * 4;
    outFrame.ptsSeconds  = double(mNextFrame) / double(mFps);
    outFrame.endOfStream = false;

    ++mNextFrame;
    return true;
}

bool ThpVideoDecoder::Seek(double seconds)
{
    if (mSrc == nullptr && mFile == nullptr) return false;
    if (seconds < 0.0) seconds = 0.0;
    uint32_t target = uint32_t(seconds * double(mFps) + 0.5);
    if (target >= mNumFrames) target = mNumFrames - 1;
    mNextFrame        = target;
    mAudioFrameCursor = target;

    // Seeking discards the streaming ADPCM history. Resetting to the cached
    // start-of-stream values produces a single brief click at the seek point
    // (decoder predicts off zero history for the first block); accepting that
    // for v1 since proper seek would need to re-decode from the previous keyframe.
    for (uint32_t ch = 0; ch < mAudioNumChannels; ++ch)
    {
        mChYn1[ch] = mChCache[ch].yn1;
        mChYn2[ch] = mChCache[ch].yn2;
    }
    return true;
}

AudioStreamDesc ThpVideoDecoder::GetAudioDesc() const
{
    AudioStreamDesc d;
    d.sampleRate    = mAudioSampleRate;
    d.numChannels   = mAudioNumChannels;
    d.bitsPerSample = 16;
    return d;
}

AudioDecodeResult ThpVideoDecoder::DecodeNextAudio(DecodedAudio& outChunk)
{
    if ((mSrc == nullptr && mFile == nullptr) || !HasAudio())
    {
        outChunk = {};
        return AudioDecodeResult::EndOfStream;
    }

    if (mAudioFrameCursor >= mNumFrames)
    {
        outChunk = {};
        outChunk.endOfStream = true;
        return AudioDecodeResult::EndOfStream;
    }

    const FrameRecord& rec = mFrames[mAudioFrameCursor];
    if (rec.audio.size() != mAudioNumChannels || rec.audioSamples == 0)
    {
        ++mAudioFrameCursor;
        return AudioDecodeResult::NeedsMoreInput;
    }

    // Decode each channel independently into a per-channel temp buffer, then
    // interleave into mAudioPcmScratch for the streaming voice.
    const uint32_t samples  = rec.audioSamples;
    const size_t   needTmp  = size_t(samples) * mAudioNumChannels;
    if (mAudioPcmScratch.size() < needTmp) mAudioPcmScratch.resize(needTmp);

    std::vector<int16_t> chTmp(samples);
    for (uint32_t ch = 0; ch < mAudioNumChannels; ++ch)
    {
        DspChannelHeader hdr = mChCache[ch];
        // Override numSamples to this frame's count so DspDecode emits exactly the
        // expected sample count (header may carry the FULL clip's count, not the
        // per-frame slice count).
        hdr.numSamples = samples;
        // Critical: ADPCM is a streaming format. Each block predicts off the
        // PREVIOUS block's decoded yn1/yn2. Frame audio chunks are slices of one
        // continuous stream — using the cached frame-0 header (yn1=yn2=0) for
        // every frame causes a loud bass pop at every frame boundary because the
        // decoder's prediction history snaps back to zero between frames.
        // Carry the running state forward instead.
        hdr.yn1 = mChYn1[ch];
        hdr.yn2 = mChYn2[ch];

        const uint8_t* adpcmPtr = nullptr;
        if (mSrc != nullptr)
        {
            adpcmPtr = mSrc + rec.audio[ch].offset;
        }
        else
        {
            // Streaming mode: pull this channel's ADPCM bytes for this frame.
            if (mAudioReadBuf.size() < rec.audio[ch].size) mAudioReadBuf.resize(rec.audio[ch].size);
            if (!ReadAt(rec.audio[ch].offset, rec.audio[ch].size, mAudioReadBuf.data()))
            {
                continue; // skip this channel rather than emit garbage
            }
            adpcmPtr = mAudioReadBuf.data();
        }

        DspDecode(hdr,
                  adpcmPtr,
                  rec.audio[ch].size,
                  chTmp.data(),
                  samples);

        // Capture the last two decoded samples as the next frame's starting
        // history. (DspDecode would have ended its internal yn1/yn2 at exactly
        // these values; reading them from the output buffer is equivalent.)
        if (samples >= 2)
        {
            mChYn1[ch] = chTmp[samples - 1];
            mChYn2[ch] = chTmp[samples - 2];
        }
        else if (samples == 1)
        {
            mChYn2[ch] = mChYn1[ch];
            mChYn1[ch] = chTmp[0];
        }

        // Interleave into output. Force LE byte order regardless of host —
        // see WriteSampleLE comment for why.
        for (uint32_t i = 0; i < samples; ++i)
        {
            WriteSampleLE(&mAudioPcmScratch[i * mAudioNumChannels + ch], chTmp[i]);
        }
    }

    outChunk.samples      = reinterpret_cast<const uint8_t*>(mAudioPcmScratch.data());
    outChunk.byteSize     = size_t(samples) * mAudioNumChannels * sizeof(int16_t);
    outChunk.sampleCount  = samples;
    outChunk.ptsSeconds   = double(mAudioFrameCursor) / double(mFps);
    outChunk.endOfStream  = false;

    ++mAudioFrameCursor;
    return AudioDecodeResult::Produced;
}

} // namespace VideoPlayerAddon
