#include "Util/AlphaUnpack.h"

namespace VideoPlayerAddon
{

void UnpackTopBottomAlpha(const uint8_t* src,
                          uint32_t width,
                          uint32_t srcHeight,
                          std::vector<uint8_t>& out)
{
    if (src == nullptr || width == 0 || srcHeight < 2)
    {
        out.clear();
        return;
    }

    const uint32_t outHeight = srcHeight / 2;
    const size_t outBytes = size_t(width) * outHeight * 4;
    if (out.size() != outBytes)
    {
        out.resize(outBytes);
    }

    const uint8_t* top = src;
    const uint8_t* bot = src + size_t(width) * outHeight * 4;
    uint8_t* dst = out.data();

    const size_t pixels = size_t(width) * outHeight;
    for (size_t i = 0; i < pixels; ++i)
    {
        dst[0] = top[0];
        dst[1] = top[1];
        dst[2] = top[2];
        dst[3] = bot[0]; // alpha from bottom-half R channel
        dst += 4;
        top += 4;
        bot += 4;
    }
}

} // namespace VideoPlayerAddon
