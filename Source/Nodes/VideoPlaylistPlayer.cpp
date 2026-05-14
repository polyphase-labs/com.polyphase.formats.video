#include "Nodes/VideoPlaylistPlayer.h"

#include "Playlist/PlaylistRegistry.h"
#include "Playlist/PlaylistEvents.h"
#include "Assets/VideoClip.h"

#include "Engine.h"
#include "Log.h"
#include "Datum.h"
#include "Property.h"

#include <algorithm>
#include <cstdlib>

FORCE_LINK_DEF(VideoPlaylistPlayer);
DEFINE_NODE(VideoPlaylistPlayer, Node3D);

using VideoPlayerAddon::PlaylistMode;
using VideoPlayerAddon::Playlist;
using VideoPlayerAddon::PlaylistItem;
using VideoPlayerAddon::PlaylistRegistry;
using VideoPlayerAddon::PlaylistEventDispatcher;
using VideoPlayerAddon::kPlaylistModeStrings;

static const char* sTransitionModeStrings[] =
{
    "Auto",
    "NearSeamless",
    "TrueSeamless",
};
static_assert((int)TransitionMode::Count == 3, "Update sTransitionModeStrings");

bool VideoPlaylistPlayer::HandlePropChange(Datum* datum, uint32_t /*index*/, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    auto* self = static_cast<VideoPlaylistPlayer*>(prop->mOwner);
    bool handled = false;

    if (prop->mName == "Play")
    {
        // "Play" is a fire-button toggle, mirroring VideoPlayer3D's pattern.
        // Don't store the bool; just invoke transport.
        if (*(bool*)newValue) self->Play();
        else                  self->Stop();
        handled = true;
    }
    else if (prop->mName == "Clip Paths")
    {
        // Datum has already written into mInlineClipPaths; rebuild the cache so
        // a subsequent Advance() sees the new list.
        self->RebuildInlineCache();
    }
    else if (prop->mName == "Playlist Name")
    {
        // Don't suppress storage — let Datum keep the new name. Resolution
        // happens lazily via ResolveRegistryPlaylist().
    }
    else if (prop->mName == "Audio Enabled")
    {
        self->SetAudioEnabled(*(bool*)newValue);
    }
    else if (prop->mName == "Volume")
    {
        self->SetVolume(*(float*)newValue);
    }

    return handled;
}

VideoPlaylistPlayer::VideoPlaylistPlayer()
{
    mName = "VideoPlaylistPlayer";
}

VideoPlaylistPlayer::~VideoPlaylistPlayer()
{
}

const char* VideoPlaylistPlayer::GetTypeName() const
{
    return "VideoPlaylistPlayer";
}

void VideoPlaylistPlayer::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);

    // SCOPED_CATEGORY expands to a local `scopedCat` var; wrap each section in
    // its own block so two macros don't collide as redefinitions.
    {
        SCOPED_CATEGORY("Playlist");

        // Storage points at mPlaying (the live state) so the inspector toggle
        // reflects actual playback even when Play()/Stop() is called from C++/Lua.
        // HandlePropChange suppresses storage and routes to transport instead.
        outProps.push_back(Property(DatumType::Bool, "Play", this, &mPlaying, 1, HandlePropChange));

        outProps.push_back(Property(DatumType::String, "Playlist Name", this, &mPlaylistName, 1, HandlePropChange));

        outProps.push_back(Property(DatumType::String, "Clip Paths", this, &mInlineClipPaths, 1, HandlePropChange)
                           .MakeVector(0, 64));

        outProps.push_back(Property(DatumType::Integer, "Playback Mode", this, &mPlaybackMode, 1, HandlePropChange,
                                    NULL_DATUM, (int32_t)PlaylistMode::Count, kPlaylistModeStrings));

        outProps.push_back(Property(DatumType::Integer, "Transition Mode", this, &mTransitionMode, 1, HandlePropChange,
                                    NULL_DATUM, (int32_t)TransitionMode::Count, sTransitionModeStrings));

        outProps.push_back(Property(DatumType::Bool,  "Auto Start",    this, &mAutoStart));
    }

    {
        SCOPED_CATEGORY("Audio");
        outProps.push_back(Property(DatumType::Bool,  "Audio Enabled", this, &mAudioEnabled, 1, HandlePropChange));
        outProps.push_back(Property(DatumType::Float, "Volume",        this, &mVolume,       1, HandlePropChange));
    }
}

void VideoPlaylistPlayer::Create()
{
    Node3D::Create();

    // Always spawn the primary. mNext is allocated lazily per the resolved
    // transition mode at Start() so a node that ends up NearSeamless on
    // GameCube doesn't pay the second decoder cost.
    mPrimary = CreateChild<VideoPlayer3D>("PlaylistPrimary");
    if (mPrimary != nullptr)
    {
#if EDITOR
        mPrimary->mHiddenInTree = true;
#endif
        mPrimary->SetTransient(true);
    }
}

void VideoPlaylistPlayer::Destroy()
{
    // Disconnect from the primary's signal before children are torn down.
    if (mPrimary != nullptr)
    {
        mPrimary->DisconnectSignal("OnFinished", this);
        mPrimary = nullptr;
    }
    if (mNext != nullptr)
    {
        mNext->DisconnectSignal("OnFinished", this);
        mNext = nullptr;
    }
    Node3D::Destroy();
}

void VideoPlaylistPlayer::Start()
{
    Node3D::Start();

    // Build the inline cache from the inspector array so the first Advance has
    // something to walk. Bound (registry) mode skips this and reads the registry
    // each time.
    if (mInlineCache.empty() && !mInlineClipPaths.empty())
    {
        RebuildInlineCache();
    }

    // Allocate the prebuffer child if the resolved transition mode wants it.
    // Compile-time pick on GameCube means mNext is never created there.
    const TransitionMode tm = ResolveEffectiveTransition();
    if (tm == TransitionMode::TrueSeamless && mNext == nullptr)
    {
        mNext = CreateChild<VideoPlayer3D>("PlaylistNext");
        if (mNext != nullptr)
        {
#if EDITOR
            mNext->mHiddenInTree = true;
#endif
            mNext->SetTransient(true);
            mNext->SetAudioEnabled(false);  // secondary stays silent until promoted
            mNext->SetVolume(mVolume);
        }
    }

    // Wire OnFinished from the primary back to us.
    if (mPrimary != nullptr)
    {
        mPrimary->ConnectSignal("OnFinished", this, &VideoPlaylistPlayer::OnPrimaryFinished);
        mPrimary->SetAudioEnabled(mAudioEnabled);
        mPrimary->SetVolume(mVolume);
    }

    // Defer auto-play one Tick so script listeners attached during their own
    // Start() are connected before OnPlaylistStarted fires. Mirrors VideoPlayer3D's
    // mPendingReady pattern.
    if (mAutoStart)
    {
        mPendingPlay = 1;
    }
}

void VideoPlaylistPlayer::Tick(float deltaTime)
{
    Node3D::Tick(deltaTime);

    if (mPendingPlay > 0)
    {
        --mPendingPlay;
        if (mPendingPlay == 0)
        {
            Play();
        }
    }

    // TrueSeamless prebuffer: when the primary is within ~500 ms of EOS, open
    // the upcoming clip on mNext and pause it. On the actual OnFinished swap,
    // the new primary's first frame is already decoded. Skip when the playlist
    // is one item or unbound, when degraded, or when already prebuffered.
    if (mPlaying && mNext != nullptr && !mPrebuffered && !mForceNearSeamless &&
        mPrimary != nullptr && mPrimary->IsReady())
    {
        const double dur     = mPrimary->GetDuration();
        const double playhead = mPrimary->GetTime();
        const double remaining = dur - playhead;
        if (dur > 0.0 && remaining > 0.0 && remaining < 0.5)
        {
            const size_t srcCount = SourceCount();
            if (srcCount > 1)
            {
                bool wrapped = false;
                int32_t nextIdx = ComputeNextIndex(&wrapped);
                if (nextIdx >= 0)
                {
                    OpenOnChild(mNext, nextIdx);
                    // Stop() on the secondary leaves it paused at frame 0 with
                    // its texture intact (VideoPlayer3D::CloseVideo preserves
                    // textures, Stop just halts advancement).
                    mNext->Stop();
                    mPrebuffered      = true;
                    mPrebufferedIndex = nextIdx;
                    mPrebufferedWrap  = wrapped;
                }
            }
        }
    }
}

// ---- Transport ----

void VideoPlaylistPlayer::Play()
{
    if (mPlaying || mPrimary == nullptr) return;

    const size_t count = SourceCount();
    if (count == 0)
    {
        LogWarning("VideoPlaylistPlayer::Play: source list is empty (PlaylistName='%s', inline=%zu)",
                   mPlaylistName.c_str(), mInlineClipPaths.size());
        return;
    }

    mCurrentIndex = 0;
    OpenOnChild(mPrimary, mCurrentIndex);
    mPlaying = true;

    const std::string firstName = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistStarted",     firstName);
    EmitPlaylistEvent("OnPlaylistItemStarted", firstName);

    mPrimary->Play();
}

void VideoPlaylistPlayer::Stop()
{
    if (!mPlaying) return;

    const std::string lastName = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistItemFinished", lastName);
    EmitPlaylistEvent("OnPlaylistEnded",        lastName);

    if (mPrimary != nullptr) mPrimary->Stop();
    mPlaying      = false;
    mCurrentIndex = -1;
    mPrebuffered  = false;
}

void VideoPlaylistPlayer::Next()
{
    if (!mPlaying || SourceCount() == 0) return;

    bool wrapped = false;
    int32_t nextIdx = ComputeNextIndex(&wrapped);

    const std::string outgoing = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistItemFinished", outgoing);

    if (nextIdx < 0)
    {
        // Sequential mode at end: synthesize an Ended event, mirroring Stop's
        // tail behavior so listeners get a clean shutdown.
        EmitPlaylistEvent("OnPlaylistEnded", outgoing);
        if (mPrimary != nullptr) mPrimary->Stop();
        mPlaying = false;
        mCurrentIndex = -1;
        mPrebuffered  = false;
        return;
    }

    mCurrentIndex = nextIdx;
    if (wrapped)
    {
        EmitPlaylistEvent("OnPlaylistLooped", GetItemDisplayName(mCurrentIndex));
    }

    OpenOnChild(mPrimary, mCurrentIndex);
    mPrebuffered = false;  // skipping invalidates any prebuffer
    EmitPlaylistEvent("OnPlaylistItemStarted", GetItemDisplayName(mCurrentIndex));
    if (mPrimary != nullptr) mPrimary->Play();
}

void VideoPlaylistPlayer::Previous()
{
    if (!mPlaying || SourceCount() == 0) return;

    const int32_t count = (int32_t)SourceCount();
    int32_t prev = mCurrentIndex - 1;
    if (prev < 0) prev = count - 1;  // always wrap on Previous regardless of mode

    const std::string outgoing = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistItemFinished", outgoing);

    mCurrentIndex = prev;
    OpenOnChild(mPrimary, mCurrentIndex);
    mPrebuffered = false;
    EmitPlaylistEvent("OnPlaylistItemStarted", GetItemDisplayName(mCurrentIndex));
    if (mPrimary != nullptr) mPrimary->Play();
}

void VideoPlaylistPlayer::JumpTo(int32_t index)
{
    const int32_t count = (int32_t)SourceCount();
    if (count == 0 || index < 0 || index >= count) return;

    if (!mPlaying)
    {
        mCurrentIndex = index;
        OpenOnChild(mPrimary, mCurrentIndex);
        mPlaying = true;
        EmitPlaylistEvent("OnPlaylistStarted",     GetItemDisplayName(mCurrentIndex));
        EmitPlaylistEvent("OnPlaylistItemStarted", GetItemDisplayName(mCurrentIndex));
        if (mPrimary != nullptr) mPrimary->Play();
        return;
    }

    if (index == mCurrentIndex) return;

    const std::string outgoing = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistItemFinished", outgoing);

    mCurrentIndex = index;
    OpenOnChild(mPrimary, mCurrentIndex);
    mPrebuffered = false;
    EmitPlaylistEvent("OnPlaylistItemStarted", GetItemDisplayName(mCurrentIndex));
    if (mPrimary != nullptr) mPrimary->Play();
}

// ---- Mutation ----

int32_t VideoPlaylistPlayer::AddVideo(const std::string& path)
{
    if (!mPlaylistName.empty())
    {
        Playlist* pl = PlaylistRegistry::Get().CreateOrGet(mPlaylistName);
        return pl ? pl->AddItem(path) : 0;
    }
    mInlineClipPaths.push_back(path);
    RebuildInlineCache();
    return mInlineCache.empty() ? 0 : mInlineCache.back().itemId;
}

bool VideoPlaylistPlayer::RemoveVideo(int32_t itemId)
{
    if (!mPlaylistName.empty())
    {
        Playlist* pl = PlaylistRegistry::Get().Find(mPlaylistName);
        return pl && pl->RemoveItem(itemId);
    }
    // Inline: cache IDs are reassigned on each rebuild, so locate by ID then
    // erase the corresponding path slot.
    for (size_t i = 0; i < mInlineCache.size(); ++i)
    {
        if (mInlineCache[i].itemId == itemId)
        {
            if (i < mInlineClipPaths.size())
            {
                mInlineClipPaths.erase(mInlineClipPaths.begin() + i);
            }
            RebuildInlineCache();
            return true;
        }
    }
    return false;
}

void VideoPlaylistPlayer::ClearPlaylist()
{
    if (!mPlaylistName.empty())
    {
        if (Playlist* pl = PlaylistRegistry::Get().Find(mPlaylistName))
        {
            pl->Clear();
        }
        return;
    }
    mInlineClipPaths.clear();
    mInlineCache.clear();
}

void VideoPlaylistPlayer::SetPlaybackMode(PlaylistMode m)
{
    if ((int)m < 0 || (int)m >= (int)PlaylistMode::Count) return;
    if (Playlist* pl = ResolveRegistryPlaylist())
    {
        pl->SetMode(m);
    }
    else
    {
        mPlaybackMode = (int32_t)m;
    }
}

PlaylistMode VideoPlaylistPlayer::GetPlaybackMode() const
{
    return ResolveEffectiveMode();
}

int32_t VideoPlaylistPlayer::GetPlaylistCount() const
{
    return (int32_t)SourceCount();
}

void VideoPlaylistPlayer::SetPlaylistName(const std::string& name)
{
    mPlaylistName = name;
}

void VideoPlaylistPlayer::SetAudioEnabled(bool enabled)
{
    mAudioEnabled = enabled;
    if (mPrimary != nullptr) mPrimary->SetAudioEnabled(enabled);
    // Secondary stays silent (avoids double audio during prebuffer pause).
}

void VideoPlaylistPlayer::SetVolume(float v)
{
    mVolume = v;
    if (mPrimary != nullptr) mPrimary->SetVolume(v);
    if (mNext    != nullptr) mNext->SetVolume(v);
}

Texture* VideoPlaylistPlayer::GetCurrentTexture() const
{
    return (mPrimary != nullptr) ? mPrimary->GetTexture() : nullptr;
}

// ---- Internals ----

/* static */ void VideoPlaylistPlayer::OnPrimaryFinished(Node* listener,
                                                         const std::vector<Datum>& /*args*/)
{
    auto* self = static_cast<VideoPlaylistPlayer*>(listener);
    if (self == nullptr || !self->mPlaying) return;
    self->Advance();
}

void VideoPlaylistPlayer::Advance()
{
    if (!mPlaying) return;

    const std::string outgoing = GetItemDisplayName(mCurrentIndex);
    EmitPlaylistEvent("OnPlaylistItemFinished", outgoing);

    // If a prebuffer is sitting on mNext, honor that exact choice so the
    // played clip matches what's actually loaded — Random mode would otherwise
    // re-roll a different index here and play the wrong clip on the swap.
    int32_t nextIdx = -1;
    bool    wrapped = false;
    bool    consumePrebuffer = false;

    if (mPrebuffered && mNext != nullptr && !mForceNearSeamless &&
        mPrebufferedIndex >= 0 && (size_t)mPrebufferedIndex < SourceCount())
    {
        nextIdx          = mPrebufferedIndex;
        wrapped          = mPrebufferedWrap;
        consumePrebuffer = true;
    }
    else
    {
        nextIdx = ComputeNextIndex(&wrapped);
    }

    if (nextIdx < 0)
    {
        EmitPlaylistEvent("OnPlaylistEnded", outgoing);
        if (mPrimary != nullptr) mPrimary->Stop();
        mPlaying          = false;
        mCurrentIndex     = -1;
        mPrebuffered      = false;
        mPrebufferedIndex = -1;
        return;
    }

    mCurrentIndex = nextIdx;

    if (wrapped)
    {
        EmitPlaylistEvent("OnPlaylistLooped", GetItemDisplayName(mCurrentIndex));
    }

    if (consumePrebuffer)
    {
        // True-seamless swap: mNext already has the next clip prebuffered.
        // Promote it to primary, demote the old primary to secondary, rebind
        // the OnFinished signal so this handler keeps firing for whoever's
        // playing.
        VideoPlayer3D* newPrimary = mNext;
        VideoPlayer3D* oldPrimary = mPrimary;

        RebindFinishedSignal(newPrimary);

        mPrimary = newPrimary;
        mNext    = oldPrimary;

        mPrimary->SetAudioEnabled(mAudioEnabled);
        mPrimary->SetVolume(mVolume);
        if (mNext != nullptr)
        {
            mNext->SetAudioEnabled(false);
        }
    }
    else
    {
        // NearSeamless / first item / degraded / no prebuffer ready.
        OpenOnChild(mPrimary, mCurrentIndex);
    }

    mPrebuffered      = false;
    mPrebufferedIndex = -1;

    EmitPlaylistEvent("OnPlaylistItemStarted", GetItemDisplayName(mCurrentIndex));
    if (mPrimary != nullptr) mPrimary->Play();
}

void VideoPlaylistPlayer::RebindFinishedSignal(VideoPlayer3D* newPrimary)
{
    if (mPrimary != nullptr) mPrimary->DisconnectSignal("OnFinished", this);
    if (newPrimary != nullptr)
    {
        newPrimary->ConnectSignal("OnFinished", this, &VideoPlaylistPlayer::OnPrimaryFinished);
    }
}

void VideoPlaylistPlayer::OpenOnChild(VideoPlayer3D* child, int32_t index)
{
    if (child == nullptr) return;

    std::string path;
    AssetRef    clip;
    if (!GetItemAt(index, path, clip))
    {
        LogError("VideoPlaylistPlayer::OpenOnChild: index %d out of range", index);
        return;
    }

    // Clear before assigning so the child's HandlePropChange doesn't re-open
    // twice. Setting both is harmless — VideoPlayer3D prefers the asset when set.
    if (clip.Get() != nullptr)
    {
        child->SetFilePath("");
        child->SetVideoClip(clip.Get<VideoClip>());
    }
    else
    {
        child->SetVideoClip(nullptr);
        child->SetFilePath(path);
    }
}

VideoPlayerAddon::Playlist* VideoPlaylistPlayer::ResolveRegistryPlaylist() const
{
    if (mPlaylistName.empty()) return nullptr;
    return PlaylistRegistry::Get().Find(mPlaylistName);
}

PlaylistMode VideoPlaylistPlayer::ResolveEffectiveMode() const
{
    if (Playlist* pl = ResolveRegistryPlaylist())
    {
        return pl->GetMode();
    }
    int32_t m = mPlaybackMode;
    if (m < 0 || m >= (int32_t)PlaylistMode::Count) m = 0;
    return (PlaylistMode)m;
}

TransitionMode VideoPlaylistPlayer::ResolveEffectiveTransition() const
{
    int32_t m = mTransitionMode;
    if (m < 0 || m >= (int32_t)TransitionMode::Count) m = 0;
    if (m != (int32_t)TransitionMode::Auto)
    {
        return (TransitionMode)m;
    }
#if PLATFORM_GAMECUBE
    return TransitionMode::NearSeamless;
#else
    return TransitionMode::TrueSeamless;
#endif
}

size_t VideoPlaylistPlayer::SourceCount() const
{
    if (Playlist* pl = ResolveRegistryPlaylist()) return pl->Count();
    return mInlineCache.size();
}

bool VideoPlaylistPlayer::GetItemAt(int32_t index, std::string& outPath, AssetRef& outClip) const
{
    if (Playlist* pl = ResolveRegistryPlaylist())
    {
        const PlaylistItem* it = pl->GetByIndex((size_t)index);
        if (it == nullptr) return false;
        outPath  = it->path;
        outClip  = it->clipAsset;
        return true;
    }
    if (index < 0 || (size_t)index >= mInlineCache.size()) return false;
    outPath = mInlineCache[index].path;
    outClip = mInlineCache[index].clipAsset;
    return true;
}

std::string VideoPlaylistPlayer::GetItemDisplayName(int32_t index) const
{
    std::string path;
    AssetRef    clip;
    if (!GetItemAt(index, path, clip)) return std::string();
    if (Asset* a = clip.Get()) return a->GetName();
    return path;
}

int32_t VideoPlaylistPlayer::ComputeNextIndex(bool* outWrapped) const
{
    if (outWrapped) *outWrapped = false;

    const int32_t count = (int32_t)SourceCount();
    if (count <= 0) return -1;

    const PlaylistMode mode = ResolveEffectiveMode();

    switch (mode)
    {
    case PlaylistMode::Sequential:
    {
        const int32_t next = mCurrentIndex + 1;
        return (next < count) ? next : -1;
    }
    case PlaylistMode::LoopAll:
    {
        const int32_t next = mCurrentIndex + 1;
        if (next < count) return next;
        if (outWrapped) *outWrapped = true;
        return 0;
    }
    case PlaylistMode::Random:
    {
        // Pick any index. Permitting "same-as-current" keeps v1 simple; if a
        // user wants no-immediate-repeat we add it later.
        return std::rand() % count;
    }
    default: return -1;
    }
}

void VideoPlaylistPlayer::EmitPlaylistEvent(const char* signalName, const std::string& assetName)
{
    std::vector<Datum> args;
    args.push_back(Datum(mPlaylistName));
    args.push_back(Datum(assetName));
    EmitSignal(signalName, args);

    PlaylistEventDispatcher::Get().Fire(signalName, mPlaylistName, assetName);
}

void VideoPlaylistPlayer::RebuildInlineCache()
{
    mInlineCache.clear();
    mInlineCache.reserve(mInlineClipPaths.size());
    for (const std::string& p : mInlineClipPaths)
    {
        PlaylistItem it;
        it.itemId = mInlineNextItemId++;
        it.path   = p;
        mInlineCache.push_back(std::move(it));
    }
}
