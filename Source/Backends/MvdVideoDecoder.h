#pragma once

#include "Backends/IVideoDecoder.h"

#include <cstdint>
#include <vector>

namespace VideoPlayerAddon
{

// Runtime decoder for the N3MV container produced by CookN3dsMvdVideo. Wraps
// libctru's mvdstd hardware H.264 decoder + Y2RU YUV->RGB conversion.
//
// IMPORTANT: this class is SCAFFOLDING ONLY. The PLATFORM_3DS-gated decode path
// has been written against libctru's documented API surface but cannot be
// validated without 3DS hardware (or Citra emulator with MVD support). On non-
// 3DS platforms, OpenMemory always returns false (so the factory falls through
// to NullVideoDecoder); on 3DS, expect to iterate on:
//   - mvdstdInit / mvdstdGenerateDefaultConfig parameter tuning
//   - Y2RU service init + format conversion
//   - Frame timing / event signaling
//
// File-streaming Open(path) IS implemented (lazy fread) so the same code path
// works for both packaged-build sidecars and editor-test cooks.
class MvdVideoDecoder : public IVideoDecoder
{
public:
    MvdVideoDecoder() = default;
    ~MvdVideoDecoder() override;

    bool Open(const char* path) override;
    bool OpenMemory(const uint8_t* data, size_t size, const char* codecHint) override;
    void Close() override;

    VideoFrameDesc GetFrameDesc() const override { return {mWidth, mHeight}; }
    double GetDurationSeconds() const override
    {
        if (mFrameRateMilli == 0 || mEstimatedFrames == 0) return 0.0;
        return double(mEstimatedFrames) * 1000.0 / double(mFrameRateMilli);
    }
    double GetFrameRate() const override
    {
        return (mFrameRateMilli == 0) ? 0.0 : double(mFrameRateMilli) / 1000.0;
    }

    bool DecodeNextFrame(DecodedFrame& outFrame) override;
    bool Seek(double seconds) override;

    bool HasAudio() const override { return mAudioByteSize > 0 && mAudioSampleRate > 0; }
    AudioStreamDesc GetAudioDesc() const override;
    AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk) override;

private:
    bool ParseHeader();

    uint32_t mWidth          = 0;
    uint32_t mHeight         = 0;
    uint32_t mFrameRateMilli = 0;
    uint32_t mEstimatedFrames = 0;
    uint32_t mAudioSampleRate = 0;
    uint32_t mAudioNumChannels = 0;
    uint32_t mAudioBitsPerSample = 0;
    uint32_t mAudioByteSize = 0;
    uint32_t mVideoByteSize = 0;
    uint32_t mAudioBaseOffset = 0;
    uint32_t mVideoBaseOffset = 0;

    // Audio cursor (bytes from audio base).
    uint32_t mAudioCursor = 0;
    // Video cursor (bytes from video base — the H.264 Annex-B stream).
    uint32_t mVideoCursor = 0;
    uint32_t mNextFrame   = 0;

    // Memory mode (PC test cook).
    const uint8_t* mSrc = nullptr;
    size_t mSrcSize = 0;
    // Streaming mode (cooked builds; sidecar file).
    void* mFile = nullptr;
    size_t mFileSize = 0;

    // Decoded RGBA8 output buffer (one frame).
    std::vector<uint8_t> mFramePixels;

    // libctru handles, only allocated on 3DS. void* placeholders so the header
    // doesn't need to include <3ds.h> on non-3DS builds.
#if defined(PLATFORM_3DS) && PLATFORM_3DS
    void* mMvdContext = nullptr;
    void* mY2rContext = nullptr;
#endif
};

} // namespace VideoPlayerAddon
