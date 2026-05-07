#pragma once

// Editor-side cook for the Nintendo 3DS MVD hardware H.264 path. Produces a small
// custom container ("N3MV"): header + raw 16-bit PCM audio + Annex-B-formatted
// H.264 stream. Runtime decoder uses libctru's mvdstd service to hardware-decode
// and Y2RU to convert YUV->RGB. Hardware-only path; runtime is gated on
// PLATFORM_3DS and untested without 3DS hardware / Citra. The cook side IS
// testable on PC: produces a valid .h264 stream you can play in VLC.
//
// Layout (little-endian):
//
//   N3mvHeader (40 bytes):
//     char     magic[4]            = "N3MV"
//     uint32_t version             = 1
//     uint32_t width
//     uint32_t height
//     uint32_t frameRateMilli      = fps * 1000
//     uint32_t estimatedFrames
//     uint32_t audioSampleRate     (0 = no audio)
//     uint16_t audioNumChannels
//     uint16_t audioBitsPerSample
//     uint32_t audioByteSize
//     uint32_t videoByteSize
//
//   uint8_t audio[audioByteSize]            // s16le interleaved PCM
//   uint8_t video[videoByteSize]            // H.264 Annex-B (start-code-prefixed NAL units)

#include <cstdint>
#include <string>
#include <vector>

#include "Cook/DolphinVideoCook.h" // reuse DolphinCookParams

enum class Platform : int;

namespace VideoPlayerAddon
{

constexpr uint32_t kN3mvMagic   = 0x564D334Eu; // 'N','3','M','V' little-endian
constexpr uint32_t kN3mvVersion = 1u;

#pragma pack(push, 1)
struct N3mvHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t frameRateMilli;
    uint32_t estimatedFrames;
    uint32_t audioSampleRate;
    uint16_t audioNumChannels;
    uint16_t audioBitsPerSample;
    uint32_t audioByteSize;
    uint32_t videoByteSize;
};
#pragma pack(pop)

static_assert(sizeof(N3mvHeader) == 40, "N3mvHeader must be 40 bytes");

bool CookN3dsMvdVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError);

} // namespace VideoPlayerAddon
