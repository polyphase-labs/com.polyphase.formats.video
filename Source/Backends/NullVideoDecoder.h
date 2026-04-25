#pragma once

#include "Backends/IVideoDecoder.h"

#include <string>
#include <vector>

namespace VideoPlayerAddon
{

// Test backend that emits animated SMPTE-ish color bars at a synthetic framerate.
// Used to validate the texture streaming path end-to-end without any codec dependency.
class NullVideoDecoder : public IVideoDecoder
{
public:
    NullVideoDecoder();
    ~NullVideoDecoder() override;

    bool Open(const char* path) override;
    void Close() override;

    VideoFrameDesc GetFrameDesc() const override { return {mWidth, mHeight}; }
    double GetDurationSeconds() const override { return mDurationSec; }
    double GetFrameRate() const override { return mFrameRate; }

    bool DecodeNextFrame(DecodedFrame& outFrame) override;
    bool Seek(double seconds) override;

private:
    void RenderFrame(uint32_t frameIndex);

    uint32_t mWidth = 256;
    uint32_t mHeight = 256;
    double mFrameRate = 30.0;
    double mDurationSec = 5.0;

    std::vector<uint8_t> mPixels;
    uint32_t mFrameIndex = 0;
    uint32_t mTotalFrames = 0;
};

} // namespace VideoPlayerAddon
