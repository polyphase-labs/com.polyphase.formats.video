#pragma once

#include "Backends/IVideoDecoder.h"
#include "Cook/DolphinVideoCook.h" // PcvHeader / kPcvMagic

#include <cstdint>
#include <vector>

namespace VideoPlayerAddon
{

// Runtime decoder for the PCV1 container produced by CookDolphinVideo. Pure software
// MJPEG via stb_image_jpeg + raw 16-bit PCM passthrough. Compiled on every platform —
// the factory routes here whenever a VideoClip's payload starts with the PCV1 magic
// (so the cooked bytes work on PC for testing, on GameCube/Wii in cooked builds, and
// on any future platform that uses the same cook).
class PcvVideoDecoder : public IVideoDecoder
{
public:
    PcvVideoDecoder() = default;
    ~PcvVideoDecoder() override;

    bool Open(const char* path) override;
    bool OpenMemory(const uint8_t* data, size_t size, const char* codecHint) override;
    void Close() override;

    VideoFrameDesc GetFrameDesc() const override { return {mHeader.width, mHeader.height}; }
    double GetDurationSeconds() const override
    {
        if (mHeader.frameRateMilli == 0 || mHeader.numFrames == 0) return 0.0;
        return double(mHeader.numFrames) * 1000.0 / double(mHeader.frameRateMilli);
    }
    double GetFrameRate() const override
    {
        return (mHeader.frameRateMilli == 0) ? 0.0 : double(mHeader.frameRateMilli) / 1000.0;
    }

    bool DecodeNextFrame(DecodedFrame& outFrame) override;
    bool Seek(double seconds) override;

    bool HasAudio() const override            { return mHeader.audioByteSize > 0 && mHeader.audioSampleRate > 0; }
    AudioStreamDesc GetAudioDesc() const override;
    AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk) override;

private:
    void ResetState();

    // Caller-owned source buffer. We never copy or take ownership; OpenMemory's
    // contract is that the buffer remains valid until Close(), so we just keep the
    // pointer and indices into it.
    const uint8_t* mSrc      = nullptr;
    size_t         mSrcSize  = 0;

    PcvHeader      mHeader   = {};

    // Cumulative byte offsets into mSrc for each frame's MJPEG payload, plus the
    // payload size at index i. Built once in OpenMemory by walking the per-frame
    // section so Seek can jump directly to any frame without re-scanning.
    std::vector<uint32_t> mFrameOffsets; // size = numFrames; mSrc + mFrameOffsets[i] points at jpegBytes
    std::vector<uint32_t> mFrameSizes;   // size = numFrames; bytes of the JPEG at frame i

    // Cursor for sequential DecodeNextFrame calls. Seek resets it.
    uint32_t mNextFrame = 0;

    // Audio cursor (bytes into mSrc + audio section start).
    size_t mAudioBase = 0;     // absolute offset of audio section in mSrc
    size_t mAudioPos  = 0;     // next byte to emit (offset from mAudioBase)
    // Bytes the audio chunker hands to the caller per call. ~20 ms of stereo 16-bit
    // PCM at 22.05 kHz is ~1764 bytes — round to a frame-aligned size.
    uint32_t mAudioChunkBytes = 0;

    // Output pixel buffer. stbi_load_from_memory mallocs its own buffer; we hold it
    // here so the caller-returned pointer is stable until the next DecodeNextFrame
    // (matching the IVideoDecoder contract).
    uint8_t* mFramePixels = nullptr;
};

} // namespace VideoPlayerAddon
