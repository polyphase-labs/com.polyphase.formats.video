#include "Playlist/PlaylistRegistry.h"

namespace VideoPlayerAddon
{

PlaylistRegistry& PlaylistRegistry::Get()
{
    static PlaylistRegistry sInstance;
    return sInstance;
}

Playlist* PlaylistRegistry::CreateOrGet(const std::string& name, bool* outExisted)
{
    std::lock_guard<std::mutex> lock(mMutex);

    auto it = mPlaylists.find(name);
    if (it != mPlaylists.end())
    {
        if (outExisted) *outExisted = true;
        return it->second.get();
    }

    auto inserted = mPlaylists.emplace(name, std::make_unique<Playlist>());
    if (outExisted) *outExisted = false;
    return inserted.first->second.get();
}

Playlist* PlaylistRegistry::Find(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mPlaylists.find(name);
    return (it != mPlaylists.end()) ? it->second.get() : nullptr;
}

bool PlaylistRegistry::Destroy(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mPlaylists.erase(name) > 0;
}

void PlaylistRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mPlaylists.clear();
}

} // namespace VideoPlayerAddon
