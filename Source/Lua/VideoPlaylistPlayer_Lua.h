#pragma once

#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"

#include "Nodes/VideoPlaylistPlayer.h"

#include "LuaBindings/Node_Lua.h"
#include "LuaBindings/Node3d_Lua.h"
#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

// Must match DECLARE_NODE(VideoPlaylistPlayer, Node3D) class name so
// Script::CallFunction can resolve the metatable via luaL_getmetatable.
#define VIDEO_PLAYLIST_PLAYER_LUA_NAME "VideoPlaylistPlayer"
#define VIDEO_PLAYLIST_PLAYER_LUA_FLAG "cfVideoPlaylistPlayer"
#define CHECK_VIDEO_PLAYLIST_PLAYER(L, arg) static_cast<VideoPlaylistPlayer*>(CheckNodeLuaType(L, arg, VIDEO_PLAYLIST_PLAYER_LUA_NAME, VIDEO_PLAYLIST_PLAYER_LUA_FLAG));

struct VideoPlaylistPlayer_Lua
{
    // Transport
    static int Play(lua_State* L);
    static int Stop(lua_State* L);
    static int Next(lua_State* L);
    static int Previous(lua_State* L);
    static int JumpTo(lua_State* L);
    static int IsPlaying(lua_State* L);
    static int GetCurrentIndex(lua_State* L);

    // Playlist mutation (routes to bound registry playlist or inline list)
    static int AddVideo(lua_State* L);
    static int RemoveVideo(lua_State* L);
    static int ClearPlaylist(lua_State* L);
    static int SetPlaylistMode(lua_State* L);
    static int GetPlaylistMode(lua_State* L);
    static int GetPlaylistCount(lua_State* L);

    // Binding
    static int SetPlaylistName(lua_State* L);
    static int GetPlaylistName(lua_State* L);

    // Audio passthrough
    static int SetAudioEnabled(lua_State* L);
    static int IsAudioEnabled(lua_State* L);
    static int SetVolume(lua_State* L);
    static int GetVolume(lua_State* L);

    // Texture from currently-active primary child. Re-fetch in
    // OnPlaylistItemStarted because TrueSeamless swaps replace it.
    static int GetCurrentTexture(lua_State* L);

    static void Bind();
};

#endif
