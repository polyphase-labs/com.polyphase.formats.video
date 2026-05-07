#pragma once

// Editor-side cook to Nintendo's THP container — the native GameCube/Wii video
// format. Video is Motion-JPEG (one JPEG per frame, no inter-frame prediction);
// audio is DSP-ADPCM. Same MJPEG decode path as PCV1, but the container layout
// is correct against published THP specs and (in theory) playable by any THP
// decoder including Dolphin emulator's debug tools.
//
// Caveat: validating THP byte-correctness requires running the cooked file in
// Dolphin emulator or on real hardware. The implementation here follows the
// published spec but has not been verified end-to-end. If a cooked clip won't
// play, expect to iterate on header field interpretations. Common gotchas:
// - All multi-byte fields are big-endian (PowerPC native).
// - frame size fields refer to the WHOLE frame (incl. videoSize/audioSize hdrs).
// - DSP-ADPCM audio header fields are themselves big-endian inside the BE THP.
// - The "first frame" offset in the THP header is from the start of the file.
//
// Layout (big-endian):
//
//   ThpFileHeader (0x40 bytes)
//   ThpComponentTable (0x14 bytes: numComponents + componentTypes[16] + 4 pad)
//   For each component:
//     Video: ThpVideoInfo (8 bytes: width, height)
//     Audio: ThpAudioInfo (12 bytes: numChannels, freq, numSamples)
//   For each frame:
//     uint32_t nextFrameSize     // byte size of the NEXT frame's payload (0 on last)
//     uint32_t prevFrameSize     // byte size of the PREVIOUS frame's payload
//     uint32_t videoSize         // bytes
//     uint8_t  videoData[videoSize]   // MJPEG (JFIF baseline JPEG)
//     [if audio component present]
//     uint32_t audioSize         // bytes (= 8 + sum of per-channel ADPCM bytes)
//     uint32_t numSamplesInFrame
//     uint8_t  audioData[audioSize - 8]   // per-channel: DSP header + ADPCM bytes
//
// The ThpFileHeader's "data offset" points at the first-frame block. The "offsets
// table" is optional and we omit it (set offsetsDataOffset = 0). Players that need
// to seek will scan frame-by-frame; for sequential playback that's fine.

#include <cstdint>
#include <string>
#include <vector>

#include "Cook/DolphinVideoCook.h" // reuse DolphinCookParams

enum class Platform : int;

namespace VideoPlayerAddon
{

constexpr uint32_t kThpMagic = 0x54485000u; // 'T','H','P','\0' big-endian on the wire,
                                            // but stored as host-order uint32 for memcmp.

bool CookThpVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError);

} // namespace VideoPlayerAddon
