#include "Backends/VideoDecoderFactory.h"

#include "Backends/IVideoDecoder.h"
#include "Backends/NullVideoDecoder.h"

#if POLYPHASE_WITH_FFMPEG
#include "Backends/FFmpegVideoDecoder.h"
#endif

#include <algorithm>
#include <cctype>

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

} // namespace VideoPlayerAddon
