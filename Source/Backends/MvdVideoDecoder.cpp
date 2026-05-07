#include "Backends/MvdVideoDecoder.h"

#include "Cook/N3dsMvdCook.h" // N3mvHeader, kN3mvMagic
#include "Log.h"

#include <cstdio>
#include <cstring>

#if defined(PLATFORM_3DS) && PLATFORM_3DS
    // Real hardware path. libctru exposes mvdstd for H.264 decode and Y2RU for
    // YUV->RGB. Both must be initialized once per stream.
    #include <3ds.h>
#endif

namespace VideoPlayerAddon
{

MvdVideoDecoder::~MvdVideoDecoder()
{
    Close();
}

bool MvdVideoDecoder::ParseHeader()
{
    N3mvHeader hdr = {};
    if (mSrc != nullptr)
    {
        if (mSrcSize < sizeof(hdr)) return false;
        std::memcpy(&hdr, mSrc, sizeof(hdr));
    }
    else if (mFile != nullptr)
    {
        FILE* f = static_cast<FILE*>(mFile);
        if (fseek(f, 0, SEEK_SET) != 0) return false;
        if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    }
    else
    {
        return false;
    }

    if (hdr.magic != kN3mvMagic) return false;
    if (hdr.version != kN3mvVersion) return false;
    if (hdr.width == 0 || hdr.height == 0) return false;
    if (hdr.frameRateMilli == 0) return false;

    mWidth              = hdr.width;
    mHeight             = hdr.height;
    mFrameRateMilli     = hdr.frameRateMilli;
    mEstimatedFrames    = hdr.estimatedFrames;
    mAudioSampleRate    = hdr.audioSampleRate;
    mAudioNumChannels   = hdr.audioNumChannels;
    mAudioBitsPerSample = hdr.audioBitsPerSample;
    mAudioByteSize      = hdr.audioByteSize;
    mVideoByteSize      = hdr.videoByteSize;
    mAudioBaseOffset    = uint32_t(sizeof(N3mvHeader));
    mVideoBaseOffset    = mAudioBaseOffset + mAudioByteSize;
    mAudioCursor        = 0;
    mVideoCursor        = 0;
    mNextFrame          = 0;

    return true;
}

bool MvdVideoDecoder::OpenMemory(const uint8_t* data, size_t size, const char* /*codecHint*/)
{
    Close();
    if (data == nullptr || size < sizeof(N3mvHeader)) return false;
    mSrc = data;
    mSrcSize = size;
    if (!ParseHeader()) { Close(); return false; }

#if defined(PLATFORM_3DS) && PLATFORM_3DS
    // TODO(3DS): mvdstdInit, configure for H.264 input, BGR565 or RGBA8 output,
    // Y2RU init if MVD doesn't produce RGB directly. Width/height from header.
    LogWarning("MvdVideoDecoder: PLATFORM_3DS path is scaffolded but not validated against hardware");
#else
    LogWarning("MvdVideoDecoder: not on PLATFORM_3DS — N3MV bytes load but won't decode (hardware path stubbed)");
#endif
    return true;
}

bool MvdVideoDecoder::Open(const char* path)
{
    Close();
    if (path == nullptr || path[0] == '\0') return false;
    FILE* f = fopen(path, "rb");
    if (f == nullptr)
    {
        LogError("MvdVideoDecoder::Open: fopen failed for '%s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < long(sizeof(N3mvHeader)))
    {
        fclose(f);
        return false;
    }
    mFile = f;
    mFileSize = size_t(sz);
    if (!ParseHeader()) { Close(); return false; }
    return true;
}

void MvdVideoDecoder::Close()
{
#if defined(PLATFORM_3DS) && PLATFORM_3DS
    // TODO(3DS): mvdstdExit, y2rExit, free any allocated linear buffers.
    if (mMvdContext != nullptr) { /* mvdstdExit(); */ mMvdContext = nullptr; }
    if (mY2rContext != nullptr) { /* y2rExit();   */ mY2rContext = nullptr; }
#endif
    if (mFile != nullptr) { fclose(static_cast<FILE*>(mFile)); mFile = nullptr; }
    mSrc = nullptr;
    mSrcSize = 0;
    mFileSize = 0;
    mWidth = mHeight = 0;
    mFrameRateMilli = 0;
    mEstimatedFrames = 0;
    mAudioSampleRate = 0;
    mAudioNumChannels = 0;
    mAudioBitsPerSample = 0;
    mAudioByteSize = 0;
    mVideoByteSize = 0;
    mAudioBaseOffset = 0;
    mVideoBaseOffset = 0;
    mAudioCursor = 0;
    mVideoCursor = 0;
    mNextFrame = 0;
    mFramePixels.clear();
    mFramePixels.shrink_to_fit();
}

bool MvdVideoDecoder::DecodeNextFrame(DecodedFrame& outFrame)
{
#if defined(PLATFORM_3DS) && PLATFORM_3DS
    // TODO(3DS): feed H.264 bytes from [mVideoBaseOffset + mVideoCursor, ...) to
    // mvdstdProcessVideoFrame in chunks until it signals "frame complete". Then
    // copy/convert the YUV output into mFramePixels (RGBA8) via Y2RU. Update
    // mVideoCursor with the number of bytes mvdstd consumed.
    //
    // Sketch:
    //   MVDSTD_Config cfg;
    //   mvdstdGenerateDefaultConfig(&cfg, mWidth, mHeight, mWidth, mHeight, ...);
    //   mvdstdProcessVideoFrame(input_ptr, input_size, 0, &out_consumed);
    //   if (Y2RU_FastYUV2RGB(...)) ...;
    //
    // See libctru's mvdstd.h and y2r.h for parameter details.
    outFrame = {};
    outFrame.endOfStream = (mNextFrame >= mEstimatedFrames);
    return outFrame.endOfStream; // signal "EOS" so playback ends gracefully on stub
#else
    // Non-3DS host: we have header + bytes loaded, but no decoder. Behave as if
    // we hit end-of-stream so the player ends rather than spinning.
    outFrame = {};
    outFrame.endOfStream = true;
    return true;
#endif
}

bool MvdVideoDecoder::Seek(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    uint32_t target = (mFrameRateMilli > 0)
        ? uint32_t(seconds * double(mFrameRateMilli) / 1000.0 + 0.5) : 0;
    if (mEstimatedFrames > 0 && target >= mEstimatedFrames) target = mEstimatedFrames - 1;
    mNextFrame = target;

    // Audio cursor by sample-rate math.
    if (HasAudio())
    {
        const uint32_t bytesPerFrame = mAudioNumChannels * (mAudioBitsPerSample / 8);
        const uint64_t targetSamples = uint64_t(seconds * double(mAudioSampleRate));
        uint32_t pos = uint32_t(targetSamples) * bytesPerFrame;
        if (pos > mAudioByteSize) pos = mAudioByteSize;
        if (bytesPerFrame > 0) pos -= (pos % bytesPerFrame);
        mAudioCursor = pos;
    }

    // Video cursor: H.264 doesn't allow byte-level seek without IDR-frame
    // index. TODO(3DS): build an IDR offset table at Open time so seek can jump
    // to the nearest keyframe. For v1 we reset to 0 (start) on any seek.
    mVideoCursor = 0;
    return true;
}

AudioStreamDesc MvdVideoDecoder::GetAudioDesc() const
{
    AudioStreamDesc d;
    d.sampleRate    = mAudioSampleRate;
    d.numChannels   = mAudioNumChannels;
    d.bitsPerSample = mAudioBitsPerSample;
    return d;
}

AudioDecodeResult MvdVideoDecoder::DecodeNextAudio(DecodedAudio& outChunk)
{
    if (!HasAudio() || mAudioCursor >= mAudioByteSize)
    {
        outChunk = {};
        outChunk.endOfStream = true;
        return AudioDecodeResult::EndOfStream;
    }
    // Memory mode is the only supported audio path right now (the 3DS hardware
    // path can read straight from mFile via fread once that's wired up).
    if (mSrc == nullptr)
    {
        outChunk = {};
        return AudioDecodeResult::NeedsMoreInput;
    }

    const uint32_t bytesPerFrame = mAudioNumChannels * (mAudioBitsPerSample / 8);
    const uint32_t chunkSize     = (mAudioSampleRate / 50) * bytesPerFrame; // ~20 ms
    const uint32_t remaining     = mAudioByteSize - mAudioCursor;
    uint32_t take = (remaining < chunkSize) ? remaining : chunkSize;
    if (bytesPerFrame > 0) take -= (take % bytesPerFrame);
    if (take == 0)
    {
        outChunk = {};
        outChunk.endOfStream = true;
        return AudioDecodeResult::EndOfStream;
    }

    outChunk.samples     = mSrc + mAudioBaseOffset + mAudioCursor;
    outChunk.byteSize    = take;
    outChunk.sampleCount = take / bytesPerFrame;
    outChunk.ptsSeconds  = (mAudioSampleRate > 0)
        ? double(mAudioCursor / bytesPerFrame) / double(mAudioSampleRate) : 0.0;
    outChunk.endOfStream = false;

    mAudioCursor += take;
    return AudioDecodeResult::Produced;
}

} // namespace VideoPlayerAddon
