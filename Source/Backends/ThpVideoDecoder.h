#pragma once

#include "Backends/IVideoDecoder.h"
#include "Cook/DspAdpcm.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace VideoPlayerAddon
{

// Software demuxer + decoder for Nintendo's THP container. MJPEG video frames
// (decoded via stb_image_jpeg, same as PcvVideoDecoder) plus per-channel DSP-ADPCM
// audio (decoded into 16-bit PCM and chunked for streaming).
//
// Bytes are big-endian on the wire (PowerPC native); we swap on read.
//
// Cross-platform — same TU compiles on PC for testing and on Dolphin/Wii in
// cooked builds. The factory routes here when clip bytes start with "THP\0".
class ThpVideoDecoder : public IVideoDecoder
{
public:
    ThpVideoDecoder() = default;
    ~ThpVideoDecoder() override;

    bool Open(const char* path) override;
    bool OpenMemory(const uint8_t* data, size_t size, const char* codecHint) override;
    void Close() override;

    VideoFrameDesc GetFrameDesc() const override { return {mWidth, mHeight}; }
    double GetDurationSeconds() const override
    {
        if (mFps <= 0.0f || mNumFrames == 0) return 0.0;
        return double(mNumFrames) / double(mFps);
    }
    double GetFrameRate() const override { return double(mFps); }

    bool DecodeNextFrame(DecodedFrame& outFrame) override;
    bool Seek(double seconds) override;

    bool HasAudio() const override { return mAudioNumChannels > 0 && mAudioSampleRate > 0; }
    AudioStreamDesc GetAudioDesc() const override;
    AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk) override;

private:
    struct FrameRecord
    {
        size_t frameStart;       // absolute byte offset of this frame's nextFrameSize header
        uint32_t videoOffset;    // absolute byte offset of the JPEG bytes (NOT the videoSize header)
        uint32_t videoSize;      // bytes
        // Per-channel ADPCM slice for this frame. Each entry points into mSrc.
        // Empty when the audio component is absent.
        struct ChannelSlice { uint32_t offset; uint32_t size; };
        std::vector<ChannelSlice> audio;
        uint32_t audioSamples;   // PCM samples this frame contributes per channel
    };

    bool ParseHeaderAndComponents();
    bool BuildFrameTable();

    // Generic read at an absolute byte offset in the underlying source. Routes to
    // memcpy from mSrc (memory mode) or fseek+fread on mFile (streaming mode).
    bool ReadAt(size_t offset, size_t size, void* dst);
    uint32_t ReadBE32At(size_t offset);
    float ReadBEFloatAt(size_t offset);

    // Source: exactly one of these is set after a successful Open.
    const uint8_t* mSrc      = nullptr;   // memory mode: borrowed pointer
    size_t         mSrcSize  = 0;
    FILE*          mFile     = nullptr;   // streaming mode: open handle, owned
    size_t         mFileSize = 0;

    // Scratch buffers reused across frames so we don't re-alloc per Decode call.
    std::vector<uint8_t> mFrameReadBuf;
    std::vector<uint8_t> mAudioReadBuf;

    uint32_t mWidth         = 0;
    uint32_t mHeight        = 0;
    float    mFps           = 0.0f;
    uint32_t mNumFrames     = 0;
    uint32_t mFirstFrameOffset = 0;

    // Audio component (single component supported).
    uint32_t mAudioNumChannels = 0;
    uint32_t mAudioSampleRate  = 0;
    uint32_t mAudioTotalSamples = 0;

    std::vector<FrameRecord> mFrames;

    // Per-channel decode state. Each frame's audio block carries its own header
    // (so seeking is well-defined), but we cache the most recent header here so
    // DspDecode can be called per-frame without re-parsing.
    std::vector<DspChannelHeader> mChCache;

    // Running per-channel ADPCM history. The encoder emits one continuous stream
    // where each 8-byte block's prediction depends on the previous block's
    // DECODED yn1/yn2. Resetting these per frame produces audible popping every
    // frame boundary; instead we carry them forward from the previous frame's
    // last sample. Reset to mChCache.yn1/yn2 (typically 0) on Seek and Open.
    std::vector<int16_t> mChYn1;
    std::vector<int16_t> mChYn2;

    uint32_t mNextFrame = 0;
    uint32_t mAudioFrameCursor = 0; // tracks which video-frame's audio slice to emit next

    uint8_t* mFramePixels = nullptr;
    std::vector<int16_t> mAudioPcmScratch; // interleaved decoded PCM for current frame
};

} // namespace VideoPlayerAddon
