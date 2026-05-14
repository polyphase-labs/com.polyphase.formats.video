#include "Playlist/Playlist.h"

#include <cstring>

namespace VideoPlayerAddon
{

const char* kPlaylistModeStrings[(int)PlaylistMode::Count] =
{
    "Sequential",
    "LoopAll",
    "Random",
};

// Lua exposes lowercase + underscore form ("loop_all") so playback mode strings
// look natural alongside other enum-string APIs. ParsePlaylistMode accepts both
// the inspector-display form and the Lua form so scripts and serialized inspector
// values are interchangeable.
bool ParsePlaylistMode(const char* str, PlaylistMode& out)
{
    if (str == nullptr) return false;

    auto eq = [str](const char* lit) { return strcmp(str, lit) == 0; };

    if (eq("sequential") || eq("Sequential")) { out = PlaylistMode::Sequential; return true; }
    if (eq("loop_all")   || eq("LoopAll")
                         || eq("loopall")
                         || eq("Loop All"))   { out = PlaylistMode::LoopAll;    return true; }
    if (eq("random")     || eq("Random"))     { out = PlaylistMode::Random;     return true; }
    return false;
}

const char* PlaylistModeToString(PlaylistMode m)
{
    int idx = (int)m;
    if (idx < 0 || idx >= (int)PlaylistMode::Count) return "Sequential";
    return kPlaylistModeStrings[idx];
}

int32_t Playlist::AddItem(const std::string& path)
{
    PlaylistItem it;
    it.itemId = mNextItemId++;
    it.path   = path;
    mItems.push_back(std::move(it));
    return mItems.back().itemId;
}

int32_t Playlist::AddItem(const AssetRef& clip)
{
    PlaylistItem it;
    it.itemId    = mNextItemId++;
    it.clipAsset = clip;
    mItems.push_back(std::move(it));
    return mItems.back().itemId;
}

bool Playlist::RemoveItem(int32_t itemId)
{
    for (auto it = mItems.begin(); it != mItems.end(); ++it)
    {
        if (it->itemId == itemId)
        {
            mItems.erase(it);
            return true;
        }
    }
    return false;
}

void Playlist::Clear()
{
    mItems.clear();
}

void Playlist::SetMode(PlaylistMode m)
{
    if ((int)m < 0 || (int)m >= (int)PlaylistMode::Count) return;
    mMode = m;
}

const PlaylistItem* Playlist::GetByIndex(size_t i) const
{
    return (i < mItems.size()) ? &mItems[i] : nullptr;
}

const PlaylistItem* Playlist::FindById(int32_t itemId) const
{
    for (const PlaylistItem& it : mItems)
    {
        if (it.itemId == itemId) return &it;
    }
    return nullptr;
}

int32_t Playlist::IndexOfId(int32_t itemId) const
{
    for (size_t i = 0; i < mItems.size(); ++i)
    {
        if (mItems[i].itemId == itemId) return (int32_t)i;
    }
    return -1;
}

} // namespace VideoPlayerAddon
