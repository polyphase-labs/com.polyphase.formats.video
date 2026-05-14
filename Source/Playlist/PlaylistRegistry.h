#pragma once

#include "Playlist/Playlist.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace VideoPlayerAddon
{

// Singleton registry of named Playlist objects. Lifetime is the addon DLL's:
// VideoPlayer.cpp::OnUnload calls Clear() so stored AssetRefs release before
// the engine's hot-reload purge runs.
class PlaylistRegistry
{
public:
    static PlaylistRegistry& Get();

    // Returns the existing playlist or creates a new one. *outExisted (when
    // non-null) reports whether an existing playlist was returned.
    Playlist* CreateOrGet(const std::string& name, bool* outExisted = nullptr);

    // Returns nullptr if no playlist with that name is registered.
    Playlist* Find(const std::string& name);

    // Removes the playlist (and frees its items). No-op if not registered.
    bool Destroy(const std::string& name);

    // Drops every playlist. Called from VideoPlayer.cpp::OnUnload.
    void Clear();

private:
    PlaylistRegistry() = default;

    std::unordered_map<std::string, std::unique_ptr<Playlist>> mPlaylists;
    // Lua coroutine + main thread can both touch the registry (script can
    // mutate it during a Tick that's also reading it). Single mutex is enough
    // — playlist ops are short, contention is low.
    std::mutex mMutex;
};

} // namespace VideoPlayerAddon
