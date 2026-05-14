#include "Lua/VideoPlayer_Lua.h"

#include "Playlist/Playlist.h"
#include "Playlist/PlaylistRegistry.h"
#include "Playlist/PlaylistEvents.h"

#include "ScriptFunc.h"
#include "Log.h"

#if LUA_ENABLED

using VideoPlayerAddon::PlaylistMode;
using VideoPlayerAddon::Playlist;
using VideoPlayerAddon::PlaylistRegistry;
using VideoPlayerAddon::PlaylistEventDispatcher;
using VideoPlayerAddon::ParsePlaylistMode;
using VideoPlayerAddon::PlaylistModeToString;

int VideoPlayer_Lua::CreatePlaylist(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    bool existed = false;
    PlaylistRegistry::Get().CreateOrGet(name ? name : "", &existed);
    lua_pushboolean(L, existed ? 1 : 0);
    return 1;
}

int VideoPlayer_Lua::DestroyPlaylist(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    bool destroyed = PlaylistRegistry::Get().Destroy(name ? name : "");
    lua_pushboolean(L, destroyed ? 1 : 0);
    return 1;
}

int VideoPlayer_Lua::AddVideo(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    const char* path = CHECK_STRING(L, 2);
    Playlist* pl = PlaylistRegistry::Get().CreateOrGet(name ? name : "");
    int32_t id = pl ? pl->AddItem(path ? path : "") : 0;
    lua_pushinteger(L, id);
    return 1;
}

int VideoPlayer_Lua::RemoveVideo(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    int32_t itemId = (int32_t)CHECK_INTEGER(L, 2);
    Playlist* pl = PlaylistRegistry::Get().Find(name ? name : "");
    bool removed = pl && pl->RemoveItem(itemId);
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

int VideoPlayer_Lua::ClearPlaylist(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    if (Playlist* pl = PlaylistRegistry::Get().Find(name ? name : ""))
    {
        pl->Clear();
    }
    return 0;
}

int VideoPlayer_Lua::SetPlaylistMode(lua_State* L)
{
    const char* name    = CHECK_STRING(L, 1);
    const char* modeStr = CHECK_STRING(L, 2);
    Playlist* pl = PlaylistRegistry::Get().Find(name ? name : "");
    if (pl == nullptr)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    PlaylistMode mode;
    if (!ParsePlaylistMode(modeStr ? modeStr : "", mode))
    {
        LogWarning("VideoPlayer.SetPlaylistMode: unknown mode '%s' (expected sequential|loop_all|random)",
                   modeStr ? modeStr : "");
        lua_pushboolean(L, 0);
        return 1;
    }
    pl->SetMode(mode);
    lua_pushboolean(L, 1);
    return 1;
}

int VideoPlayer_Lua::GetPlaylistMode(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    Playlist* pl = PlaylistRegistry::Get().Find(name ? name : "");
    if (pl == nullptr)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, PlaylistModeToString(pl->GetMode()));
    return 1;
}

int VideoPlayer_Lua::GetPlaylistCount(lua_State* L)
{
    const char* name = CHECK_STRING(L, 1);
    Playlist* pl = PlaylistRegistry::Get().Find(name ? name : "");
    lua_pushinteger(L, pl ? (int)pl->Count() : 0);
    return 1;
}

int VideoPlayer_Lua::Connect(lua_State* L)
{
    const char* eventName = CHECK_STRING(L, 1);
    if (!lua_isfunction(L, 2))
    {
        LogWarning("VideoPlayer.Connect: arg 2 must be a function");
        lua_pushinteger(L, 0);
        return 1;
    }
    ScriptFunc fn(L, 2);
    auto id = PlaylistEventDispatcher::Get().Connect(eventName ? eventName : "", fn);
    lua_pushinteger(L, id);
    return 1;
}

int VideoPlayer_Lua::Disconnect(lua_State* L)
{
    int32_t id = (int32_t)CHECK_INTEGER(L, 1);
    bool ok = PlaylistEventDispatcher::Get().Disconnect(id);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

void VideoPlayer_Lua::Bind()
{
    lua_State* L = GetLua();
    const int startTop = lua_gettop(L);

    lua_newtable(L);
    const int tIndex = lua_gettop(L);

    lua_pushcfunction(L, CreatePlaylist);    lua_setfield(L, tIndex, "CreatePlaylist");
    lua_pushcfunction(L, DestroyPlaylist);   lua_setfield(L, tIndex, "DestroyPlaylist");
    lua_pushcfunction(L, AddVideo);          lua_setfield(L, tIndex, "AddVideo");
    lua_pushcfunction(L, RemoveVideo);       lua_setfield(L, tIndex, "RemoveVideo");
    lua_pushcfunction(L, ClearPlaylist);     lua_setfield(L, tIndex, "ClearPlaylist");
    lua_pushcfunction(L, SetPlaylistMode);   lua_setfield(L, tIndex, "SetPlaylistMode");
    lua_pushcfunction(L, GetPlaylistMode);   lua_setfield(L, tIndex, "GetPlaylistMode");
    lua_pushcfunction(L, GetPlaylistCount);  lua_setfield(L, tIndex, "GetPlaylistCount");
    lua_pushcfunction(L, Connect);           lua_setfield(L, tIndex, "Connect");
    lua_pushcfunction(L, Disconnect);        lua_setfield(L, tIndex, "Disconnect");

    lua_setglobal(L, "VideoPlayer");

    OCT_ASSERT(lua_gettop(L) == startTop);
}

#endif
