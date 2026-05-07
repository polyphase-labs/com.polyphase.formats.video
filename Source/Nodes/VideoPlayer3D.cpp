#include "Nodes/VideoPlayer3D.h"

#include "Backends/IVideoDecoder.h"
#include "Backends/VideoDecoderFactory.h"
#include "Backends/AsyncMediaPump.h"
#include "Backends/ThpVideoDecoder.h"
#include "Backends/MvdVideoDecoder.h"

#include <cstring>
#include "Util/AlphaUnpack.h"
#include "EngineAPIAccess.h"
#include "Assets/VideoClip.h"

#include "Engine.h"
#include "Log.h"
#include "Assets/Texture.h"
#include "Datum.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include <algorithm>

FORCE_LINK_DEF(VideoPlayer3D);
DEFINE_NODE(VideoPlayer3D, Node3D);

static const char* sAlphaModeStrings[] =
{
    "None",
    "RGBA",
    "Top Bottom Split",
};
static_assert(int32_t(VideoAlphaMode::Count) == 3, "Need to update VideoAlphaMode string table");

bool VideoPlayer3D::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    VideoPlayer3D* vp = static_cast<VideoPlayer3D*>(prop->mOwner);
    bool handled = false;

    if (prop->mName == "File Path")
    {
        const std::string* s = reinterpret_cast<const std::string*>(newValue);
        vp->SetFilePath(*s);
        handled = true;
    }
    else if (prop->mName == "Video Clip")
    {
        // newValue points at the new Asset* the inspector is assigning.
        Asset* a = *reinterpret_cast<Asset* const*>(newValue);
        vp->SetVideoClip(static_cast<VideoClip*>(a));
        handled = true;
    }
    else if (prop->mName == "Alpha Mode")
    {
        vp->SetAlphaMode(*(VideoAlphaMode*)newValue);
        handled = true;
    }
    else if (prop->mName == "Audio Enabled")
    {
        vp->SetAudioEnabled(*(bool*)newValue);
        handled = true;
    }
    else if (prop->mName == "Volume")
    {
        vp->SetVolume(*(float*)newValue);
        handled = true;
    }
    else if (prop->mName == "Play")
    {
        if (*(bool*)newValue)
            vp->Play();
        else
            vp->Stop();
        handled = true;
    }

    return handled;
}

VideoPlayer3D::VideoPlayer3D()
{
    mName = "VideoPlayer";
}

VideoPlayer3D::~VideoPlayer3D()
{
    CloseVideo();
    if (mTexture != nullptr)
    {
        delete mTexture;
        mTexture = nullptr;
    }
}

const char* VideoPlayer3D::GetTypeName() const
{
    return "VideoPlayer";
}

void VideoPlayer3D::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);

    SCOPED_CATEGORY("Video");

    outProps.push_back(Property(DatumType::Bool, "Play", this, &mPlaying, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Asset, "Video Clip", this, &mVideoClip, 1, HandlePropChange,
                                int32_t(VideoClip::GetStaticType())));
    outProps.push_back(Property(DatumType::String, "File Path", this, &mFilePath, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "Auto Play", this, &mAutoPlay));
    outProps.push_back(Property(DatumType::Bool, "Loop", this, &mLoop));
    outProps.push_back(Property(DatumType::Float, "Playback Speed", this, &mPlaybackSpeed));
    outProps.push_back(Property(DatumType::Integer, "Alpha Mode", this, &mAlphaMode, 1, HandlePropChange,
                                NULL_DATUM, int32_t(VideoAlphaMode::Count), sAlphaModeStrings));
    outProps.push_back(Property(DatumType::Bool, "Audio Enabled", this, &mAudioEnabled, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Float, "Volume", this, &mVolume, 1, HandlePropChange));
}

void VideoPlayer3D::Create()
{
    Node3D::Create();
}

void VideoPlayer3D::Destroy()
{
    CloseVideo();
    if (mTexture != nullptr)
    {
        delete mTexture;
        mTexture = nullptr;
    }
    Node3D::Destroy();
}

void VideoPlayer3D::Start()
{
    Node3D::Start();

    mStarted = true;

    if (mVideoClip.Get() != nullptr || !mFilePath.empty())
    {
        if (OpenVideo())
        {
            // Defer OnReady + auto-Play to the first Tick. Script nodes wired up as
            // signal listeners connect during their own Start(), which may run after
            // ours. Emitting here would fire into an empty listener list.
            mPendingReady = true;
            mPendingAutoPlay = mAutoPlay;
        }
    }
}

void VideoPlayer3D::Tick(float deltaTime)
{
    Node3D::Tick(deltaTime);

    if (mPendingReady)
    {
        mPendingReady = false;
        EmitSignal("OnReady", {});
        if (mPendingAutoPlay)
        {
            mPendingAutoPlay = false;
            Play();
        }
    }

    if (!mPlaying || !mReady || mPump == nullptr || mTexture == nullptr)
    {
        return;
    }

    // Surface worker-thread errors on the main thread so Lua listeners receive OnError
    // (EmitSignal is main-thread-only; the pump sets an atomic flag the worker can raise).
    if (mPump->HasError())
    {
        EmitErrorSignal("Decoder error during playback");
        mPlaying = false;
        return;
    }

    // --- Audio: drain the pump's audio queue and feed the engine streaming voice ---
    PolyphaseEngineAPI* apiForAudio = VideoPlayerAddon::GetEngineAPI();
    if (mAudioStreamId != 0 && apiForAudio != nullptr && apiForAudio->Audio_SubmitStreamBuffer != nullptr)
    {
        // First, retry any chunk we stashed last tick because the engine queue was full.
        if (mPendingAudioRetry.valid)
        {
            const int32_t accepted = apiForAudio->Audio_SubmitStreamBuffer(
                mAudioStreamId, mPendingAudioRetry.samples.data(),
                uint32_t(mPendingAudioRetry.samples.size()));
            if (accepted > 0)
            {
                mSubmittedAudioSec += double(mPendingAudioRetry.sampleCount) / double(mAudioSampleRate);
                mPendingAudioRetry.valid = false;
                mPendingAudioRetry.samples.clear();
            }
            // If still rejected, keep holding it and skip draining this tick.
        }

        VideoPlayerAddon::AsyncMediaPump::AudioChunk chunk;
        while (!mPendingAudioRetry.valid && mPump->TryPopAudioChunk(chunk))
        {
            if (chunk.samples.empty() || mAudioSampleRate == 0) continue;
            const int32_t accepted = apiForAudio->Audio_SubmitStreamBuffer(
                mAudioStreamId, chunk.samples.data(),
                uint32_t(chunk.samples.size()));
            if (accepted <= 0)
            {
                // XAudio2 queue is full. Stash the chunk and try again next tick — losing
                // it would create compounding audio gaps that audibly speed up playback.
                mPendingAudioRetry.samples     = std::move(chunk.samples);
                mPendingAudioRetry.sampleCount = chunk.sampleCount;
                mPendingAudioRetry.valid       = true;
                break;
            }
            mSubmittedAudioSec += double(chunk.sampleCount) / double(mAudioSampleRate);
        }
    }

    // Prebuffer gate: unpause the voice only once we've accumulated enough audio in
    // XAudio2's queue that the initial output won't underrun. Before this kicks in, we
    // hold the playhead so the video doesn't drift ahead of silent audio.
    if (mAudioStreamId != 0 && !mVoiceUnpaused)
    {
        const bool decoderHasAudio = mPump->GetDecoder() != nullptr && mPump->GetDecoder()->HasAudio();
        if (!decoderHasAudio || mSubmittedAudioSec >= kPrebufferSec || mPump->IsEndOfStream())
        {
            if (apiForAudio != nullptr && apiForAudio->Audio_SetStreamPaused != nullptr)
            {
                apiForAudio->Audio_SetStreamPaused(mAudioStreamId, false);
            }
            mVoiceUnpaused = true;
        }
        else
        {
            // Still prebuffering — hold the playhead at the clock base so the video
            // pump stays parked on the first frame until audio is ready to start.
            mPlayheadSec = mClockBaseSec;
            return;
        }
    }

    // --- Advance playhead ---
    // Audio-as-master clock: the video follows the samples-played counter from the engine
    // streaming voice. Falls back to wall-clock advancement when audio is unavailable
    // (no stream opened, no audio in file, or platform lacks streaming audio).
    bool usedAudioClock = false;
    if (mAudioStreamId != 0 && mAudioSampleRate > 0)
    {
        PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
        if (api != nullptr && api->Audio_GetStreamPlayedSamples != nullptr)
        {
            const uint64_t rawSamples = api->Audio_GetStreamPlayedSamples(mAudioStreamId);
            // Subtract the last seek/loop snapshot so the clock is relative to the current
            // play segment. XAudio2's SamplesPlayed is cumulative since voice creation.
            const uint64_t samplesSinceBase = (rawSamples >= mAudioSamplesAtBase)
                ? (rawSamples - mAudioSamplesAtBase) : 0;
            if (samplesSinceBase > 0 || mAudioStarted)
            {
                mAudioStarted = true;
                mPlayheadSec  = mClockBaseSec + double(samplesSinceBase) / double(mAudioSampleRate);
                usedAudioClock = true;
            }
            // else: hold the playhead at mClockBaseSec until audio actually starts outputting.
        }
    }
    if (!usedAudioClock)
    {
        mPlayheadSec += double(deltaTime) * double(mPlaybackSpeed);
    }

    // Catch-up-aware pop: the pump keeps a small FIFO of decoded frames. We request
    // every frame whose pts <= playhead + half-frame-tolerance, discarding all but the
    // newest. This way a brief audio-clock hiccup that skips us past several frames
    // resolves to ONE present of the most-recent in-time frame, not a rapid flip through
    // each stale intermediate frame (which looked like "skip back then skip forward").
    //
    // The tolerance is half a typical frame period so we don't display frames more than
    // ~half a frame before their ideal time.
    const double kPopTolerance = 0.020; // 20 ms — half of a 24fps frame
    VideoPlayerAddon::AsyncMediaPump::VideoFrameSlot slot;
    if (mPump->TryPopDueVideoFrame(mPlayheadSec + kPopTolerance, slot))
    {
        if (mAlphaMode == VideoAlphaMode::TopBottomSplit)
        {
            VideoPlayerAddon::UnpackTopBottomAlpha(slot.pixels.data(), mSourceWidth, mSourceHeight, mUnpackScratch);
            if (!mUnpackScratch.empty())
            {
                mTexture->UpdatePixels(mUnpackScratch.data(), mUnpackScratch.size());
            }
        }
        else
        {
            mTexture->UpdatePixels(slot.pixels.data(), slot.pixels.size());
        }
        mNextFrameSec = slot.ptsSeconds;
    }

    // EOS is flagged by the worker after the final frame; present-side catches up here.
    // Hold for a full frame period past the last frame's PTS so it gets the same on-screen
    // lifetime as every other frame — otherwise the loop fires the instant the last frame
    // is popped and it visibly flashes by in ~1 tick instead of ~1/fps.
    const double finalFrameHold = (mFrameRate > 0.0) ? (1.0 / mFrameRate) : 0.0;
    if (mPump->IsEndOfStream() && mPlayheadSec >= mNextFrameSec + finalFrameHold)
    {
        HandleEndOfStream();
    }
}

void VideoPlayer3D::EditorTick(float deltaTime)
{
    Node3D::EditorTick(deltaTime);
    // In the editor, show the first frame (if any) but don't advance playback.
}

void VideoPlayer3D::Play()
{
    if (!mReady)
    {
        // Late Play() called before Start()? Try to open now if we have a clip or path.
        if ((mVideoClip.Get() == nullptr && mFilePath.empty()) || !OpenVideo())
        {
            return;
        }
    }
    if (!mPlaying)
    {
        mPlaying = true;

        PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
        // Only directly unpause the voice when it has already been started once since the
        // current flush boundary (i.e. we're resuming a previously-playing stream). Before
        // that, the Tick prebuffer gate decides when to unpause — avoids kicking off audio
        // with only ~20ms queued which would underrun and throw off the audio-master clock.
        if (mAudioStreamId != 0 && api != nullptr && api->Audio_SetStreamPaused != nullptr && mVoiceUnpaused)
        {
            api->Audio_SetStreamPaused(mAudioStreamId, false);
        }
        EmitSignal("OnPlay", {});
    }
}

void VideoPlayer3D::Pause()
{
    if (mPlaying)
    {
        mPlaying = false;

        PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
        if (mAudioStreamId != 0 && api != nullptr && api->Audio_SetStreamPaused != nullptr)
        {
            api->Audio_SetStreamPaused(mAudioStreamId, true);
        }
        EmitSignal("OnPause", {});
    }
}

void VideoPlayer3D::Stop()
{
    const bool wasPlaying = mPlaying;
    mPlaying = false;
    mPlayheadSec = 0.0;
    mNextFrameSec = 0.0;
    mAudioStarted = false;
    mVoiceUnpaused = false;        // Force re-prebuffer on next Play.
    mSubmittedAudioSec = 0.0;
    mPendingAudioRetry.valid = false;
    mPendingAudioRetry.samples.clear();
    if (mPump != nullptr)
    {
        mPump->RequestSeek(0.0);
    }
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mAudioStreamId != 0 && api != nullptr)
    {
        if (api->Audio_FlushStream != nullptr)
        {
            api->Audio_FlushStream(mAudioStreamId);
        }
        if (api->Audio_SetStreamPaused != nullptr)
        {
            api->Audio_SetStreamPaused(mAudioStreamId, true);
        }
        // Re-snapshot the samples-played base at the flush point so the next Play() starts
        // the clock from the new position.
        if (api->Audio_GetStreamPlayedSamples != nullptr)
        {
            mAudioSamplesAtBase = api->Audio_GetStreamPlayedSamples(mAudioStreamId);
        }
        mClockBaseSec = 0.0;
    }
    if (wasPlaying)
    {
        EmitSignal("OnPause", {});
    }
}

void VideoPlayer3D::Seek(double seconds)
{
    if (!mReady || mPump == nullptr)
    {
        return;
    }
    mPump->RequestSeek(seconds);
    mPlayheadSec = seconds;
    mNextFrameSec = seconds;
    mAudioStarted = false;
    mVoiceUnpaused = false;        // Re-prebuffer after seek.
    mSubmittedAudioSec = 0.0;
    mPendingAudioRetry.valid = false;
    mPendingAudioRetry.samples.clear();
    // Discard any audio still queued for the pre-seek playhead and rebase the sample clock.
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mAudioStreamId != 0 && api != nullptr)
    {
        if (api->Audio_FlushStream != nullptr)
        {
            api->Audio_FlushStream(mAudioStreamId);
        }
        if (api->Audio_SetStreamPaused != nullptr)
        {
            // Pause the voice during the post-seek prebuffer window.
            api->Audio_SetStreamPaused(mAudioStreamId, true);
        }
        if (api->Audio_GetStreamPlayedSamples != nullptr)
        {
            mAudioSamplesAtBase = api->Audio_GetStreamPlayedSamples(mAudioStreamId);
        }
        mClockBaseSec = seconds;
    }
}

double VideoPlayer3D::GetDuration() const
{
    return mDurationSec;
}

void VideoPlayer3D::SetFilePath(const std::string& path)
{
    if (path == mFilePath) return;

    mFilePath = path;

    if (mStarted)
    {
        CloseVideo();
        if ((mVideoClip.Get() != nullptr || !mFilePath.empty()) && OpenVideo())
        {
            // Defer OnReady one tick so listeners set up via ConnectSignal earlier still fire.
            mPendingReady = true;
            mPendingAutoPlay = mAutoPlay;
        }
    }
}

void VideoPlayer3D::SetVideoClip(VideoClip* clip)
{
    if (mVideoClip.Get() == clip) return;

    mVideoClip = clip;

    if (mStarted)
    {
        CloseVideo();
        if ((mVideoClip.Get() != nullptr || !mFilePath.empty()) && OpenVideo())
        {
            mPendingReady = true;
            mPendingAutoPlay = mAutoPlay;
        }
    }
}

VideoClip* VideoPlayer3D::GetVideoClip() const
{
    return mVideoClip.Get<VideoClip>();
}

void VideoPlayer3D::SetAlphaMode(VideoAlphaMode m)
{
    if (m == mAlphaMode) return;

    mAlphaMode = m;

    // Resizing the output texture requires reopening the video. Only do it if started.
    if (mStarted && mReady)
    {
        CloseVideo();
        if (OpenVideo())
        {
            mPendingReady = true;
        }
    }
}

void VideoPlayer3D::SetAudioEnabled(bool enabled)
{
    if (enabled == mAudioEnabled) return;
    mAudioEnabled = enabled;

    // Mid-playback toggle: close or open the audio stream to match. Video keeps playing.
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mReady && api != nullptr)
    {
        if (!enabled && mAudioStreamId != 0 && api->Audio_CloseStream != nullptr)
        {
            api->Audio_CloseStream(mAudioStreamId);
            mAudioStreamId = 0;
            mAudioStarted = false;
        }
        else if (enabled && mAudioStreamId == 0 && mPump != nullptr &&
                 mPump->GetDecoder() != nullptr && mPump->GetDecoder()->HasAudio() &&
                 api->Audio_OpenStream != nullptr)
        {
            const VideoPlayerAddon::AudioStreamDesc desc = mPump->GetDecoder()->GetAudioDesc();
            mAudioStreamId = api->Audio_OpenStream(desc.sampleRate, desc.numChannels, desc.bitsPerSample);
            if (mAudioStreamId != 0)
            {
                mAudioSampleRate  = desc.sampleRate;
                mAudioNumChannels = desc.numChannels;
                if (api->Audio_SetStreamVolume != nullptr)
                    api->Audio_SetStreamVolume(mAudioStreamId, mVolume);
            }
        }
    }
}

void VideoPlayer3D::SetVolume(float v)
{
    mVolume = std::max(0.0f, std::min(1.0f, v));
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mAudioStreamId != 0 && api != nullptr && api->Audio_SetStreamVolume != nullptr)
    {
        api->Audio_SetStreamVolume(mAudioStreamId, mVolume);
    }
}

namespace
{
    bool IsAbsolutePath(const std::string& p)
    {
        if (p.empty()) return false;
        // Windows: "C:/..." or "\\server\share\..."
        if (p.size() >= 2 && p[1] == ':') return true;
        if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return true;
        // Unix-style absolute (also covers forward-slash roots on Windows for consistency)
        if (p[0] == '/') return true;
        return false;
    }

    std::string ResolveAssetPath(const std::string& rawPath)
    {
        if (IsAbsolutePath(rawPath)) return rawPath;

        const std::string& projectDir = GetEngineState()->mProjectDirectory;
        if (projectDir.empty()) return rawPath;

        // mProjectDirectory already ends with a separator in the engine's conventions,
        // but guard against the edge case where it doesn't.
        std::string base = projectDir;
        if (!base.empty() && base.back() != '/' && base.back() != '\\')
        {
            base.push_back('/');
        }
        return base + rawPath;
    }
}

bool VideoPlayer3D::OpenVideo()
{
    CloseVideo();

    VideoClip* clip = mVideoClip.Get<VideoClip>();
    if (clip == nullptr && mFilePath.empty())
    {
        EmitErrorSignal("No video clip or file path set");
        return false;
    }

    std::unique_ptr<VideoPlayerAddon::IVideoDecoder> decoder;
    if (clip != nullptr)
    {
        // VideoClip path: decode either from the asset's inline source bytes
        // (default) or stream from a sidecar file on disk (when the asset has
        // mUseSidecar set + a sidecar path written by TestCook). The sidecar
        // lazy-IO path drops peak memory during playback from clip-size to
        // ~150 KB, important for longer clips on GameCube's 24 MB RAM.
        // Presence of mSidecarPath is the authoritative "did the cook produce a
        // sidecar?" signal. THP/N3MV cooks set it; PCV1 (and uncooked imports)
        // leave it empty.
        const bool useSidecar = !clip->GetSidecarPath().empty();

        if (useSidecar)
        {
            // For sidecar mode the factory has no bytes to peek at — pick the
            // decoder by file extension. .cook.thp -> ThpVideoDecoder,
            // .cook.n3mv -> MvdVideoDecoder. Anything else is unexpected;
            // fall through to factory-by-magic which will attempt OpenMemory
            // and fail cleanly.
            const std::string& sp = clip->GetSidecarPath();
            const auto endsWith = [&](const char* suffix) {
                const size_t n = std::strlen(suffix);
                return sp.size() >= n && sp.compare(sp.size() - n, n, suffix) == 0;
            };
            if (endsWith(".cook.thp"))
            {
                decoder = std::unique_ptr<VideoPlayerAddon::IVideoDecoder>(new VideoPlayerAddon::ThpVideoDecoder());
            }
            else if (endsWith(".cook.n3mv"))
            {
                decoder = std::unique_ptr<VideoPlayerAddon::IVideoDecoder>(new VideoPlayerAddon::MvdVideoDecoder());
            }
            else
            {
                decoder = VideoPlayerAddon::CreateVideoDecoderForClip(clip);
            }
        }
        else
        {
            decoder = VideoPlayerAddon::CreateVideoDecoderForClip(clip);
        }
        if (decoder == nullptr)
        {
            EmitErrorSignal("No decoder backend available for video clip");
            return false;
        }

        bool opened = false;
        if (useSidecar)
        {
            opened = decoder->Open(clip->GetSidecarPath().c_str());
            if (!opened)
            {
                LogError("VideoPlayer3D: sidecar Open failed for '%s' at '%s' — "
                         "falling back to inline bytes if any",
                         clip->GetName().c_str(), clip->GetSidecarPath().c_str());
            }
        }
        if (!opened)
        {
            const auto& bytes = clip->GetSourceData();
            opened = decoder->OpenMemory(bytes.data(), bytes.size(), clip->GetCodecHint().c_str());
        }
        if (!opened)
        {
            LogError("VideoPlayer3D: decoder open failed for VideoClip '%s' (sidecar=%d, bytes=%u)",
                     clip->GetName().c_str(), useSidecar ? 1 : 0, clip->GetSourceSize());
            EmitErrorSignal("Decoder failed to open video clip");
            return false;
        }
    }
    else
    {
        // Legacy file-path mode. The inspector-set path is relative to the project
        // directory; FFmpeg wants a real OS path. Resolve here so the player is
        // robust to whatever CWD the editor launched with.
        const std::string resolvedPath = ResolveAssetPath(mFilePath);

        decoder = VideoPlayerAddon::CreateVideoDecoder(resolvedPath);
        if (decoder == nullptr)
        {
            EmitErrorSignal("No decoder backend available for file");
            return false;
        }

        if (!decoder->Open(resolvedPath.c_str()))
        {
            LogError("VideoPlayer3D: decoder open failed for resolved path '%s' (raw '%s')",
                     resolvedPath.c_str(), mFilePath.c_str());
            EmitErrorSignal("Decoder failed to open file");
            return false;
        }
    }

    const VideoPlayerAddon::VideoFrameDesc desc = decoder->GetFrameDesc();
    mSourceWidth = desc.width;
    mSourceHeight = desc.height;

    if (mSourceWidth == 0 || mSourceHeight == 0)
    {
        EmitErrorSignal("Decoder reported zero dimensions");
        return false;
    }

    mOutputWidth = mSourceWidth;
    mOutputHeight = mSourceHeight;
    if (mAlphaMode == VideoAlphaMode::TopBottomSplit)
    {
        if ((mSourceHeight & 1u) != 0)
        {
            EmitErrorSignal("Top/Bottom split alpha requires even source height");
            return false;
        }
        mOutputHeight = mSourceHeight / 2;
    }

    // Cache decoder-side descriptors so the main thread can answer queries without
    // touching the worker-owned decoder once the pump is running.
    mDurationSec = decoder->GetDurationSeconds();
    mFrameRate   = decoder->GetFrameRate();

    // Reuse the existing streaming texture if dimensions/format match — this is the
    // common case when SetFilePath swaps to another clip of the same resolution (e.g.
    // an attract sequence's intro/loop/outro). Reusing keeps consumers' cached
    // Texture* pointers stable AND keeps the previous clip's last frame on screen
    // until the new pump's first frame is uploaded, so the swap looks like a clean
    // freeze-and-replace instead of flashing the no-texture fallback (white).
    const bool needNewTexture =
        (mTexture == nullptr) ||
        (int32_t(mTexture->GetWidth()) != mOutputWidth) ||
        (int32_t(mTexture->GetHeight()) != mOutputHeight) ||
        (mTexture->GetFormat() != PixelFormat::RGBA8);

    if (needNewTexture)
    {
        if (mTexture != nullptr)
        {
            delete mTexture;
            mTexture = nullptr;
        }
        // Seed a fresh streaming texture with opaque black so the first uploaded
        // frame doesn't get blended against undefined memory.
        std::vector<uint8_t> zeros(size_t(mOutputWidth) * mOutputHeight * 4, 0);
        for (size_t i = 3; i < zeros.size(); i += 4) zeros[i] = 255;

        mTexture = new Texture();
        mTexture->SetMipmapped(false);
        mTexture->SetFilterType(FilterType::Linear);
        mTexture->SetWrapMode(WrapMode::Clamp);
        mTexture->SetFormat(PixelFormat::RGBA8);
        mTexture->Init(mOutputWidth, mOutputHeight, zeros.data());
        mTexture->Create();
    }

    // Capture audio descriptor before handing the decoder to the pump — once the pump
    // starts, the worker owns the decoder and direct queries would race.
    const bool hadAudio = decoder->HasAudio();
    VideoPlayerAddon::AudioStreamDesc audioDesc = hadAudio ? decoder->GetAudioDesc() : VideoPlayerAddon::AudioStreamDesc{};

    // Hand the decoder to the async pump. The pump owns it for the rest of its life.
    mPump = std::unique_ptr<VideoPlayerAddon::AsyncMediaPump>(new VideoPlayerAddon::AsyncMediaPump());
    if (!mPump->Start(std::move(decoder)))
    {
        EmitErrorSignal("Failed to start async media pump");
        mPump.reset();
        return false;
    }

    // Open an engine streaming voice for the audio track, if present and enabled.
    mAudioStreamId      = 0;
    mAudioSampleRate    = 0;
    mAudioNumChannels   = 0;
    mAudioStarted       = false;
    mAudioSamplesAtBase = 0;
    mClockBaseSec       = 0.0;
    mVoiceUnpaused      = false;
    mSubmittedAudioSec  = 0.0;

    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mAudioEnabled && hadAudio && api != nullptr && api->Audio_OpenStream != nullptr)
    {
        mAudioStreamId = api->Audio_OpenStream(audioDesc.sampleRate, audioDesc.numChannels, audioDesc.bitsPerSample);
        if (mAudioStreamId != 0)
        {
            mAudioSampleRate  = audioDesc.sampleRate;
            mAudioNumChannels = audioDesc.numChannels;
            if (api->Audio_SetStreamVolume != nullptr)
                api->Audio_SetStreamVolume(mAudioStreamId, mVolume);
            // Start paused — Play() unpauses. Prevents SamplesPlayed advancing before
            // the game logic is ready.
            if (api->Audio_SetStreamPaused != nullptr)
                api->Audio_SetStreamPaused(mAudioStreamId, true);
        }
    }
    else if (mAudioEnabled && !hadAudio)
    {
        // Informational: notify listeners that the file has no audio track (not an error,
        // the video still plays silently).
        EmitSignal("OnAudioMissing", {});
    }

    mPlayheadSec = 0.0;
    mNextFrameSec = 0.0;
    mReady = true;

    return true;
}

void VideoPlayer3D::CloseVideo()
{
    mPlaying = false;
    mReady = false;
    mPlayheadSec = 0.0;
    mNextFrameSec = 0.0;
    mDurationSec = 0.0;
    mFrameRate = 30.0;
    mAudioStarted = false;

    // Close the engine audio stream before stopping the pump — the pump's audio queue
    // lives inside the pump; once it's destroyed we can't submit any more chunks, and
    // the engine stream will drain on its own via the voice callback freeing the last
    // queued buffers.
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (mAudioStreamId != 0 && api != nullptr && api->Audio_CloseStream != nullptr)
    {
        api->Audio_CloseStream(mAudioStreamId);
    }
    mAudioStreamId    = 0;
    mAudioSampleRate  = 0;
    mAudioNumChannels = 0;

    if (mPump != nullptr)
    {
        // Stop joins the worker thread and destroys the decoder it owned.
        mPump->Stop();
        mPump.reset();
    }

    // NOTE: mTexture is intentionally NOT deleted here. It survives a SetFilePath
    // swap so that (a) consumers caching the Texture* keep a live pointer, and
    // (b) the screen continues to show the previous clip's last frame instead
    // of the no-texture fallback while the new pump decodes its first frame.
    // Final teardown happens in ~VideoPlayer3D() and Destroy().

    mUnpackScratch.clear();
    mUnpackScratch.shrink_to_fit();

    mSourceWidth = mSourceHeight = 0;
    mOutputWidth = mOutputHeight = 0;
}

void VideoPlayer3D::HandleEndOfStream()
{
    if (mLoop)
    {
        mPlayheadSec = 0.0;
        mNextFrameSec = 0.0;
        mAudioStarted = false;
        mVoiceUnpaused = false;        // Re-prebuffer on loop.
        mSubmittedAudioSec = 0.0;
        mPendingAudioRetry.valid = false;
        mPendingAudioRetry.samples.clear();
        if (mPump != nullptr)
        {
            mPump->RequestSeek(0.0);
        }
        // Flush the engine audio stream so the last few seconds of pre-loop audio don't
        // keep playing after the video has looped back. Re-snapshot the samples-played
        // base so the audio clock restarts from 0 on the new segment.
        PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
        if (mAudioStreamId != 0 && api != nullptr)
        {
            if (api->Audio_FlushStream != nullptr)
            {
                api->Audio_FlushStream(mAudioStreamId);
            }
            if (api->Audio_SetStreamPaused != nullptr)
            {
                // Pause the voice during the post-loop prebuffer window.
                api->Audio_SetStreamPaused(mAudioStreamId, true);
            }
            if (api->Audio_GetStreamPlayedSamples != nullptr)
            {
                mAudioSamplesAtBase = api->Audio_GetStreamPlayedSamples(mAudioStreamId);
            }
            mClockBaseSec = 0.0;
        }
        EmitSignal("OnLoop", {});
    }
    else
    {
        mPlaying = false;
        EmitSignal("OnFinished", {});
    }
}

void VideoPlayer3D::EmitErrorSignal(const char* message)
{
    LogError("VideoPlayer3D: %s (%s)", message ? message : "error", mFilePath.c_str());
    std::vector<Datum> args;
    args.push_back(Datum(std::string(message ? message : "error")));
    EmitSignal("OnError", args);
}
