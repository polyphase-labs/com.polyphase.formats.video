#pragma once

// Editor-side cook pipeline for software-decode console targets (GameCube, Wii,
// and 3DS). Takes a source video file (any container FFmpeg can read) and
// produces a small custom container the runtime PcvVideoDecoder reads back:
// header + raw PCM audio + per-frame MJPEG payload. The on-disk format is opaque
// to anything outside this addon. Named "Dolphin" for historical reasons; same
// PCV1 bytes work on any platform whose runtime can software-decode JPEG. 3DS
// will eventually also get a hardware-H.264 path via MVD; until then this MJPEG
// path is the working baseline.
//
// Layout (all little-endian):
//
//   PcvHeader (40 bytes):
//     char     magic[4]            = "PCV1"
//     uint32_t version             = 1
//     uint32_t width
//     uint32_t height
//     uint32_t numFrames
//     uint32_t frameRateMilli      = fps * 1000  (e.g. 30000 = 30.000 fps)
//     uint32_t audioSampleRate     (0 = no audio)
//     uint16_t audioNumChannels    (0 = no audio)
//     uint16_t audioBitsPerSample
//     uint32_t audioByteSize
//     uint32_t framesByteSize
//
//   uint8_t  audioData[audioByteSize]                  // 16-bit signed PCM, interleaved
//   for i in [0, numFrames):
//     uint32_t frameByteSize
//     uint8_t  jpegBytes[frameByteSize]                // baseline JFIF JPEG

#include <cstdint>
#include <string>
#include <vector>

enum class Platform : int;

namespace VideoPlayerAddon
{

constexpr uint32_t kPcvMagic   = 0x31564350u; // 'P','C','V','1' little-endian
constexpr uint32_t kPcvVersion = 1u;

#pragma pack(push, 1)
struct PcvHeader
{
    uint32_t magic;              // kPcvMagic
    uint32_t version;            // kPcvVersion
    uint32_t width;
    uint32_t height;
    uint32_t numFrames;
    uint32_t frameRateMilli;
    uint32_t audioSampleRate;
    uint16_t audioNumChannels;
    uint16_t audioBitsPerSample;
    uint32_t audioByteSize;
    uint32_t framesByteSize;
};
#pragma pack(pop)

static_assert(sizeof(PcvHeader) == 40, "PcvHeader must be 40 bytes (on-disk wire format)");

// Quality / size knobs for the Dolphin cook. Defaults target a 16:9 letterbox at
// 30 fps and ~88 KB/sec audio — fine for a few-second attract clip but big for a
// 30s clip on a GameCube (24 MB total RAM). Promote to per-clip values via the
// VideoClip inspector and tune down for longer / less critical clips. JPEG quality
// uses ffmpeg's qscale:v scale where 2 = best, 31 = worst (lower number = bigger
// file but better picture).
struct DolphinCookParams
{
    uint32_t width            = 512;
    uint32_t height           = 288;
    uint32_t fps              = 30;
    uint32_t jpegQuality      = 3;
    uint32_t audioSampleRate  = 22050;
    uint32_t audioNumChannels = 2;
};

// Cook a source video into a PCV1 byte blob suitable for storing inside a VideoClip
// .oct on the Dolphin platform. Returns false on failure with an explanation in
// outError. Editor-only; never called from the runtime.
//
// sourceBytes: full source-container bytes (mp4/webm/etc.)
// codecHint:   lower-case extension used to pick the temp file's extension so FFmpeg
//              can pick the right demuxer ("mp4", "webm", "mov", ...). May be empty.
// platform:    Platform::GameCube or Platform::Wii.
// params:      per-clip cook knobs (see DolphinCookParams).
bool CookDolphinVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError);

} // namespace VideoPlayerAddon
