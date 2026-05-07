#include "Backends/PcvVideoDecoder.h"

#include "Log.h"

#include <stb_image.h>

#include <algorithm>
#include <cstring>

namespace VideoPlayerAddon
{

namespace
{
    // Read a uint32 little-endian from src+offset. No bounds check — caller validates.
    inline uint32_t ReadU32LE(const uint8_t* src, size_t offset)
    {
        return uint32_t(src[offset + 0])        |
               uint32_t(src[offset + 1]) <<  8  |
               uint32_t(src[offset + 2]) << 16  |
               uint32_t(src[offset + 3]) << 24;
    }
} // namespace

PcvVideoDecoder::~PcvVideoDecoder()
{
    Close();
}

bool PcvVideoDecoder::Open(const char* /*path*/)
{
    // PCV1 is always loaded from memory (it lives inside the VideoClip asset's
    // payload). Path-based open has no meaning here.
    return false;
}

bool PcvVideoDecoder::OpenMemory(const uint8_t* data, size_t size, const char* /*codecHint*/)
{
    Close();

    if (data == nullptr || size < sizeof(PcvHeader)) return false;

    PcvHeader h;
    memcpy(&h, data, sizeof(PcvHeader));
    if (h.magic != kPcvMagic)        return false;
    if (h.version != kPcvVersion)    return false;
    if (h.width == 0 || h.height == 0) return false;
    if (h.numFrames == 0)            return false;
    if (h.frameRateMilli == 0)       return false;

    // Validate sizes fit within the buffer.
    const size_t headerEnd  = sizeof(PcvHeader);
    const size_t audioEnd   = headerEnd + h.audioByteSize;
    const size_t framesEnd  = audioEnd  + h.framesByteSize;
    if (audioEnd  > size) return false;
    if (framesEnd > size) return false;

    // Build the frame offset/size table by walking the per-frame section. Each entry
    // is (uint32 size, bytes...). Stop early if we hit corrupt sizes.
    std::vector<uint32_t> frameOffsets;
    std::vector<uint32_t> frameSizes;
    frameOffsets.reserve(h.numFrames);
    frameSizes.reserve(h.numFrames);

    size_t cursor = audioEnd;
    for (uint32_t i = 0; i < h.numFrames; ++i)
    {
        if (cursor + 4 > framesEnd) return false;
        uint32_t fsize = ReadU32LE(data, cursor);
        cursor += 4;
        if (fsize == 0 || cursor + fsize > framesEnd) return false;
        frameOffsets.push_back(uint32_t(cursor));
        frameSizes.push_back(fsize);
        cursor += fsize;
    }

    mSrc           = data;
    mSrcSize       = size;
    mHeader        = h;
    mFrameOffsets  = std::move(frameOffsets);
    mFrameSizes    = std::move(frameSizes);
    mNextFrame     = 0;
    mAudioBase     = headerEnd;
    mAudioPos      = 0;

    // ~20 ms chunk, frame-aligned. For stereo 22050 Hz 16-bit that's 1764 bytes.
    if (HasAudio())
    {
        const uint32_t bytesPerFrame = uint32_t(h.audioNumChannels) * uint32_t(h.audioBitsPerSample / 8);
        const uint32_t targetFrames  = h.audioSampleRate / 50; // 20 ms
        mAudioChunkBytes = std::max<uint32_t>(targetFrames * bytesPerFrame, 1u);
    }

    return true;
}

void PcvVideoDecoder::Close()
{
    if (mFramePixels != nullptr)
    {
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
    }
    mSrc = nullptr;
    mSrcSize = 0;
    mHeader = {};
    mFrameOffsets.clear();
    mFrameOffsets.shrink_to_fit();
    mFrameSizes.clear();
    mFrameSizes.shrink_to_fit();
    mNextFrame = 0;
    mAudioBase = 0;
    mAudioPos  = 0;
    mAudioChunkBytes = 0;
}

bool PcvVideoDecoder::DecodeNextFrame(DecodedFrame& outFrame)
{
    if (mSrc == nullptr) return false;

    if (mNextFrame >= mHeader.numFrames)
    {
        outFrame = {};
        outFrame.endOfStream = true;
        return true;
    }

    // Free the previous frame's pixels — the IVideoDecoder contract says the returned
    // pointer is valid only until the next DecodeNextFrame / Seek / Close.
    if (mFramePixels != nullptr)
    {
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
    }

    const uint8_t* jpegPtr   = mSrc + mFrameOffsets[mNextFrame];
    const int      jpegBytes = int(mFrameSizes[mNextFrame]);

    int decW = 0, decH = 0, decCh = 0;
    mFramePixels = stbi_load_from_memory(jpegPtr, jpegBytes, &decW, &decH, &decCh, /*req_comp=*/4);
    if (mFramePixels == nullptr)
    {
        LogError("PcvVideoDecoder: stbi_load_from_memory failed on frame %u (%d bytes)",
                 mNextFrame, jpegBytes);
        return false;
    }

    if (uint32_t(decW) != mHeader.width || uint32_t(decH) != mHeader.height)
    {
        // The cook is supposed to scale every frame to the header dims. A mismatch
        // means the .oct is corrupt or was hand-edited; bail rather than upload a
        // mis-sized buffer to GPU memory.
        LogError("PcvVideoDecoder: frame %u dims %dx%d don't match header %ux%u",
                 mNextFrame, decW, decH, mHeader.width, mHeader.height);
        stbi_image_free(mFramePixels);
        mFramePixels = nullptr;
        return false;
    }

    outFrame.pixels      = mFramePixels;
    outFrame.byteSize    = size_t(mHeader.width) * size_t(mHeader.height) * 4;
    outFrame.ptsSeconds  = double(mNextFrame) * 1000.0 / double(mHeader.frameRateMilli);
    outFrame.endOfStream = false;

    ++mNextFrame;
    return true;
}

bool PcvVideoDecoder::Seek(double seconds)
{
    if (mSrc == nullptr) return false;

    if (seconds < 0.0) seconds = 0.0;
    const double frames = seconds * double(mHeader.frameRateMilli) / 1000.0;
    uint32_t targetFrame = uint32_t(frames + 0.5);
    if (targetFrame >= mHeader.numFrames) targetFrame = mHeader.numFrames - 1;
    mNextFrame = targetFrame;

    // Snap audio cursor to the same wall-clock time. Round down to frame alignment
    // so we don't tear samples mid-frame.
    if (HasAudio())
    {
        const uint32_t bytesPerFrame = uint32_t(mHeader.audioNumChannels) * uint32_t(mHeader.audioBitsPerSample / 8);
        const uint64_t targetSamples = uint64_t(seconds * double(mHeader.audioSampleRate));
        size_t pos = size_t(targetSamples) * bytesPerFrame;
        if (pos > mHeader.audioByteSize) pos = mHeader.audioByteSize;
        // Floor to a frame boundary.
        if (bytesPerFrame > 0) pos -= (pos % bytesPerFrame);
        mAudioPos = pos;
    }

    return true;
}

AudioStreamDesc PcvVideoDecoder::GetAudioDesc() const
{
    AudioStreamDesc d;
    d.sampleRate    = mHeader.audioSampleRate;
    d.numChannels   = mHeader.audioNumChannels;
    d.bitsPerSample = mHeader.audioBitsPerSample;
    return d;
}

AudioDecodeResult PcvVideoDecoder::DecodeNextAudio(DecodedAudio& outChunk)
{
    if (mSrc == nullptr || !HasAudio())
    {
        outChunk = {};
        return AudioDecodeResult::EndOfStream;
    }

    if (mAudioPos >= mHeader.audioByteSize)
    {
        outChunk = {};
        outChunk.endOfStream = true;
        return AudioDecodeResult::EndOfStream;
    }

    const uint32_t bytesPerFrame = uint32_t(mHeader.audioNumChannels) * uint32_t(mHeader.audioBitsPerSample / 8);
    size_t remaining = mHeader.audioByteSize - mAudioPos;
    size_t take = (remaining < mAudioChunkBytes) ? remaining : mAudioChunkBytes;
    // Frame-align the chunk so we never split a stereo sample.
    if (bytesPerFrame > 0)
    {
        take -= (take % bytesPerFrame);
        if (take == 0)
        {
            // Last partial frame — flush whatever's left aligned to a frame boundary.
            take = remaining - (remaining % bytesPerFrame);
            if (take == 0)
            {
                outChunk = {};
                outChunk.endOfStream = true;
                return AudioDecodeResult::EndOfStream;
            }
        }
    }

    outChunk.samples      = mSrc + mAudioBase + mAudioPos;
    outChunk.byteSize     = take;
    outChunk.sampleCount  = (bytesPerFrame > 0) ? uint32_t(take / bytesPerFrame) : 0;
    outChunk.ptsSeconds   = (mHeader.audioSampleRate > 0)
        ? double(mAudioPos / bytesPerFrame) / double(mHeader.audioSampleRate)
        : 0.0;
    outChunk.endOfStream  = false;

    mAudioPos += take;
    return AudioDecodeResult::Produced;
}

} // namespace VideoPlayerAddon
