#pragma once

#include "Asset.h"

#include <cstdint>
#include <string>
#include <vector>

// Defined in an addon DLL; deliberately not POLYPHASE_API (which would mark the class
// dllimport from Polyphase.dll on Windows). Same convention as VideoPlayer3D.
class VideoClip : public Asset
{
public:

    DECLARE_ASSET(VideoClip, Asset);

    VideoClip();
    ~VideoClip();

    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    // Inspector-driven property dispatcher. Catches the "Test Cook (GameCube)"
    // button press and calls TestCookForPlatform to swap mSourceData in place.
    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

    // Replaces mSourceData with the cook output for the given console platform and
    // clears the codec hint so the runtime decoder factory detects the PCV1 magic
    // at load time. Editor-only diagnostic so PCV1 round-trip can be validated on PC
    // without a console toolchain. Returns false on cook failure (mSourceData
    // untouched).
    bool TestCookForPlatform(Platform platform);

    // Bytes of the source video file as imported (editor-side). On PC platforms these
    // are also what gets written through to the cooked .oct so the runtime can decode
    // the original container. Console platforms will replace this with a per-platform
    // cooked variant in a later stage.
    const std::vector<uint8_t>& GetSourceData() const { return mSourceData; }
    uint32_t GetSourceSize() const { return (uint32_t)mSourceData.size(); }

    // Streaming-from-disk: when the cook format is THP or N3MV, the cooked
    // bytes live in a sidecar file at this absolute path and mSourceData is
    // empty. The runtime decoder opens this path via fopen + lazy seek/read.
    // Empty when the asset is uncooked, freshly imported, or used PCV1 (which
    // always inlines).
    const std::string& GetSidecarPath() const { return mSidecarPath; }

    // Hints captured at import time. Used by the runtime decoder to pick a backend
    // (e.g. ffmpeg vs. THP) and to size the output texture before the first frame
    // arrives.
    const std::string& GetCodecHint() const { return mCodecHint; }
    uint32_t GetWidth() const                { return mWidth; }
    uint32_t GetHeight() const               { return mHeight; }
    float    GetDurationSeconds() const      { return mDurationSec; }
    float    GetFrameRate() const            { return mFrameRate; }
    uint32_t GetAudioSampleRate() const      { return mAudioSampleRate; }
    uint32_t GetAudioNumChannels() const     { return mAudioNumChannels; }

protected:

    // Source bytes (full container as imported). Always present in the editor; on
    // shipped console builds this is replaced with the cooked-variant bytes during
    // SaveStream.
    std::vector<uint8_t> mSourceData;

    // Extension lower-case ("mp4", "webm", ...). Empty on cooked console variants
    // where the codec is implied by the platform.
    std::string mCodecHint;

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    float    mDurationSec = 0.0f;
    float    mFrameRate = 0.0f;
    uint32_t mAudioSampleRate = 0;
    uint32_t mAudioNumChannels = 0;

    // Per-clip console-cook knobs. Defaults bias toward small file size for the
    // "30s attract loop on GameCube" use case, which has tight RAM constraints
    // (24 MB total on GC) and where intra-only MJPEG can't compete with H.264's
    // inter-frame prediction. For a 30s clip at these defaults expect roughly
    // 3-5 MB cooked output. Crank up Width/Height/FPS/JPEG Quality for sharper
    // content (longer clips will need streaming-from-disk to fit on GC).
    //
    // JPEG Quality uses ffmpeg's qscale:v scale: 2 = best, 31 = worst. 7 is
    // visibly clean for attract content at 320x180 without obvious blocking.
    //
    // Editor-editable via the inspector; serialized at the end of the asset
    // stream so existing .oct files still load (LoadStream falls back to
    // defaults when the bytes aren't present).
    uint32_t mCookWidth     = 320;
    uint32_t mCookHeight    = 180;
    uint32_t mCookFps       = 30;
    uint32_t mCookJpegQuality = 7;
    uint32_t mCookAudioRate = 22050;
    uint32_t mCookAudioCh   = 2;

    // Container format produced by the GameCube/Wii cook. PCV1 is our simple
    // MJPEG + raw PCM container (cross-platform, easy to debug, larger files).
    // THP is Nintendo's native GameCube/Wii format (MJPEG + DSP-ADPCM, smaller
    // audio, in theory playable by any THP decoder including Dolphin emulator's
    // dev tools). 3DS always uses PCV1 — THP is GC/Wii-only.
    //   0 = PCV1, 1 = THP
    uint32_t mCookFormat = 0;

    // Cook preset selector. Picking anything but Custom rewrites the six tunable
    // knobs above to a baked-in profile suitable for that preset; switching back
    // to Custom is a no-op (knobs stay at whatever the previous preset left them).
    //   0 = Custom (user edits knobs directly — last-set values stick)
    //   1 = Tiny     (~500 KB / 30s — 240×135, 15 fps, qscale 13, 11 kHz mono)
    //   2 = Balanced (~3 MB / 30s   — 320×180, 30 fps, qscale 7,  22 kHz stereo)
    //   3 = Quality  (~10 MB / 30s  — 512×288, 30 fps, qscale 3,  22 kHz stereo)
    uint32_t mCookPreset = 0;

    // Legacy serialization slot — was an inspector toggle in v3, now driven
    // implicitly by Cook Format (THP/N3MV always sidecar, PCV1 always inline).
    // Kept on disk for backward-compat when reading v3 .oct files; ignored at
    // cook and runtime. Newly-saved files keep writing this for forward compat.
    bool mUseSidecar = false;

    // Set only when the cook produced a sidecar (THP or N3MV). Empty for PCV1
    // (inline) or uncooked imports. Absolute path; the runtime opens it via
    // fopen + lazy seek/read so peak memory drops from clip-size to ~150 KB.
    std::string mSidecarPath;
};
