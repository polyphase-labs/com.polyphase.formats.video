#pragma once

#include "AssetRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace VideoPlayerAddon
{

enum class PlaylistMode : int32_t
{
    Sequential = 0,  // play in order, stop at end
    LoopAll    = 1,  // play in order, wrap to first
    Random     = 2,  // pick a random next clip on each advance
    Count      = 3,
};

// String table aligned with PlaylistMode enum order. Used by both the inspector
// dropdown and the Lua mode-string parsers so the two surfaces stay in sync.
// Element type is intentionally non-const (`const char*` not `const char* const`)
// so this passes to Property's `const char** enumStrings` parameter without a
// qualifier-stripping cast; the strings themselves are still string literals.
extern const char* kPlaylistModeStrings[(int)PlaylistMode::Count];

bool        ParsePlaylistMode(const char* str, PlaylistMode& out);
const char* PlaylistModeToString(PlaylistMode m);

struct PlaylistItem
{
    int32_t     itemId   = 0;   // monotonic per-playlist; never reused after remove
    std::string path;           // project-relative path; resolved at playback time
    AssetRef    clipAsset;      // optional VideoClip ref; takes precedence over path
};

// Pure data container — no playback state, no I/O. The owning runtime
// (VideoPlaylistPlayer node or PlaylistRegistry) iterates items and decides
// what to do with them.
class Playlist
{
public:
    int32_t  AddItem(const std::string& path);
    int32_t  AddItem(const AssetRef& clip);
    bool     RemoveItem(int32_t itemId);
    void     Clear();

    void         SetMode(PlaylistMode m);
    PlaylistMode GetMode() const { return mMode; }

    size_t              Count() const { return mItems.size(); }
    const PlaylistItem* GetByIndex(size_t i) const;
    const PlaylistItem* FindById(int32_t itemId) const;
    int32_t             IndexOfId(int32_t itemId) const;  // -1 if absent

private:
    std::vector<PlaylistItem> mItems;
    PlaylistMode mMode = PlaylistMode::Sequential;
    int32_t      mNextItemId = 1;
};

} // namespace VideoPlayerAddon
