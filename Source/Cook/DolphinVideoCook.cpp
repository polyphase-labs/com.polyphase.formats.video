#include "Cook/DolphinVideoCook.h"

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
#include <system_error>

// File / process helpers in the engine (DoesDirExist, CreateDir, SYS_ExecFull,
// SYS_OpenDirectory, ...) are not POLYPHASE_API, so they don't link from an addon
// DLL. Use std::filesystem + popen instead — every host the editor builds on has
// both, and the cook is editor-only so the C++17 surface is available.

#if defined(_WIN32)
    #define ADDON_POPEN  _popen
    #define ADDON_PCLOSE _pclose
#else
    #include <sys/wait.h> // WEXITSTATUS
    #define ADDON_POPEN  popen
    #define ADDON_PCLOSE pclose
#endif

namespace VideoPlayerAddon
{

namespace fs = std::filesystem;

namespace
{
    // PCM audio is always 16-bit signed; nothing in the runtime path supports anything
    // else, so it isn't a per-clip knob.
    constexpr uint32_t kCookAudioBits  = 16;

    std::string GetFFmpegBin()
    {
        // Priority: explicit env var > PATH fallback. Addon ships its own ffmpeg.exe
        // under External/ffmpeg/bin; users can set POLYPHASE_FFMPEG to that path or
        // a system install. Bare "ffmpeg" works when it's on PATH.
        const char* env = std::getenv("POLYPHASE_FFMPEG");
        if (env != nullptr && env[0] != '\0') return env;
        return "ffmpeg";
    }

    std::string CookTempDir()
    {
        std::string root = GetEngineState()->mProjectDirectory + "Intermediate/VideoClipCook";
        std::error_code ec;
        fs::create_directories(root, ec); // no-op if it already exists
        return root;
    }

    void AppendU32LE(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(uint8_t(v >>  0));
        out.push_back(uint8_t(v >>  8));
        out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 24));
    }

    std::string Quote(const std::string& s)
    {
        return "\"" + s + "\"";
    }

    // Spawn a shell command via popen, capture combined stdout+stderr (FFmpeg writes
    // its progress + warnings to stderr by default), and surface a useful error tail
    // in outError on failure. Returns true only when the command exited 0.
    bool RunCommand(const std::string& cmd, std::string& outError, bool streamProgress = false)
    {
        // Redirect stderr into stdout so the single popen pipe carries everything.
        //
        // Windows quoting: _popen spawns via `cmd /c <command>`. If the command both
        // starts and ends with `"`, cmd /c strips that outer pair — which mangles
        // commands like `"ffmpeg.exe" ... "frames\frame_%05d.jpg"` into
        // `ffmpeg.exe" ... "frames\...` and the path becomes garbage. The fix is
        // to wrap the entire string in one extra quote pair so one survives the
        // strip and the inner quoting reaches ffmpeg intact.
#if defined(_WIN32)
        const std::string redirected = "\"" + cmd + " 2>&1\"";
#else
        const std::string redirected = cmd + " 2>&1";
#endif

        FILE* pipe = ADDON_POPEN(redirected.c_str(), "r");
        if (pipe == nullptr)
        {
            outError = "popen failed for: " + cmd;
            return false;
        }

        std::string captured;
        captured.reserve(512);
        std::string lineBuf;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe) != nullptr)
        {
            captured.append(buf);
            if (streamProgress)
            {
                // Re-chunk into lines and emit each through LogDebug so the editor
                // log shows ffmpeg progress (e.g. "frame=  120 fps=30 ...") tick up
                // during long cooks instead of going silent for tens of seconds.
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
            // Cap captured output so a misbehaving tool can't blow up RAM.
            if (captured.size() > 64 * 1024) break;
        }
        if (streamProgress && !lineBuf.empty())
        {
            LogDebug("ffmpeg: %s", lineBuf.c_str());
        }

        int rc = ADDON_PCLOSE(pipe);
#if !defined(_WIN32)
        // POSIX: pclose returns wait status; extract real exit code.
        if (rc != -1) rc = WEXITSTATUS(rc);
#endif
        if (rc != 0)
        {
            outError = "ffmpeg failed (exitCode=" + std::to_string(rc) + ")";
            if (!captured.empty())
            {
                const size_t kTailMax = 1024;
                std::string tail = captured.size() > kTailMax
                    ? captured.substr(captured.size() - kTailMax) : captured;
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
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); return false; }
        out.resize(size_t(sz));
        size_t got = (sz > 0) ? fread(out.data(), 1, size_t(sz), f) : 0;
        fclose(f);
        return got == size_t(sz);
    }

    // Delete every frame_*.jpg in the given directory. Re-creates the directory if
    // missing so callers can rely on it existing on return.
    void ClearFramesDir(const std::string& framesDir)
    {
        std::error_code ec;
        if (!fs::is_directory(framesDir, ec))
        {
            fs::create_directories(framesDir, ec);
            return;
        }
        for (const auto& entry : fs::directory_iterator(framesDir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.size() >= 11 &&
                name.rfind("frame_", 0) == 0 &&
                name.substr(name.size() - 4) == ".jpg")
            {
                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
            }
        }
    }

    // Returns the sorted list of frame_*.jpg basenames. Used to drive the mux pass —
    // we trust the filesystem rather than guessing how many frames ffmpeg produced.
    std::vector<std::string> ListFrameFiles(const std::string& framesDir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::is_directory(framesDir, ec)) return out;
        for (const auto& entry : fs::directory_iterator(framesDir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.size() >= 11 &&
                name.rfind("frame_", 0) == 0 &&
                name.substr(name.size() - 4) == ".jpg")
            {
                out.push_back(std::move(name));
            }
        }
        std::sort(out.begin(), out.end()); // ffmpeg's %05d numbering sorts lexicographically
        return out;
    }
} // namespace

bool CookDolphinVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError)
{
    if (platform != Platform::GameCube &&
        platform != Platform::Wii      &&
        platform != Platform::N3DS)
    {
        outError = "CookDolphinVideo: wrong platform";
        return false;
    }
    if (sourceBytes.empty())
    {
        outError = "CookDolphinVideo: empty source";
        return false;
    }

    // Sanity-clamp inspector inputs so a typo can't blow up FFmpeg.
    const uint32_t cookWidth   = (params.width   > 0 && params.width   <= 1024) ? params.width   : 512u;
    const uint32_t cookHeight  = (params.height  > 0 && params.height  <= 1024) ? params.height  : 288u;
    const uint32_t cookFps     = (params.fps     > 0 && params.fps     <= 60)   ? params.fps     : 30u;
    const uint32_t cookQuality = (params.jpegQuality >= 2 && params.jpegQuality <= 31) ? params.jpegQuality : 3u;
    const uint32_t cookAudioRate = (params.audioSampleRate >= 8000 && params.audioSampleRate <= 48000)
                                       ? params.audioSampleRate : 22050u;
    const uint32_t cookAudioCh = (params.audioNumChannels == 1 || params.audioNumChannels == 2)
                                       ? params.audioNumChannels : 2u;

    const std::string tempDir = CookTempDir();
    const std::string ext = codecHint.empty() ? std::string("bin") : codecHint;
    const std::string srcPath   = tempDir + "/source." + ext;
    const std::string framesDir = tempDir + "/frames";
    const std::string framePat  = framesDir + "/frame_%05d.jpg";
    const std::string audioPath = tempDir + "/audio.pcm";

    // Wipe stale state from a previous cook.
    ClearFramesDir(framesDir);
    {
        std::error_code ec;
        fs::remove(audioPath, ec);
    }

    // Drop the source bytes to disk so FFmpeg has a path to chew on.
    {
        Stream s((const char*)sourceBytes.data(), uint32_t(sourceBytes.size()));
        s.WriteFile(srcPath.c_str());
    }

    const std::string ffmpeg = GetFFmpegBin();

    // ---- Pass 1: extract video frames as MJPEG. ----
    {
        std::string cmd;
        cmd += Quote(ffmpeg);
        cmd += " -y -hide_banner -loglevel info -stats";
        cmd += " -i " + Quote(srcPath);
        cmd += " -an"; // strip audio for this pass
        cmd += " -vf \"scale=" + std::to_string(cookWidth) + ":" + std::to_string(cookHeight)
             + ":flags=lanczos,format=yuvj420p\"";
        cmd += " -r " + std::to_string(cookFps);
        cmd += " -qscale:v " + std::to_string(cookQuality);
        cmd += " " + Quote(framePat);

        LogDebug("CookDolphinVideo: extracting MJPEG frames at %ux%u @ %u fps qscale=%u...",
                 cookWidth, cookHeight, cookFps, cookQuality);
        if (!RunCommand(cmd, outError, /*streamProgress=*/true))
        {
            return false;
        }
    }

    // ---- Pass 2: extract audio as raw 16-bit signed little-endian PCM. ----
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

        // Tolerate sources with no audio: command may fail. Probe file existence after.
        std::string ignore;
        RunCommand(cmd, ignore);

        std::error_code ec;
        hasAudio = fs::is_regular_file(audioPath, ec);
    }

    // ---- Mux: header + audio + per-frame MJPEG. ----
    std::vector<uint8_t> audioBytes;
    if (hasAudio)
    {
        if (!ReadFileBytes(audioPath, audioBytes))
        {
            outError = "CookDolphinVideo: failed to read intermediate audio.pcm";
            return false;
        }
    }

    const std::vector<std::string> frameNames = ListFrameFiles(framesDir);
    if (frameNames.empty())
    {
        outError = "CookDolphinVideo: ffmpeg produced no frames (check stderr / source)";
        return false;
    }

    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(frameNames.size());
    uint32_t totalFrameBytes = 0;
    for (const std::string& name : frameNames)
    {
        std::string framePath = framesDir + "/" + name;
        std::vector<uint8_t> jpegBytes;
        if (!ReadFileBytes(framePath, jpegBytes))
        {
            outError = "CookDolphinVideo: failed to read intermediate " + framePath;
            return false;
        }
        totalFrameBytes += 4 + uint32_t(jpegBytes.size());
        frames.push_back(std::move(jpegBytes));
    }

    PcvHeader hdr = {};
    hdr.magic              = kPcvMagic;
    hdr.version            = kPcvVersion;
    hdr.width              = cookWidth;
    hdr.height             = cookHeight;
    hdr.numFrames          = uint32_t(frames.size());
    hdr.frameRateMilli     = cookFps * 1000u;
    hdr.audioSampleRate    = hasAudio ? cookAudioRate : 0u;
    hdr.audioNumChannels   = hasAudio ? uint16_t(cookAudioCh) : uint16_t(0);
    hdr.audioBitsPerSample = hasAudio ? uint16_t(kCookAudioBits) : uint16_t(0);
    hdr.audioByteSize      = uint32_t(audioBytes.size());
    hdr.framesByteSize     = totalFrameBytes;

    outCookedData.clear();
    outCookedData.reserve(sizeof(PcvHeader) + audioBytes.size() + totalFrameBytes);

    // Header (memcpy so packed-struct layout matches the on-disk format).
    const uint8_t* hdrPtr = reinterpret_cast<const uint8_t*>(&hdr);
    outCookedData.insert(outCookedData.end(), hdrPtr, hdrPtr + sizeof(PcvHeader));

    // Audio.
    outCookedData.insert(outCookedData.end(), audioBytes.begin(), audioBytes.end());

    // Per-frame: size prefix + bytes.
    for (const auto& jpeg : frames)
    {
        AppendU32LE(outCookedData, uint32_t(jpeg.size()));
        outCookedData.insert(outCookedData.end(), jpeg.begin(), jpeg.end());
    }

    LogDebug("CookDolphinVideo: %u frames @ %ux%u, audio=%u bytes, output=%zu bytes",
             hdr.numFrames, hdr.width, hdr.height, hdr.audioByteSize, outCookedData.size());

    return true;
}

} // namespace VideoPlayerAddon

#endif // EDITOR
