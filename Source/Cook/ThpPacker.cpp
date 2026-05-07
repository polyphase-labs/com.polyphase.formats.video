#include "Cook/ThpPacker.h"

#if EDITOR

#include "Cook/DspAdpcm.h"
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
    constexpr uint32_t kThpVersion = 0x11000u; // common observed value (1.1)

    // Big-endian write helpers. THP fields are PPC native byte order; we're cooking
    // on x86 LE so every multi-byte write goes through a swap.
    void WriteBE32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(uint8_t(v >> 24));
        out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >>  8));
        out.push_back(uint8_t(v >>  0));
    }
    void WriteBE32At(std::vector<uint8_t>& out, size_t offset, uint32_t v)
    {
        out[offset + 0] = uint8_t(v >> 24);
        out[offset + 1] = uint8_t(v >> 16);
        out[offset + 2] = uint8_t(v >>  8);
        out[offset + 3] = uint8_t(v >>  0);
    }
    void WriteBytes(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
    {
        out.insert(out.end(), p, p + n);
    }
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

    void ClearFramesDir(const std::string& dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) { fs::create_directories(dir, ec); return; }
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string n = e.path().filename().string();
            if (n.size() >= 11 && n.rfind("frame_", 0) == 0 &&
                n.substr(n.size() - 4) == ".jpg")
            {
                std::error_code rmEc;
                fs::remove(e.path(), rmEc);
            }
        }
    }

    std::vector<std::string> ListFrameFiles(const std::string& dir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return out;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string n = e.path().filename().string();
            if (n.size() >= 11 && n.rfind("frame_", 0) == 0 &&
                n.substr(n.size() - 4) == ".jpg")
            {
                out.push_back(std::move(n));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
} // namespace

bool CookThpVideo(
    const std::vector<uint8_t>& sourceBytes,
    const std::string& codecHint,
    Platform platform,
    const DolphinCookParams& params,
    std::vector<uint8_t>& outCookedData,
    std::string& outError)
{
    if (platform != Platform::GameCube && platform != Platform::Wii)
    {
        outError = "CookThpVideo: wrong platform (THP is GameCube/Wii only)";
        return false;
    }
    if (sourceBytes.empty()) { outError = "empty source"; return false; }

    const uint32_t cookWidth   = (params.width   > 0 && params.width   <= 1024) ? params.width   : 512u;
    const uint32_t cookHeight  = (params.height  > 0 && params.height  <= 1024) ? params.height  : 288u;
    const uint32_t cookFps     = (params.fps     > 0 && params.fps     <= 60)   ? params.fps     : 30u;
    const uint32_t cookQuality = (params.jpegQuality >= 2 && params.jpegQuality <= 31) ? params.jpegQuality : 3u;
    const uint32_t cookAudioRate = (params.audioSampleRate >= 8000 && params.audioSampleRate <= 48000)
                                       ? params.audioSampleRate : 22050u;
    const uint32_t cookAudioCh   = (params.audioNumChannels == 1 || params.audioNumChannels == 2)
                                       ? params.audioNumChannels : 2u;

    const std::string tempDir   = CookTempDir();
    const std::string ext       = codecHint.empty() ? std::string("bin") : codecHint;
    const std::string srcPath   = tempDir + "/source." + ext;
    const std::string framesDir = tempDir + "/frames";
    const std::string framePat  = framesDir + "/frame_%05d.jpg";
    const std::string audioPath = tempDir + "/audio.pcm";

    ClearFramesDir(framesDir);
    { std::error_code ec; fs::remove(audioPath, ec); }

    {
        Stream s((const char*)sourceBytes.data(), uint32_t(sourceBytes.size()));
        s.WriteFile(srcPath.c_str());
    }

    const std::string ffmpeg = GetFFmpegBin();

    // ---- Pass 1: MJPEG frames. ----
    {
        std::string cmd;
        cmd += Quote(ffmpeg);
        cmd += " -y -hide_banner -loglevel info -stats";
        cmd += " -i " + Quote(srcPath);
        cmd += " -an";
        cmd += " -vf \"scale=" + std::to_string(cookWidth) + ":" + std::to_string(cookHeight)
             + ":flags=lanczos,format=yuvj420p\"";
        cmd += " -r " + std::to_string(cookFps);
        cmd += " -qscale:v " + std::to_string(cookQuality);
        cmd += " " + Quote(framePat);

        LogDebug("CookThpVideo: extracting MJPEG frames at %ux%u @ %u fps qscale=%u...",
                 cookWidth, cookHeight, cookFps, cookQuality);
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

    // ---- Read JPEG frames. ----
    const std::vector<std::string> frameNames = ListFrameFiles(framesDir);
    if (frameNames.empty())
    {
        outError = "CookThpVideo: ffmpeg produced no frames";
        return false;
    }
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(frameNames.size());
    for (const std::string& name : frameNames)
    {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(framesDir + "/" + name, bytes))
        {
            outError = "CookThpVideo: read fail " + name;
            return false;
        }
        frames.push_back(std::move(bytes));
    }
    const uint32_t numFrames = uint32_t(frames.size());

    // ---- Encode audio to DSP-ADPCM (per channel). ----
    std::vector<uint8_t> audioPcm;
    if (hasAudio) ReadFileBytes(audioPath, audioPcm);

    const uint32_t bytesPerPcmFrame = cookAudioCh * sizeof(int16_t);
    const uint32_t totalAudioFrames = hasAudio ? uint32_t(audioPcm.size() / bytesPerPcmFrame) : 0;

    // Per-channel ADPCM blobs and headers (one per channel, generated once for the
    // full clip — we re-slice into per-frame audio blocks below).
    std::vector<DspChannelHeader> chHeaders(hasAudio ? cookAudioCh : 0);
    std::vector<std::vector<uint8_t>> chAdpcm(hasAudio ? cookAudioCh : 0);

    if (hasAudio)
    {
        std::vector<int16_t> chSamples(totalAudioFrames);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(audioPcm.data());
        for (uint32_t ch = 0; ch < cookAudioCh; ++ch)
        {
            for (uint32_t i = 0; i < totalAudioFrames; ++i)
            {
                chSamples[i] = pcm[i * cookAudioCh + ch];
            }
            DspEncode(chSamples.data(), totalAudioFrames, cookAudioRate,
                      chHeaders[ch], chAdpcm[ch]);
        }
        LogDebug("CookThpVideo: DSP-ADPCM encoded %u channel(s), ~%zu bytes each",
                 cookAudioCh, chAdpcm.empty() ? 0 : chAdpcm[0].size());
    }

    // ---- Per-video-frame audio slicing.
    //
    // CRITICAL: the slice MUST land on ADPCM block boundaries (every 8 bytes /
    // every 14 samples). The encoder produces one continuous stream where each
    // block depends on the previous block's reconstructed yn1/yn2; if we tell
    // the decoder to emit a non-block-multiple number of samples, it stops mid-
    // block and its history snapshot doesn't match what the encoder had at the
    // *block* boundary. The next frame's decode then starts with stale history
    // and you hear a low-frequency pop at every frame boundary.
    //
    // Solution: every frame except possibly the last gets exactly N full blocks
    // (= 14*N samples). The last frame gets all remaining blocks but its
    // numSamples is trimmed to whatever's left of the original PCM count.
    const uint32_t totalAdpcmBlocks = (totalAudioFrames + 13) / 14;
    const uint32_t blocksPerVideoFrame = (numFrames > 0 && totalAdpcmBlocks > 0)
        ? (totalAdpcmBlocks + numFrames - 1) / numFrames : 0;
    const uint32_t fullSamplesPerVideoFrame = blocksPerVideoFrame * 14;

    // ---- Build the THP file. ----
    outCookedData.clear();
    outCookedData.reserve(64 * 1024 + 256 * numFrames);

    // Reserve space for the file header — we patch in the offsets after we know them.
    constexpr size_t kHeaderSize       = 0x40;
    constexpr size_t kComponentsBase   = kHeaderSize;     // ThpComponents follows header
    constexpr size_t kComponentsSize   = 4 + 16 + 4;      // numComponents + types[16] + pad
    const     size_t kVideoInfoSize    = 8;
    const     size_t kAudioInfoSize    = hasAudio ? 12 : 0;
    const     size_t kFirstFrameOffset = kComponentsBase + kComponentsSize + kVideoInfoSize + kAudioInfoSize;

    outCookedData.resize(kFirstFrameOffset, 0);

    // Components table.
    {
        size_t off = kComponentsBase;
        WriteBE32At(outCookedData, off,         hasAudio ? 2u : 1u); // numComponents
        // Component types: byte 0 = video (0), byte 1 = audio (1), rest = 0xFF (unused).
        for (int i = 0; i < 16; ++i) outCookedData[off + 4 + i] = 0xFF;
        outCookedData[off + 4 + 0] = 0; // video
        if (hasAudio) outCookedData[off + 4 + 1] = 1; // audio
        // 4 bytes pad after the types — already zero.
    }

    // Video info.
    {
        size_t off = kComponentsBase + kComponentsSize;
        WriteBE32At(outCookedData, off,     cookWidth);
        WriteBE32At(outCookedData, off + 4, cookHeight);
    }
    // Audio info.
    if (hasAudio)
    {
        size_t off = kComponentsBase + kComponentsSize + kVideoInfoSize;
        WriteBE32At(outCookedData, off,     cookAudioCh);
        WriteBE32At(outCookedData, off + 4, cookAudioRate);
        WriteBE32At(outCookedData, off + 8, totalAudioFrames);
    }

    // Frames. Track per-frame size so we can patch each frame's prev/next.
    std::vector<uint32_t> frameOffsets(numFrames);
    std::vector<uint32_t> frameSizes(numFrames);
    uint32_t firstFrameSize = 0;

    for (uint32_t i = 0; i < numFrames; ++i)
    {
        const size_t frameStart = outCookedData.size();
        frameOffsets[i] = uint32_t(frameStart);

        // Frame header: nextFrameSize + prevFrameSize. We don't know nextFrameSize yet,
        // so write a placeholder and patch after this frame is fully built.
        WriteBE32(outCookedData, 0); // nextFrameSize (patched)
        WriteBE32(outCookedData, (i == 0) ? firstFrameSize : frameSizes[i - 1]); // prevFrameSize

        // Video payload.
        WriteBE32(outCookedData, uint32_t(frames[i].size()));
        WriteBytes(outCookedData, frames[i].data(), frames[i].size());

        // Pad video payload to 32-byte alignment? THP files are usually 32-aligned
        // throughout for DSP DMA. Add zero padding inside the videoSize-counted span
        // would corrupt the size, so we pad AFTER instead and bump videoSize. For
        // simplicity v1 skips alignment — fix in a follow-up if Dolphin complains.

        // Audio payload (per video frame). Each channel: header + ADPCM slice.
        // Slices are ALWAYS aligned to ADPCM block boundaries; see the long
        // comment above the per-frame loop for why.
        if (hasAudio)
        {
            const uint32_t blockStart = i * blocksPerVideoFrame;
            const uint32_t blockEnd   = std::min((i + 1) * blocksPerVideoFrame, totalAdpcmBlocks);
            const uint32_t blocksThisFrame = (blockEnd > blockStart) ? (blockEnd - blockStart) : 0;
            const uint32_t bytesThisFrame  = blocksThisFrame * 8;
            const uint32_t adpcmStartByte  = blockStart * 8;

            // numSamples emitted per channel this frame. All but the last frame
            // emit exactly 14*blocks; the final frame trims to the original PCM
            // count so we don't emit padding samples.
            uint32_t samplesThisFrame;
            if (blockEnd < totalAdpcmBlocks)
            {
                samplesThisFrame = blocksThisFrame * 14;
            }
            else
            {
                // Last frame: original samples beyond blockStart*14, capped at
                // what the blocks can hold.
                const uint32_t baseSample = blockStart * 14;
                samplesThisFrame = (totalAudioFrames > baseSample)
                    ? std::min(totalAudioFrames - baseSample, blocksThisFrame * 14)
                    : 0u;
            }

            const uint32_t perChannelHeaderBytes = uint32_t(sizeof(DspChannelHeader));
            const uint32_t totalChannelBytes     = cookAudioCh * (perChannelHeaderBytes + bytesThisFrame);
            const uint32_t audioSize             = 8 + totalChannelBytes;

            WriteBE32(outCookedData, audioSize);
            WriteBE32(outCookedData, samplesThisFrame);
            for (uint32_t ch = 0; ch < cookAudioCh; ++ch)
            {
                DspChannelHeader hdrBE = DspHeaderToBE(chHeaders[ch]);
                WriteBytes(outCookedData,
                           reinterpret_cast<const uint8_t*>(&hdrBE),
                           sizeof(hdrBE));
                if (bytesThisFrame > 0 && adpcmStartByte < chAdpcm[ch].size())
                {
                    WriteBytes(outCookedData,
                               chAdpcm[ch].data() + adpcmStartByte,
                               bytesThisFrame);
                }
            }
        }

        const uint32_t frameSize = uint32_t(outCookedData.size() - frameStart);
        frameSizes[i] = frameSize;
        if (i == 0) firstFrameSize = frameSize;

        // Patch the PREVIOUS frame's nextFrameSize now that we know it.
        if (i > 0)
        {
            WriteBE32At(outCookedData, frameOffsets[i - 1], frameSize);
        }
    }
    // Last frame's nextFrameSize stays 0 (sentinel).

    // ---- Patch the file header. ----
    const uint32_t lastFrameOffset = numFrames > 0 ? frameOffsets[numFrames - 1] : uint32_t(kFirstFrameOffset);
    const uint32_t totalDataSize   = uint32_t(outCookedData.size()) - uint32_t(kFirstFrameOffset);

    // Magic: "THP\0" — write the 4 ASCII bytes directly.
    outCookedData[0] = 'T'; outCookedData[1] = 'H';
    outCookedData[2] = 'P'; outCookedData[3] = '\0';
    WriteBE32At(outCookedData, 0x04, kThpVersion);

    // maxBufferSize: largest frame payload size; readers use this to size their
    // streaming buffer.
    uint32_t maxFrameSize = 0;
    for (uint32_t s : frameSizes) maxFrameSize = std::max(maxFrameSize, s);
    WriteBE32At(outCookedData, 0x08, maxFrameSize);
    WriteBE32At(outCookedData, 0x0C, hasAudio ? fullSamplesPerVideoFrame : 0u); // maxAudioSamples
    // FPS as IEEE float (BE).
    {
        const float f = float(cookFps);
        uint32_t v;
        std::memcpy(&v, &f, 4);
        WriteBE32At(outCookedData, 0x10, v);
    }
    WriteBE32At(outCookedData, 0x14, numFrames);
    WriteBE32At(outCookedData, 0x18, firstFrameSize);
    WriteBE32At(outCookedData, 0x1C, totalDataSize);
    WriteBE32At(outCookedData, 0x20, uint32_t(kComponentsBase));   // componentDataOffset
    WriteBE32At(outCookedData, 0x24, 0u);                          // offsetsDataOffset (none)
    WriteBE32At(outCookedData, 0x28, uint32_t(kFirstFrameOffset));
    WriteBE32At(outCookedData, 0x2C, lastFrameOffset);

    LogDebug("CookThpVideo: %u frames, %ux%u @ %u fps, audio=%s, output=%zu bytes",
             numFrames, cookWidth, cookHeight, cookFps,
             hasAudio ? "DSP-ADPCM" : "none", outCookedData.size());

    return true;
}

} // namespace VideoPlayerAddon

#endif // EDITOR
