#include "Backends/VideoDecoderFactory.h"

#include "Backends/IVideoDecoder.h"
#include "Backends/NullVideoDecoder.h"
#include "Backends/PcvVideoDecoder.h"
#include "Backends/ThpVideoDecoder.h"
#include "Backends/MvdVideoDecoder.h"
#include "Cook/DolphinVideoCook.h" // kPcvMagic
#include "Cook/N3dsMvdCook.h"      // kN3mvMagic
#include "Assets/VideoClip.h"

#if POLYPHASE_WITH_FFMPEG
#include "Backends/FFmpegVideoDecoder.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstring>

namespace VideoPlayerAddon
{

namespace
{
std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::string GetExtension(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    return ToLower(path.substr(dot));
}
} // namespace

std::unique_ptr<IVideoDecoder> CreateVideoDecoder(const std::string& path)
{
    const std::string ext = GetExtension(path);

    // Null backend: explicit sentinel path ("null://...") or empty path for tests.
    if (path.rfind("null://", 0) == 0 || path.empty())
    {
        return std::unique_ptr<IVideoDecoder>(new NullVideoDecoder());
    }

#if POLYPHASE_WITH_FFMPEG
    if (ext == ".mp4" || ext == ".mov" || ext == ".webm" ||
        ext == ".mkv" || ext == ".avi" || ext == ".m4v")
    {
        return std::unique_ptr<IVideoDecoder>(new FFmpegVideoDecoder());
    }
#endif

    // Unknown extension: fall back to null so the addon at least loads cleanly
    // during development when FFmpeg binaries aren't present yet.
    return std::unique_ptr<IVideoDecoder>(new NullVideoDecoder());
}

std::unique_ptr<IVideoDecoder> CreateVideoDecoderForClip(const VideoClip* clip)
{
    if (clip == nullptr || clip->GetSourceSize() == 0)
    {
        return std::unique_ptr<IVideoDecoder>(new NullVideoDecoder());
    }

    // Cooked-payload detection: GameCube/Wii/3DS VideoClips ship a cooked container
    // instead of the original source. Sniff the first 4 bytes for known magics and
    // route to the matching software decoder. Works on every platform (handy for
    // testing cooks on PC before packaging for hardware).
    const auto& bytes = clip->GetSourceData();
    if (bytes.size() >= 4)
    {
        uint32_t magic = 0;
        memcpy(&magic, bytes.data(), 4);
        if (magic == kPcvMagic)
        {
            return std::unique_ptr<IVideoDecoder>(new PcvVideoDecoder());
        }
        // THP: magic is "THP\0" stored as the literal four ASCII bytes (NOT a host
        // uint32 swap from a wire BE value — the file's first four bytes are
        // 0x54 0x48 0x50 0x00 regardless of host endianness, since they're
        // characters not a number).
        if (bytes[0] == 'T' && bytes[1] == 'H' && bytes[2] == 'P' && bytes[3] == '\0')
        {
            return std::unique_ptr<IVideoDecoder>(new ThpVideoDecoder());
        }
        // N3MV (3DS hardware H.264): magic = "N3MV" little-endian uint32.
        if (magic == kN3mvMagic)
        {
            return std::unique_ptr<IVideoDecoder>(new MvdVideoDecoder());
        }
    }

#if POLYPHASE_WITH_FFMPEG
    // Codec hint is captured at import as a lower-case extension (no dot). For PC
    // we treat any of the known container hints as a green light for the FFmpeg
    // backend; consoles use the PCV1 path above.
    const std::string& hint = clip->GetCodecHint();
    if (hint == "mp4" || hint == "mov" || hint == "webm" ||
        hint == "mkv" || hint == "avi" || hint == "m4v" ||
        hint.empty()) // empty hint: let FFmpeg probe
    {
        return std::unique_ptr<IVideoDecoder>(new FFmpegVideoDecoder());
    }
#endif

    return std::unique_ptr<IVideoDecoder>(new NullVideoDecoder());
}

} // namespace VideoPlayerAddon
