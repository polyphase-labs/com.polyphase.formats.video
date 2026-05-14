#pragma once

#include "Nodes/3D/Node3d.h"
#include "Nodes/VideoPlayer3D.h"
#include "Playlist/Playlist.h"

#include <cstdint>
#include <string>
#include <vector>

namespace VideoPlayerAddon { class Playlist; }

// Transition strategy at clip boundaries. Auto picks NearSeamless on GameCube
// (single decoder, brief gap) and TrueSeamless elsewhere (dual-decoder
// prebuffer of the next clip ~500 ms before EOS).
enum class TransitionMode : int32_t
{
    Auto         = 0,
    NearSeamless = 1,
    TrueSeamless = 2,
    Count        = 3,
};

// Drag-and-drop playlist node. Source list comes from either:
//   - mPlaylistName (when non-empty AND registered): drives from
//     PlaylistRegistry. Lua mutations on that playlist propagate live.
//   - mInlineClipPaths: inline string-array exposed in the inspector.
//
// Owns 1-2 child VideoPlayer3D nodes (created in Create(), mHiddenInTree=true).
// The primary child does the actual playback; in TrueSeamless mode, a second
// child prebuffers the next clip ~500 ms before EOS so the swap on
// OnFinished is gap-free.
class VideoPlaylistPlayer : public Node3D
{
public:

    DECLARE_NODE(VideoPlaylistPlayer, Node3D);

    VideoPlaylistPlayer();
    virtual ~VideoPlaylistPlayer();

    virtual const char* GetTypeName() const override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Start() override;
    virtual void Tick(float deltaTime) override;

    // Transport
    void Play();
    void Stop();
    void Next();
    void Previous();
    void JumpTo(int32_t index);
    bool IsPlaying() const { return mPlaying; }
    int32_t GetCurrentIndex() const { return mCurrentIndex; }

    // Per-instance playlist mutation. Routes to the bound registry playlist
    // when mPlaylistName is non-empty AND registered; otherwise mutates the
    // inline list. Both paths invalidate the inline cache so the next
    // Advance() resolves fresh source data.
    int32_t AddVideo(const std::string& path);
    bool    RemoveVideo(int32_t itemId);
    void    ClearPlaylist();

    void                              SetPlaybackMode(VideoPlayerAddon::PlaylistMode m);
    VideoPlayerAddon::PlaylistMode    GetPlaybackMode() const;
    int32_t                           GetPlaylistCount() const;

    void               SetPlaylistName(const std::string& name);
    const std::string& GetPlaylistName() const { return mPlaylistName; }

    // Audio passthroughs to the active child(ren).
    void  SetAudioEnabled(bool enabled);
    bool  IsAudioEnabled() const { return mAudioEnabled; }
    void  SetVolume(float v);
    float GetVolume() const { return mVolume; }

    // Texture surface from the currently-active primary child. Use this in
    // VideoPlaylistConnector.lua to feed a Quad widget. Becomes a different
    // texture object on swap, so connector scripts should re-fetch on
    // OnPlaylistItemStarted.
    Texture* GetCurrentTexture() const;

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    // SignalHandlerFP receives the LISTENER (us). We disambiguate which child
    // finished by inspecting mPrimary at call time — we Disconnect/Reconnect
    // around swaps so this handler only ever fires for the active primary.
    static void OnPrimaryFinished(Node* listener, const std::vector<Datum>& args);

    // Dispatches signal both at the node level (for :ConnectSignal users) and
    // at the global level (for VideoPlayer.Connect users). Pass the logical
    // playlist name (mPlaylistName or empty) and the clip's display name (path
    // or VideoClip name).
    void EmitPlaylistEvent(const char* signalName, const std::string& assetName);

    // Builds mInlineCache from mInlineClipPaths (assigning monotonic ids).
    // Cheap; called whenever HandlePropChange touches the array or on first
    // Start().
    void RebuildInlineCache();

    // Returns the registry playlist when bound, otherwise nullptr (caller
    // should fall back to mInlineCache).
    VideoPlayerAddon::Playlist* ResolveRegistryPlaylist() const;

    // Returns the effective playlist mode: registry mode when bound, else the
    // node's own mPlaybackMode. Resolved per call so live Lua mode mutations
    // take effect at the next Advance().
    VideoPlayerAddon::PlaylistMode ResolveEffectiveMode() const;

    // Returns the effective transition mode after applying the GameCube
    // override on Auto. Computed compile-time on shipping platforms.
    TransitionMode ResolveEffectiveTransition() const;

    // Returns the source list size (registry or inline).
    size_t SourceCount() const;

    // Convenience: get the path/asset for the current clip at the given index.
    // playlistName is mPlaylistName (may be empty for inline-only nodes).
    bool GetItemAt(int32_t index, std::string& outPath, AssetRef& outClip) const;
    std::string GetItemDisplayName(int32_t index) const;

    // Loads `index` into a child VideoPlayer3D. Doesn't Play(); caller decides.
    void OpenOnChild(VideoPlayer3D* child, int32_t index);

    // Computes the next index per the active mode. Returns -1 when Sequential
    // has reached the end. *outWrapped reports if the chosen index wrapped
    // (used by Advance to fire OnPlaylistLooped).
    int32_t ComputeNextIndex(bool* outWrapped) const;

    // Fired by OnPrimaryFinished and by Stop()/Next()/Previous()/JumpTo().
    void Advance();

    // Disconnects mPrimary's OnFinished from us, then connects on `newPrimary`.
    // Used during the TrueSeamless swap.
    void RebindFinishedSignal(VideoPlayer3D* newPrimary);

    // ----- Inspector properties -----

    std::string              mPlaylistName;
    std::vector<std::string> mInlineClipPaths;
    int32_t                  mPlaybackMode    = (int32_t)VideoPlayerAddon::PlaylistMode::Sequential;
    int32_t                  mTransitionMode  = (int32_t)TransitionMode::Auto;
    bool                     mAutoStart       = true;
    bool                     mAudioEnabled    = true;
    float                    mVolume          = 1.0f;

    // ----- Runtime state -----

    VideoPlayer3D* mPrimary = nullptr;   // always non-null after Create()
    VideoPlayer3D* mNext    = nullptr;   // null in NearSeamless / GameCube
    int32_t        mCurrentIndex      = -1;
    bool           mPlaying           = false;
    bool           mPrebuffered       = false;  // true while mNext holds the upcoming clip
    int32_t        mPrebufferedIndex  = -1;     // index loaded into mNext (matches mPrebuffered)
    bool           mPrebufferedWrap   = false;  // captured Looped flag for the prebuffered choice
    bool           mForceNearSeamless = false;  // set after a TrueSeamless degrade event
    int32_t        mPendingPlay       = 0;      // counts down Tick frames before auto-play

    // Built from mInlineClipPaths; rebuilt on inspector edits + on first Start().
    std::vector<VideoPlayerAddon::PlaylistItem> mInlineCache;
    int32_t                                     mInlineNextItemId = 1;
};
