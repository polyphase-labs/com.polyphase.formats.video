#pragma once

#include "Nodes/3D/Node3d.h"
#include "AssetRef.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VideoClip;

namespace VideoPlayerAddon
{
    class IVideoDecoder;
    class AsyncMediaPump;
}

enum class VideoAlphaMode : uint8_t
{
    None = 0,          // Ignore alpha channel (fully opaque sampling)
    RGBA = 1,          // Use the decoded frame's alpha channel
    TopBottomSplit = 2,// Top half = RGB, bottom half = alpha (luminance)
    Count
};

class Texture;

// Node type defined in an addon DLL; use the engine's default visibility (no POLYPHASE_API,
// which would incorrectly mark the class as dllimport from Polyphase.dll on Windows).
class VideoPlayer3D : public Node3D
{
public:

    DECLARE_NODE(VideoPlayer3D, Node3D);

    VideoPlayer3D();
    virtual ~VideoPlayer3D();

    virtual const char* GetTypeName() const override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Start() override;
    virtual void Tick(float deltaTime) override;
    virtual void EditorTick(float deltaTime) override;

    // Control
    void Play();
    void Pause();
    void Stop();
    void Seek(double seconds);

    // Queries
    double GetTime() const { return mPlayheadSec; }
    double GetDuration() const;
    bool   IsPlaying() const { return mPlaying; }
    bool   IsReady() const   { return mReady; }

    // Configuration
    void SetFilePath(const std::string& path);
    const std::string& GetFilePath() const { return mFilePath; }

    // VideoClip-asset-driven playback (preferred over SetFilePath when both are set).
    // The clip's source bytes are decoded in-memory; nothing on disk is referenced.
    void SetVideoClip(VideoClip* clip);
    VideoClip* GetVideoClip() const;

    void SetAutoPlay(bool v) { mAutoPlay = v; }
    bool GetAutoPlay() const { return mAutoPlay; }

    void SetLoop(bool v) { mLoop = v; }
    bool GetLoop() const { return mLoop; }

    void SetPlaybackSpeed(float v) { mPlaybackSpeed = v; }
    float GetPlaybackSpeed() const { return mPlaybackSpeed; }

    void SetAlphaMode(VideoAlphaMode m);
    VideoAlphaMode GetAlphaMode() const { return mAlphaMode; }

    // Audio
    void SetAudioEnabled(bool enabled);
    bool IsAudioEnabled() const { return mAudioEnabled; }
    void SetVolume(float v);
    float GetVolume() const { return mVolume; }
    // True when the currently-open file has a decodable audio stream AND the engine
    // opened a streaming voice for it. False for video-only files or platforms where
    // streaming audio isn't implemented.
    bool HasAudioStream() const { return mAudioStreamId != 0; }

    // Output
    Texture* GetTexture() const { return mTexture; }

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    bool OpenVideo();  // (re)opens decoder and output texture based on mFilePath and mAlphaMode
    void CloseVideo(); // tears down decoder and output texture
    void HandleEndOfStream();
    void EmitErrorSignal(const char* message);

    // Properties (serialized / editor-exposed)
    std::string mFilePath;
    AssetRef    mVideoClip; // typed accessor: mVideoClip.Get<VideoClip>()
    bool mAutoPlay = false;
    bool mLoop = false;
    float mPlaybackSpeed = 1.0f;
    VideoAlphaMode mAlphaMode = VideoAlphaMode::None;
    bool mAudioEnabled = true;
    float mVolume = 1.0f;

    // State (transient)
    // Decoder now owned by the async pump. VideoPlayer3D talks to the pump; the pump runs
    // the decoder on a worker thread and hands off frames via a drop-oldest single-slot queue.
    std::unique_ptr<VideoPlayerAddon::AsyncMediaPump> mPump;
    // Cached decoder descriptors captured during OpenVideo so the main thread can answer
    // GetDuration/GetFrameDesc without touching the worker-owned decoder.
    double   mDurationSec = 0.0;
    double   mFrameRate = 30.0;
    Texture* mTexture = nullptr;       // owned by this node (not AssetManager)
    std::vector<uint8_t> mUnpackScratch; // only used when mAlphaMode == TopBottomSplit

    uint32_t mSourceWidth = 0;
    uint32_t mSourceHeight = 0;
    uint32_t mOutputWidth = 0;
    uint32_t mOutputHeight = 0;

    double mPlayheadSec = 0.0;
    double mNextFrameSec = 0.0;
    bool   mPlaying = false;
    bool   mReady = false;
    bool   mStarted = false;
    bool   mPendingReady = false;    // Emit OnReady on next Tick so script listeners wired during Start receive it.
    bool   mPendingAutoPlay = false; // Defer Play() alongside the deferred OnReady so listeners catch OnPlay too.

    // Audio playback state
    uint32_t mAudioStreamId = 0;    // 0 = not open; non-zero = engine streaming-voice handle
    uint32_t mAudioSampleRate = 0;  // captured from AsyncMediaPump::GetDecoder()->GetAudioDesc()
    uint32_t mAudioNumChannels = 0;
    bool     mAudioStarted = false; // set to true after the first audio sample is actually output
                                    // (samples-played transitions 0 -> >0); used to gate video-advance
                                    // during audio startup.
    // XAudio2's SamplesPlayed counter is cumulative since voice creation and cannot be
    // reset without destroying the voice. On every seek/loop we snapshot its current value
    // here and subtract on subsequent reads so the audio clock is seek-relative. mClockBaseSec
    // is the playhead time the snapshot corresponds to (usually 0 after Stop/loop; the seek
    // target after Seek).
    uint64_t mAudioSamplesAtBase = 0;
    double   mClockBaseSec = 0.0;
    // Prebuffer gate: the voice stays paused until we've submitted this much audio. Avoids
    // an underrun at the start of playback (and after every loop/seek) where the first
    // submitted chunk is ~20 ms, XAudio2 plays it then stalls waiting for more, and the
    // addon's audio-master clock goes out of sync while the decoder catches up.
    bool     mVoiceUnpaused = false;
    double   mSubmittedAudioSec = 0.0;  // total audio time submitted since last flush
    static constexpr double kPrebufferSec = 0.100;  // ~5 typical AAC frames

    // If Audio_SubmitStreamBuffer returns 0 (XAudio2 pending-buffer cap hit) we stash the
    // rejected chunk here and retry it on the next Tick. Dropping it would create audio
    // gaps that compound on long playback.
    struct PendingAudio { std::vector<uint8_t> samples; uint32_t sampleCount = 0; bool valid = false; };
    PendingAudio mPendingAudioRetry;
};
