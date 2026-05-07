#include "Cook/N3dsMvdCook.h"

#if EDITOR

#include "Engine.h"
#include "EngineTypes.h"
#include "Log.h"
#include "Stream.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
    #define ADDON_POPEN  _popen
    #define ADDON_PCLOSE _pclose
#else
    #include <sys/wait.h>
    #define ADDON_POPEN  popen
    #define ADDON_PCLOSE pclose
#endif

namespace VideoPlayerAddon
{

namespace fs = std::filesystem;

namespace
{
    std::string GetFFmpegBin()
    {
        const char* env = std::getenv("POLYPHASE_FFMPEG");
        if (env != nullptr && env[0] != '\0') return env;
        return "ffmpeg";
    }

    std::string CookTempDir()
    {
        std::string root = GetEngineState()->mProjectDirectory + "Intermediate/VideoClipCook";
        std::error_code ec;
        fs::create_directories(root, ec);
        return root;
    }

    std::string Quote(const std::string& s) { return "\"" + s + "\""; }

    bool RunCommand(const std::string& cmd, std::string& outError, bool streamProgress = false)
    {
#if defined(_WIN32)
        const std::string redirected = "\"" + cmd + " 2>&1\"";
#else
        const std::string redirected = cmd + " 2>&1";
#endif
        FILE* pipe = ADDON_POPEN(redirected.c_str(), "r");
        if (pipe == nullptr) { outError = "popen failed"; return false; }

        std::string captured, lineBuf;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe) != nullptr)
        {
            captured.append(buf);
            if (streamProgress)
            {
                lineBuf.append(buf);
                size_t nl;
                while ((nl = lineBuf.find('\n')) != std::string::npos)
                {
                    std::string line = lineBuf.substr(0, nl);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) LogDebug("ffmpeg: %s", line.c_str());
                    lineBuf.erase(0, nl + 1);
                }
            }
            if (captured.size() > 64 * 1024) break;
        }
        if (streamProgress && !lineBuf.empty()) LogDebug("ffmpeg: %s", lineBuf.c_str());

        int rc = ADDON_PCLOSE(pipe);
#if !defined(_WIN32)
        if (rc != -1) rc = WEXITSTATUS(rc);
#endif
        if (rc != 0)
        {
            outError = "ffmpeg failed (exitCode=" + std::to_string(rc) + ")";
            if (!captured.empty())
            {
                std::string tail = captured.size() > 1024 ? captured.substr(captured.size() - 1024) : captured;
                outError += "\n" + tail;
            }
            return false;
        }
        return true;
    }

    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) return false;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); return false; }
        out.resize(size_t(sz));
        size_t got = (sz > 0) ? fread(out.data(), 1, size_t(sz), f) : 0;
        fclose(f);
        return got == size_t(sz);
    }
} // namespace

bool CookN3dsMvdVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError)
{
    if (platform != Platform::N3DS)
    {
        outError = "CookN3dsMvdVideo: wrong platform (3DS-only)";
        return false;
    }
    if (sourceBytes.empty()) { outError = "empty source"; return false; }

    // 3DS MVD limitations: H.264 baseline profile only, level 3.0 max (so up to
    // 720x480 / 1.6 Mbps roughly). Top screen is 400x240; 320x180 is a sensible
    // default. We clamp to MVD-supported ranges.
    //
    // Width and height MUST be even — x264 requires it for 4:2:0 chroma
    // subsampling and rejects odd dimensions outright. The MJPEG cooks tolerate
    // odd values, so a preset like Tiny (240x135) works there but fails here.
    // Round down to even so any preset is valid; the user loses at most one row
    // / column, which is invisible at console resolutions.
    auto evenDown = [](uint32_t v) { return v & ~1u; };
    uint32_t cookWidth      = (params.width   > 0 && params.width   <= 720) ? params.width  : 320u;
    uint32_t cookHeight     = (params.height  > 0 && params.height  <= 480) ? params.height : 180u;
    cookWidth  = evenDown(cookWidth);
    cookHeight = evenDown(cookHeight);
    if (cookWidth  == 0) cookWidth  = 2;
    if (cookHeight == 0) cookHeight = 2;
    const uint32_t cookFps        = (params.fps     > 0 && params.fps     <= 30)  ? params.fps    : 24u;
    const uint32_t cookAudioRate  = (params.audioSampleRate >= 8000 && params.audioSampleRate <= 48000)
                                       ? params.audioSampleRate : 22050u;
    const uint32_t cookAudioCh    = (params.audioNumChannels == 1 || params.audioNumChannels == 2)
                                       ? params.audioNumChannels : 2u;
    constexpr uint32_t kAudioBits = 16;

    const std::string tempDir   = CookTempDir();
    const std::string ext       = codecHint.empty() ? std::string("bin") : codecHint;
    const std::string srcPath   = tempDir + "/source." + ext;
    const std::string h264Path  = tempDir + "/video.h264";
    const std::string audioPath = tempDir + "/audio.pcm";

    {
        std::error_code ec;
        fs::remove(h264Path, ec);
        fs::remove(audioPath, ec);
        Stream s((const char*)sourceBytes.data(), uint32_t(sourceBytes.size()));
        s.WriteFile(srcPath.c_str());
    }

    const std::string ffmpeg = GetFFmpegBin();

    // ---- Pass 1: H.264 baseline Annex-B stream. ----
    {
        std::string cmd;
        cmd += Quote(ffmpeg);
        cmd += " -y -hide_banner -loglevel info -stats";
        cmd += " -i " + Quote(srcPath);
        cmd += " -an";
        cmd += " -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p";
        cmd += " -vf \"scale=" + std::to_string(cookWidth) + ":" + std::to_string(cookHeight) + ":flags=lanczos\"";
        cmd += " -r " + std::to_string(cookFps);
        cmd += " -bsf:v h264_mp4toannexb -f h264";
        cmd += " " + Quote(h264Path);

        LogDebug("CookN3dsMvdVideo: encoding H.264 baseline at %ux%u @ %u fps...",
                 cookWidth, cookHeight, cookFps);
        if (!RunCommand(cmd, outError, /*streamProgress=*/true)) return false;
    }

    // ---- Pass 2: PCM audio. ----
    bool hasAudio = false;
    {
        std::string cmd;
        cmd += Quote(ffmpeg);
        cmd += " -y -hide_banner -loglevel info -stats";
        cmd += " -i " + Quote(srcPath);
        cmd += " -vn";
        cmd += " -ac " + std::to_string(cookAudioCh);
        cmd += " -ar " + std::to_string(cookAudioRate);
        cmd += " -f s16le";
        cmd += " " + Quote(audioPath);

        std::string ignore;
        RunCommand(cmd, ignore, /*streamProgress=*/true);
        std::error_code ec;
        hasAudio = fs::is_regular_file(audioPath, ec);
    }

    // ---- Read both products. ----
    std::vector<uint8_t> h264Bytes, audioBytes;
    if (!ReadFileBytes(h264Path, h264Bytes))
    {
        outError = "CookN3dsMvdVideo: failed to read intermediate H.264 stream";
        return false;
    }
    if (hasAudio)
    {
        ReadFileBytes(audioPath, audioBytes);
    }

    // Estimate frame count from source duration. Without a per-frame index in
    // the container, this is just metadata for the runtime to pre-size buffers.
    const uint32_t bytesPerPcmFrame = cookAudioCh * (kAudioBits / 8);
    const uint32_t totalAudioFrames = hasAudio ? uint32_t(audioBytes.size() / bytesPerPcmFrame) : 0;
    const uint32_t estimatedFrames  = (cookFps > 0 && totalAudioFrames > 0 && cookAudioRate > 0)
        ? uint32_t((uint64_t(totalAudioFrames) * cookFps + cookAudioRate - 1) / cookAudioRate)
        : 0;

    // ---- Mux. ----
    N3mvHeader hdr = {};
    hdr.magic              = kN3mvMagic;
    hdr.version            = kN3mvVersion;
    hdr.width              = cookWidth;
    hdr.height             = cookHeight;
    hdr.frameRateMilli     = cookFps * 1000u;
    hdr.estimatedFrames    = estimatedFrames;
    hdr.audioSampleRate    = hasAudio ? cookAudioRate : 0u;
    hdr.audioNumChannels   = hasAudio ? uint16_t(cookAudioCh) : uint16_t(0);
    hdr.audioBitsPerSample = hasAudio ? uint16_t(kAudioBits) : uint16_t(0);
    hdr.audioByteSize      = uint32_t(audioBytes.size());
    hdr.videoByteSize      = uint32_t(h264Bytes.size());

    outCookedData.clear();
    outCookedData.reserve(sizeof(N3mvHeader) + audioBytes.size() + h264Bytes.size());
    const uint8_t* hdrPtr = reinterpret_cast<const uint8_t*>(&hdr);
    outCookedData.insert(outCookedData.end(), hdrPtr, hdrPtr + sizeof(N3mvHeader));
    outCookedData.insert(outCookedData.end(), audioBytes.begin(), audioBytes.end());
    outCookedData.insert(outCookedData.end(), h264Bytes.begin(), h264Bytes.end());

    LogDebug("CookN3dsMvdVideo: ~%u frames @ %ux%u @ %u fps, audio=%u bytes, h264=%u bytes, output=%zu bytes",
             estimatedFrames, cookWidth, cookHeight, cookFps,
             hdr.audioByteSize, hdr.videoByteSize, outCookedData.size());

    return true;
}

} // namespace VideoPlayerAddon

#endif // EDITOR
