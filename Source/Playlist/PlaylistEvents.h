#pragma once

#include "ScriptFunc.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace VideoPlayerAddon
{

// Global event dispatcher for VideoPlayer.Connect("OnPlaylist*", fn).
// Separate from per-node signals because the global API is for script consumers
// that don't hold a node handle (e.g. analytics, debug logging, level-wide
// fade-in/out). VideoPlaylistPlayer::EmitPlaylistEvent fires both the node
// signal AND fans out here, so the same event lights up both surfaces.
class PlaylistEventDispatcher
{
public:
    using ListenerId = int32_t;
    static constexpr ListenerId kInvalidId = 0;

    static PlaylistEventDispatcher& Get();

    // Connect a Lua callback to a named event. Returns a non-zero handle, or
    // kInvalidId on failure (empty event name / invalid ScriptFunc).
    // Callback signature in Lua: function(playlistName, assetName).
    ListenerId Connect(const std::string& eventName, const ScriptFunc& fn);

    // Returns true if a listener with that handle existed and was removed.
    bool Disconnect(ListenerId id);

    // Dispatches to every listener registered under eventName. Listeners
    // are copied out under the mutex first so callbacks can themselves
    // Connect/Disconnect without deadlocking.
    void Fire(const std::string& eventName,
              const std::string& playlistName,
              const std::string& assetName);

    // Drops every listener. Called from VideoPlayer.cpp::OnUnload so stored
    // ScriptFunc Lua refs release before the addon DLL is freed.
    void Clear();

private:
    PlaylistEventDispatcher() = default;

    struct Entry
    {
        ListenerId  id;
        std::string event;
        ScriptFunc  fn;
    };

    std::vector<Entry> mEntries;
    ListenerId         mNextId = 1;
    std::mutex         mMutex;
};

} // namespace VideoPlayerAddon
