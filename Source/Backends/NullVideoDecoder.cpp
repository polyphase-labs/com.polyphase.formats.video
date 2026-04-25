#include "Backends/NullVideoDecoder.h"

#include <algorithm>
#include <cmath>

namespace VideoPlayerAddon
{

NullVideoDecoder::NullVideoDecoder() = default;
NullVideoDecoder::~NullVideoDecoder() { Close(); }

bool NullVideoDecoder::Open(const char* /*path*/)
{
    mPixels.assign(size_t(mWidth) * mHeight * 4, 0);
    mFrameIndex = 0;
    mTotalFrames = uint32_t(mDurationSec * mFrameRate);
    return true;
}

void NullVideoDecoder::Close()
{
    mPixels.clear();
    mPixels.shrink_to_fit();
    mFrameIndex = 0;
    mTotalFrames = 0;
}

bool NullVideoDecoder::DecodeNextFrame(DecodedFrame& outFrame)
{
    if (mFrameIndex >= mTotalFrames)
    {
        outFrame.pixels = nullptr;
        outFrame.byteSize = 0;
        outFrame.ptsSeconds = mDurationSec;
        outFrame.endOfStream = true;
        return true;
    }

    RenderFrame(mFrameIndex);
    outFrame.pixels = mPixels.data();
    outFrame.byteSize = mPixels.size();
    outFrame.ptsSeconds = double(mFrameIndex) / mFrameRate;
    outFrame.endOfStream = false;
    ++mFrameIndex;
    return true;
}

bool NullVideoDecoder::Seek(double seconds)
{
    double clamped = std::max(0.0, std::min(seconds, mDurationSec));
    mFrameIndex = uint32_t(clamped * mFrameRate);
    return true;
}

void NullVideoDecoder::RenderFrame(uint32_t frameIndex)
{
    // 8-bar SMPTE-ish pattern with a scrolling band to prove per-frame updates happen.
    static const uint8_t kBars[8][3] = {
        {192, 192, 192}, // gray
        {192, 192,   0}, // yellow
        {  0, 192, 192}, // cyan
        {  0, 192,   0}, // green
        {192,   0, 192}, // magenta
        {192,   0,   0}, // red
        {  0,   0, 192}, // blue
        { 16,  16,  16}, // near-black
    };

    const uint32_t bandHeight = std::max<uint32_t>(1, mHeight / 16);
    const uint32_t bandY = (frameIndex * 4) % mHeight;

    uint8_t* row = mPixels.data();
    for (uint32_t y = 0; y < mHeight; ++y)
    {
        const bool inBand = (y >= bandY && y < bandY + bandHeight);
        for (uint32_t x = 0; x < mWidth; ++x)
        {
            uint32_t bar = (x * 8u) / mWidth;
            if (bar > 7) bar = 7;
            uint8_t r = kBars[bar][0];
            uint8_t g = kBars[bar][1];
            uint8_t b = kBars[bar][2];
            if (inBand)
            {
                r = uint8_t(255 - r);
                g = uint8_t(255 - g);
                b = uint8_t(255 - b);
            }
            row[0] = r;
            row[1] = g;
            row[2] = b;
            row[3] = 255;
            row += 4;
        }
    }
}

} // namespace VideoPlayerAddon
