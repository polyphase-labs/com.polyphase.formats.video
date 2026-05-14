#pragma once

#include "EngineTypes.h"
#include "Engine.h"

#if LUA_ENABLED

#include "LuaBindings/LuaUtils.h"

// Global "VideoPlayer" Lua table — playlist registry + global event subscription.
// Per-node methods live on the metatables in VideoPlayer3D_Lua and
// VideoPlaylistPlayer_Lua.
struct VideoPlayer_Lua
{
    // Playlist registry
    static int CreatePlaylist(lua_State* L);    // (name)
    static int DestroyPlaylist(lua_State* L);   // (name)
    static int AddVideo(lua_State* L);          // (name, path)
    static int RemoveVideo(lua_State* L);       // (name, itemId)
    static int ClearPlaylist(lua_State* L);     // (name)
    static int SetPlaylistMode(lua_State* L);   // (name, "sequential"|"loop_all"|"random")
    static int GetPlaylistMode(lua_State* L);   // (name) -> string
    static int GetPlaylistCount(lua_State* L);  // (name) -> int

    // Global event subscriptions. eventName ∈ {"OnPlaylistStarted",
    // "OnPlaylistEnded", "OnPlaylistLooped", "OnPlaylistItemStarted",
    // "OnPlaylistItemFinished"}. Callback signature: function(playlistName, assetName).
    static int Connect(lua_State* L);           // (eventName, fn) -> int handle
    static int Disconnect(lua_State* L);        // (handle) -> bool

    static void Bind();
};

#endif
