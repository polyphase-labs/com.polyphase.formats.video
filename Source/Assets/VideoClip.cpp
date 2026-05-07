#include "Assets/VideoClip.h"

#include "Log.h"
#include "Stream.h"
#include "Property.h"

#if EDITOR
#include "Engine.h"
#include "EngineTypes.h"
#include "AssetManager.h"
#include "Cook/DolphinVideoCook.h"
#include "Cook/ThpPacker.h"
#include "Cook/N3dsMvdCook.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <filesystem>

#if EDITOR && POLYPHASE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}
#endif

FORCE_LINK_DEF(VideoClip);
DEFINE_ASSET(VideoClip);


namespace
{
#if EDITOR && POLYPHASE_WITH_FFMPEG
    // Probe a source video file for inspector-display metadata. Best-effort: failures
    // log a warning but leave the caller's fields untouched. Runs once at Import time
    // (and re-runs only on re-import); not on the hot path.
    struct VideoMeta
    {
        uint32_t width = 0;
        uint32_t height = 0;
        float    durationSec = 0.0f;
        float    frameRate = 0.0f;
        uint32_t audioSampleRate = 0;
        uint32_t audioNumChannels = 0;
    };

    bool ProbeVideoFile(const char* path, VideoMeta& out)
    {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0)
        {
            avformat_close_input(&fmt);
            return false;
        }

        if (fmt->duration > 0)
        {
            out.durationSec = float(double(fmt->duration) / double(AV_TIME_BASE));
        }

        for (unsigned i = 0; i < fmt->nb_streams; ++i)
        {
            AVStream* s = fmt->streams[i];
            AVCodecParameters* par = s->codecpar;
            if (par == nullptr) continue;

            if (par->codec_type == AVMEDIA_TYPE_VIDEO && out.width == 0)
            {
                out.width  = uint32_t(par->width);
                out.height = uint32_t(par->height);

                AVRational fr = s->avg_frame_rate.num > 0 ? s->avg_frame_rate : s->r_frame_rate;
                if (fr.den > 0 && fr.num > 0)
                {
                    out.frameRate = float(av_q2d(fr));
                }
            }
            else if (par->codec_type == AVMEDIA_TYPE_AUDIO && out.audioSampleRate == 0)
            {
                out.audioSampleRate  = uint32_t(par->sample_rate);
                out.audioNumChannels = uint32_t(par->ch_layout.nb_channels);
            }
        }

        avformat_close_input(&fmt);
        return true;
    }
#endif
} // namespace

VideoClip::VideoClip()
{
    mType = VideoClip::GetStaticType();
}

VideoClip::~VideoClip()
{
}

void VideoClip::LoadStream(Stream& stream, Platform platform)
{
    Asset::LoadStream(stream, platform);

    // Header
    stream.ReadString(mCodecHint);
    mWidth            = stream.ReadUint32();
    mHeight           = stream.ReadUint32();
    mDurationSec      = stream.ReadFloat();
    mFrameRate        = stream.ReadFloat();
    mAudioSampleRate  = stream.ReadUint32();
    mAudioNumChannels = stream.ReadUint32();

    // Payload (full source container on PC; per-platform cooked variant on consoles
    // once those stages are wired up).
    uint32_t payloadSize = stream.ReadUint32();
    mSourceData.resize(payloadSize);
    for (uint32_t i = 0; i < payloadSize; ++i)
    {
        mSourceData[i] = stream.ReadUint8();
    }

    // Optional trailing block: per-clip cook knobs. Older .oct files saved before
    // these were exposed simply lack the trailing bytes — we keep the defaults set
    // by the header. Anchored by a magic uint32 so misalignment can't be silently
    // misread as garbage parameters.
    //   v1 layout: magic + 6 uints  (no Cook Format field)
    //   v2 layout: magic + 7 uints  (adds Cook Format)
    // v1 magic kept the same; we detect via remaining-bytes count after the magic.
    constexpr uint32_t kCookParamsMagic = 0xC0017AC1u;
    if (stream.GetPos() + 4 <= stream.GetSize())
    {
        const uint32_t mark = stream.ReadUint32();
        if (mark == kCookParamsMagic && stream.GetPos() + 24 <= stream.GetSize())
        {
            mCookWidth       = stream.ReadUint32();
            mCookHeight      = stream.ReadUint32();
            mCookFps         = stream.ReadUint32();
            mCookJpegQuality = stream.ReadUint32();
            mCookAudioRate   = stream.ReadUint32();
            mCookAudioCh     = stream.ReadUint32();
            // Cook Format added in v2 — present if there's another uint left.
            if (stream.GetPos() + 4 <= stream.GetSize())
            {
                mCookFormat = stream.ReadUint32();
            }
            // v3: Cook Preset, Use Sidecar, Sidecar Path. Each presence-checked
            // independently so v2 / v1 / pre-trailer files all load cleanly.
            if (stream.GetPos() + 4 <= stream.GetSize())
            {
                mCookPreset = stream.ReadUint32();
            }
            if (stream.GetPos() + 1 <= stream.GetSize())
            {
                mUseSidecar = stream.ReadBool();
            }
            if (stream.GetPos() + 4 <= stream.GetSize())
            {
                stream.ReadString(mSidecarPath);
            }
        }
        else
        {
            // No magic — rewind so the surrounding system doesn't lose alignment.
            stream.SetPos(stream.GetPos() - 4);
        }
    }
}

void VideoClip::SaveStream(Stream& stream, Platform platform)
{
    Asset::SaveStream(stream, platform);

    // Header — same layout for every platform; the payload below is what varies.
    stream.WriteString(mCodecHint);
    stream.WriteUint32(mWidth);
    stream.WriteUint32(mHeight);
    stream.WriteFloat(mDurationSec);
    stream.WriteFloat(mFrameRate);
    stream.WriteUint32(mAudioSampleRate);
    stream.WriteUint32(mAudioNumChannels);

    // Per-platform payload. PC platforms passthrough the source container so the
    // runtime FFmpeg decoder can open it directly. GameCube/Wii cook to PCV1 (MJPEG
    // frames + raw PCM audio) for the runtime ThpVideoDecoder. 3DS will get its own
    // MVD-targeted cook in a later stage. Failures fall back to passthrough so the
    // asset still round-trips; the runtime decoder will then surface the error.
    std::vector<uint8_t> payloadStorage;
    const std::vector<uint8_t>* payload = &mSourceData;

#if EDITOR
    const bool isHardwarePlatform = (platform == Platform::GameCube ||
                                     platform == Platform::Wii      ||
                                     platform == Platform::N3DS);
    bool hydratedFromSidecar = false;
    if (isHardwarePlatform)
    {
        // If a prior TestCook produced a sidecar (THP / N3MV path), TestCook
        // emptied mSourceData and stored the cooked bytes in a sidecar file at
        // the dev-machine path in mSidecarPath. The runtime can't reach that
        // path on the target device, so for hardware packaging we read the
        // sidecar back from disk and embed its bytes inline. The runtime
        // decoder factory routes by magic in the bytes themselves, so an
        // inline payload is functionally identical to the sidecar path.
        if (!mSidecarPath.empty() && mSourceData.empty())
        {
            FILE* sf = fopen(mSidecarPath.c_str(), "rb");
            if (sf != nullptr)
            {
                fseek(sf, 0, SEEK_END);
                const long sz = ftell(sf);
                fseek(sf, 0, SEEK_SET);
                if (sz > 4)
                {
                    payloadStorage.resize(size_t(sz));
                    const size_t got = fread(payloadStorage.data(), 1, size_t(sz), sf);
                    if (got == size_t(sz))
                    {
                        payload = &payloadStorage;
                        hydratedFromSidecar = true;
                        LogDebug("VideoClip::SaveStream: hydrated sidecar '%s' (%ld bytes) inline for hardware packaging",
                                 mSidecarPath.c_str(), sz);
                    }
                    else
                    {
                        LogWarning("VideoClip::SaveStream: short read on sidecar '%s' (%zu/%ld)",
                                   mSidecarPath.c_str(), got, sz);
                        payloadStorage.clear();
                    }
                }
                fclose(sf);
            }
            else
            {
                LogWarning("VideoClip::SaveStream: cannot open sidecar '%s' for hardware packaging — "
                           "the cooked .oct will be empty. Re-run Test Cook for the target platform.",
                           mSidecarPath.c_str());
            }
        }
    }

    if (isHardwarePlatform && !hydratedFromSidecar)
    {
        // If mSourceData is already a cooked PCV1 / THP / N3MV container
        // (TestCook overwrote it earlier), passthrough rather than feeding
        // cooked bytes back to FFmpeg. The runtime decoder factory routes by
        // magic, so the cooked bytes will play correctly on the target platform.
        bool alreadyCooked = false;
        if (mSourceData.size() >= 4)
        {
            const uint8_t* p = mSourceData.data();
            alreadyCooked = (p[0] == 'P' && p[1] == 'C' && p[2] == 'V' && p[3] == '1') ||
                            (p[0] == 'T' && p[1] == 'H' && p[2] == 'P' && p[3] == '\0') ||
                            (p[0] == 'N' && p[1] == '3' && p[2] == 'M' && p[3] == 'V');
        }

        if (alreadyCooked)
        {
            LogDebug("VideoClip::SaveStream: '%s' is already cooked — passthrough", mName.c_str());
            // payload still points at mSourceData
        }
        else
        {
            VideoPlayerAddon::DolphinCookParams params;
            params.width            = mCookWidth;
            params.height           = mCookHeight;
            params.fps              = mCookFps;
            params.jpegQuality      = mCookJpegQuality;
            params.audioSampleRate  = mCookAudioRate;
            params.audioNumChannels = mCookAudioCh;

            // Cook Format dispatch:
            //   THP (1)  - GC/Wii only (Nintendo native MJPEG + DSP-ADPCM)
            //   N3MV (2) - 3DS only (hardware H.264 via MVD)
            //   PCV1 (0) - any console (cross-platform software MJPEG + PCM)
            const bool useThp = (mCookFormat == 1) &&
                                (platform == Platform::GameCube || platform == Platform::Wii);
            const bool useN3mv = (mCookFormat == 2) && (platform == Platform::N3DS);

            std::string cookErr;
            bool ok = false;
            const char* cookName = "Dolphin";
            if (useThp)
            {
                cookName = "THP";
                ok = VideoPlayerAddon::CookThpVideo(mSourceData, mCodecHint, platform, params, payloadStorage, cookErr);
            }
            else if (useN3mv)
            {
                cookName = "N3MV";
                ok = VideoPlayerAddon::CookN3dsMvdVideo(mSourceData, mCodecHint, platform, params, payloadStorage, cookErr);
            }
            else
            {
                ok = VideoPlayerAddon::CookDolphinVideo(mSourceData, mCodecHint, platform, params, payloadStorage, cookErr);
            }

            if (ok)
            {
                payload = &payloadStorage;
            }
            else
            {
                LogError("VideoClip::SaveStream: %s cook failed for '%s': %s",
                         cookName, mName.c_str(), cookErr.c_str());
                // Leave payload pointing at mSourceData; runtime will detect the
                // unrecognized format and fall back to NullVideoDecoder test bars.
            }
        }
    }
#endif

    stream.WriteUint32((uint32_t)payload->size());
    for (uint8_t b : *payload)
    {
        stream.WriteUint8(b);
    }

    // Cook params trailer (see LoadStream for the matching read). Magic-prefixed
    // so it's unambiguously distinguishable from older asset versions.
    constexpr uint32_t kCookParamsMagic = 0xC0017AC1u;
    stream.WriteUint32(kCookParamsMagic);
    stream.WriteUint32(mCookWidth);
    stream.WriteUint32(mCookHeight);
    stream.WriteUint32(mCookFps);
    stream.WriteUint32(mCookJpegQuality);
    stream.WriteUint32(mCookAudioRate);
    stream.WriteUint32(mCookAudioCh);
    stream.WriteUint32(mCookFormat); // v2: Cook Format
    stream.WriteUint32(mCookPreset); // v3: Cook Preset
    stream.WriteBool(mUseSidecar);   // v3: Use Sidecar
    // v3: Sidecar Path. For hardware platforms we hydrated the sidecar bytes
    // inline above, so persist an empty path — otherwise the runtime would try
    // to fopen() the dev-machine path on the device and fail. Editor / PC
    // builds keep the path so the editor preview still streams from disk.
#if EDITOR
    stream.WriteString(isHardwarePlatform ? std::string() : mSidecarPath);
#else
    stream.WriteString(mSidecarPath);
#endif
}

void VideoClip::Create()
{
    Asset::Create();
}

void VideoClip::Destroy()
{
    Asset::Destroy();
    mSourceData.clear();
    mSourceData.shrink_to_fit();
}

bool VideoClip::Import(const std::string& path, ImportOptions* options)
{
    OCT_UNUSED(options);

    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        LogError("VideoClip::Import: failed to open '%s'", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0)
    {
        fclose(f);
        LogError("VideoClip::Import: '%s' is empty or unreadable", path.c_str());
        return false;
    }

    mSourceData.resize((size_t)sz);
    size_t got = fread(mSourceData.data(), 1, (size_t)sz, f);
    fclose(f);

    if (got != (size_t)sz)
    {
        LogError("VideoClip::Import: short read on '%s' (got %zu of %ld)", path.c_str(), got, sz);
        mSourceData.clear();
        return false;
    }

    // Codec hint is the lower-cased extension without the dot.
    mCodecHint.clear();
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < path.size())
    {
        mCodecHint.assign(path.begin() + (ptrdiff_t)dot + 1, path.end());
        std::transform(mCodecHint.begin(), mCodecHint.end(), mCodecHint.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }

    // Editor-side metadata probe: open the source with FFmpeg long enough to fill
    // width/height/duration/fps/audio for inspector display. Best-effort — a failed
    // probe only means the inspector shows zeros until the runtime decoder opens
    // the bytes; import itself still succeeds so the user can drop the asset on a
    // VideoPlayer3D and play it.
#if EDITOR && POLYPHASE_WITH_FFMPEG
    {
        VideoMeta meta;
        if (ProbeVideoFile(path.c_str(), meta))
        {
            mWidth            = meta.width;
            mHeight           = meta.height;
            mDurationSec      = meta.durationSec;
            mFrameRate        = meta.frameRate;
            mAudioSampleRate  = meta.audioSampleRate;
            mAudioNumChannels = meta.audioNumChannels;
        }
        else
        {
            LogWarning("VideoClip::Import: FFmpeg probe failed for '%s' — inspector metadata will be zero", path.c_str());
        }
    }
#endif

    return true;
}

void VideoClip::GatherProperties(std::vector<Property>& outProps)
{
    Asset::GatherProperties(outProps);

    // Read-only source metadata. Set by Import / cook; not user-editable.
    outProps.push_back(Property(DatumType::String, "Codec",         this, &mCodecHint));
    outProps.push_back(Property(DatumType::Integer,"Width",         this, &mWidth));
    outProps.push_back(Property(DatumType::Integer,"Height",        this, &mHeight));
    outProps.push_back(Property(DatumType::Float,  "Duration (s)",  this, &mDurationSec));
    outProps.push_back(Property(DatumType::Float,  "Frame Rate",    this, &mFrameRate));
    outProps.push_back(Property(DatumType::Integer,"Audio Rate",    this, &mAudioSampleRate));
    outProps.push_back(Property(DatumType::Integer,"Audio Channels",this, &mAudioNumChannels));

    // Per-clip cook knobs for the GameCube/Wii target (PCV1 container). Editing any
    // of these affects both packaging and the editor-side Test Cook buttons below.
    // Defaults bias toward acceptable quality at moderate file size; for short
    // attract clips you can crank quality up, for longer clips dial them down to
    // fit GC's 24 MB RAM budget. JPEG quality is FFmpeg's qscale:v scale where
    // 2 = best, 31 = worst (lower number = bigger file).
    outProps.push_back(Property(DatumType::Integer,"Cook Width",        this, &mCookWidth,       1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer,"Cook Height",       this, &mCookHeight,      1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer,"Cook FPS",          this, &mCookFps,         1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer,"Cook JPEG Quality", this, &mCookJpegQuality, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer,"Cook Audio Rate",   this, &mCookAudioRate,   1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer,"Cook Audio Ch",     this, &mCookAudioCh,     1, HandlePropChange));

    static const char* sCookFormatStrings[] = {
        "PCV1 (MJPEG+PCM)",
        "THP (MJPEG+ADPCM)",
        "N3MV (3DS H.264 MVD)",
    };
    outProps.push_back(Property(DatumType::Integer,"Cook Format",       this, &mCookFormat,      1, HandlePropChange,
                                NULL_DATUM, 3, sCookFormatStrings));

    // Cook preset — selecting anything but Custom rewrites the six knobs above
    // to a baked-in profile (~500KB / ~3MB / ~10MB target sizes for a 30s clip).
    static const char* sCookPresetStrings[] = { "Custom", "Tiny", "Balanced", "Quality" };
    outProps.push_back(Property(DatumType::Integer,"Cook Preset",       this, &mCookPreset,      1, HandlePropChange,
                                NULL_DATUM, 4, sCookPresetStrings));

    // Note: sidecar streaming is no longer an inspector knob — it's implicit
    // from Cook Format. THP and N3MV always sidecar (the runtime decoders
    // support file streaming, the cook output is gitignore-able next to the
    // .oct); PCV1 always inlines (its decoder doesn't support file streaming
    // and PCV1 is the "easy debug" format anyway).

#if EDITOR
    // Diagnostic buttons. Toggling either one cooks the source bytes in place to
    // the cooked-format used by that console target; on next save the asset stores
    // the cooked bytes and the editor's PIE plays it back through PcvVideoDecoder
    // (matching what a packaged build would do). One-way until the user re-imports.
    static bool sFakeTestCookGC  = false;
    static bool sFakeTestCookWii = false;
    static bool sFakeTestCook3DS = false;
    outProps.push_back(Property(DatumType::Bool, "Test Cook (GameCube)", this, &sFakeTestCookGC,  1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Test Cook (Wii)",      this, &sFakeTestCookWii, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Test Cook (3DS)",      this, &sFakeTestCook3DS, 1, HandlePropChange));
#endif
}

bool VideoClip::HandlePropChange(Datum* datum, uint32_t /*index*/, const void* newValue)
{
#if EDITOR
    Property* prop = static_cast<Property*>(datum);
    VideoClip* self = static_cast<VideoClip*>(prop->mOwner);
    if (self == nullptr) return false;

    if (prop->mName == "Test Cook (GameCube)")
    {
        if (self->TestCookForPlatform(Platform::GameCube))
        {
            self->SetDirtyFlag();
        }
        return true; // we don't care about the static-bool value the framework wanted to write
    }
    if (prop->mName == "Test Cook (Wii)")
    {
        if (self->TestCookForPlatform(Platform::Wii))
        {
            self->SetDirtyFlag();
        }
        return true;
    }
    if (prop->mName == "Test Cook (3DS)")
    {
        if (self->TestCookForPlatform(Platform::N3DS))
        {
            self->SetDirtyFlag();
        }
        return true;
    }
    if (prop->mName == "Cook Preset")
    {
        const uint32_t presetId = *reinterpret_cast<const uint32_t*>(newValue);
        switch (presetId)
        {
            case 1: // Tiny
                self->mCookWidth = 240;  self->mCookHeight = 135;
                self->mCookFps = 15;     self->mCookJpegQuality = 13;
                self->mCookAudioRate = 11025; self->mCookAudioCh = 1;
                break;
            case 2: // Balanced
                self->mCookWidth = 320;  self->mCookHeight = 180;
                self->mCookFps = 30;     self->mCookJpegQuality = 7;
                self->mCookAudioRate = 22050; self->mCookAudioCh = 2;
                break;
            case 3: // Quality
                self->mCookWidth = 512;  self->mCookHeight = 288;
                self->mCookFps = 30;     self->mCookJpegQuality = 3;
                self->mCookAudioRate = 22050; self->mCookAudioCh = 2;
                break;
            default:
                // Custom / 0 — leave knobs alone, just record the preset value.
                break;
        }
        self->mCookPreset = presetId;
        self->SetDirtyFlag();
        return true; // we wrote mCookPreset ourselves; don't let the framework re-write
    }
#else
    (void)datum;
#endif
    return false;
}

bool VideoClip::TestCookForPlatform(Platform platform)
{
#if EDITOR
    if (platform != Platform::GameCube &&
        platform != Platform::Wii      &&
        platform != Platform::N3DS)
    {
        LogError("VideoClip::TestCookForPlatform: unsupported platform");
        return false;
    }
    if (mSourceData.empty())
    {
        LogError("VideoClip::TestCookForPlatform: '%s' has no source bytes", mName.c_str());
        return false;
    }

    // TestCook is one-way: it overwrites mSourceData with the cooked container so
    // the editor can play it back through PcvVideoDecoder / ThpVideoDecoder. Once
    // cooked, the original mp4/webm is gone and ffmpeg can't re-cook from PCV1 or
    // THP bytes. Detect that state and give the user actionable advice instead of
    // a confusing "Invalid data found when processing input" error.
    if (mSourceData.size() >= 4)
    {
        const uint8_t* p = mSourceData.data();
        const bool isPcv1 = (p[0] == 'P' && p[1] == 'C' && p[2] == 'V' && p[3] == '1');
        const bool isThp  = (p[0] == 'T' && p[1] == 'H' && p[2] == 'P' && p[3] == '\0');
        const bool isN3mv = (p[0] == 'N' && p[1] == '3' && p[2] == 'M' && p[3] == 'V');
        if (isPcv1 || isThp || isN3mv)
        {
            const char* what = isPcv1 ? "PCV1" : (isThp ? "THP" : "N3MV");
            LogError("VideoClip::TestCookForPlatform: '%s' is already cooked to %s bytes — "
                     "re-import the original source video to test cook again. "
                     "(TestCook overwrites the source on success; this is intentional so the "
                     "editor PIE can play the cooked output back through the runtime decoder.)",
                     mName.c_str(), what);
            return false;
        }
    }

    VideoPlayerAddon::DolphinCookParams params;
    params.width            = mCookWidth;
    params.height           = mCookHeight;
    params.fps              = mCookFps;
    params.jpegQuality      = mCookJpegQuality;
    params.audioSampleRate  = mCookAudioRate;
    params.audioNumChannels = mCookAudioCh;

    const bool useThp = (mCookFormat == 1) &&
                        (platform == Platform::GameCube || platform == Platform::Wii);
    const bool useN3mv = (mCookFormat == 2) && (platform == Platform::N3DS);

    std::vector<uint8_t> cooked;
    std::string err;
    const char* cookName = "Dolphin";
    bool ok = false;
    if (useThp)
    {
        cookName = "THP";
        ok = VideoPlayerAddon::CookThpVideo(mSourceData, mCodecHint, platform, params, cooked, err);
    }
    else if (useN3mv)
    {
        cookName = "N3MV";
        ok = VideoPlayerAddon::CookN3dsMvdVideo(mSourceData, mCodecHint, platform, params, cooked, err);
    }
    else
    {
        ok = VideoPlayerAddon::CookDolphinVideo(mSourceData, mCodecHint, platform, params, cooked, err);
    }
    if (!ok)
    {
        LogError("VideoClip::TestCookForPlatform: %s cook failed for '%s': %s",
                 cookName, mName.c_str(), err.c_str());
        return false;
    }

    LogDebug("VideoClip::TestCookForPlatform: cooked '%s' to %zu bytes (was %zu source bytes)",
             mName.c_str(), cooked.size(), mSourceData.size());

    // Sidecar mode: write the cooked bytes to a file next to the .oct and store
    // its absolute path on the asset. mSourceData stays empty so the cooked
    // bytes live in exactly one place. Implicit from Cook Format: THP and N3MV
    // always sidecar (their runtime decoders implement file-streaming
    // Open(path) and the .cook.* artifact gets version-controlled next to the
    // .oct); PCV1 always inlines (its decoder doesn't support file streaming
    // yet and PCV1 is the dev-friendly "everything-in-one-file" format).
    const bool wantSidecar = (useThp || useN3mv);
    if (wantSidecar)
    {
        namespace fs = std::filesystem;
        const char* sidecarExt = useN3mv ? ".cook.n3mv" : ".cook.thp";

        // Drop the sidecar next to the .oct so it moves with the asset (rename,
        // copy, project reorganize all "just work"), so duplicate test copies
        // of the same source clip don't trample each others' sidecars in a
        // shared Intermediate dir, AND so it gets version-controlled with the
        // asset (Intermediate/ is gitignored). Falls back to Intermediate/
        // when we can't resolve the asset's on-disk path.
        std::string sidecarDir;
        std::string sidecarPath;
        AssetStub* stub = FetchAssetStub(mName);
        if (stub != nullptr && !stub->mPath.empty())
        {
            sidecarDir  = Asset::GetDirectoryFromPath(stub->mPath);
            sidecarPath = sidecarDir + mName + sidecarExt;
        }
        else
        {
            sidecarDir  = GetEngineState()->mProjectDirectory + "Intermediate/VideoClipSidecar/";
            // Build an asset-name-safe filename for the fallback dir (since multiple
            // assets share that namespace, sanitize for fs-unfriendly characters).
            std::string safeName = mName;
            for (char& c : safeName)
            {
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-';
                if (!ok) c = '_';
            }
            sidecarPath = sidecarDir + safeName + sidecarExt;
        }

        std::error_code ec;
        fs::create_directories(sidecarDir, ec);
        FILE* f = fopen(sidecarPath.c_str(), "wb");
        if (f == nullptr)
        {
            LogError("VideoClip::TestCookForPlatform: failed to open sidecar for write: %s", sidecarPath.c_str());
            return false;
        }
        const size_t wrote = fwrite(cooked.data(), 1, cooked.size(), f);
        fclose(f);
        if (wrote != cooked.size())
        {
            LogError("VideoClip::TestCookForPlatform: short write on sidecar (%zu/%zu)", wrote, cooked.size());
            return false;
        }

        mSidecarPath = sidecarPath;
        mSourceData.clear();
        mSourceData.shrink_to_fit();
        mCodecHint.clear();
        LogDebug("VideoClip::TestCookForPlatform: sidecar written to %s (%zu bytes)",
                 sidecarPath.c_str(), cooked.size());
        return true;
    }

    mSourceData = std::move(cooked);
    mSidecarPath.clear();
    // Clear codec hint so VideoDecoderFactory doesn't try to route by extension —
    // the runtime detects PCV1 magic in the bytes themselves.
    mCodecHint.clear();
    return true;
#else
    (void)platform;
    return false;
#endif
}

glm::vec4 VideoClip::GetTypeColor()
{
    return glm::vec4(0.1f, 0.65f, 0.85f, 1.0f);
}

const char* VideoClip::GetTypeName()
{
    return "VideoClip";
}

const char* VideoClip::GetTypeImportExt()
{
    return ".mp4";
}
