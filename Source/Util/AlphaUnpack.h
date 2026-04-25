#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VideoPlayerAddon
{

// Unpack a top/bottom-split-alpha RGBA8 frame into a half-height RGBA8 buffer.
// Input: src is width * srcHeight * 4 bytes, RGBA row-major. srcHeight must be even.
//        Top half holds the RGB image; bottom half's R channel holds alpha.
// Output: out is resized to width * (srcHeight / 2) * 4 bytes.
// Out RGB comes from the top half; out alpha from bottom half's R channel.
void UnpackTopBottomAlpha(const uint8_t* src,
                          uint32_t width,
                          uint32_t srcHeight,
                          std::vector<uint8_t>& out);

} // namespace VideoPlayerAddon
