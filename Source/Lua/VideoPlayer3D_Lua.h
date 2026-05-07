#pragma once

#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"

#include "Nodes/VideoPlayer3D.h"

#include "LuaBindings/Node_Lua.h"
#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

// Must match the DECLARE_NODE(VideoPlayer3D, Node3D) class name so that Script::CallFunction
// (Script.cpp) can resolve the metatable via `luaL_getmetatable(L, node->GetClassName())`.
#define VIDEO_PLAYER_3D_LUA_NAME "VideoPlayer3D"
#define VIDEO_PLAYER_3D_LUA_FLAG "cfVideoPlayer3D"
#define CHECK_VIDEO_PLAYER_3D(L, arg) static_cast<VideoPlayer3D*>(CheckNodeLuaType(L, arg, VIDEO_PLAYER_3D_LUA_NAME, VIDEO_PLAYER_3D_LUA_FLAG));

struct VideoPlayer3D_Lua
{
    static int Play(lua_State* L);
    static int Pause(lua_State* L);
    static int Stop(lua_State* L);
    static int Seek(lua_State* L);

    static int GetTime(lua_State* L);
    static int GetDuration(lua_State* L);
    static int IsPlaying(lua_State* L);
    static int IsReady(lua_State* L);

    static int SetFilePath(lua_State* L);
    static int GetFilePath(lua_State* L);
    static int SetVideoClip(lua_State* L);
    static int GetVideoClip(lua_State* L);
    static int SetAutoPlay(lua_State* L);
    static int GetAutoPlay(lua_State* L);
    static int SetLoop(lua_State* L);
    static int GetLoop(lua_State* L);
    static int SetPlaybackSpeed(lua_State* L);
    static int GetPlaybackSpeed(lua_State* L);
    static int SetAlphaMode(lua_State* L);
    static int GetAlphaMode(lua_State* L);

    // Audio
    static int SetAudioEnabled(lua_State* L);
    static int IsAudioEnabled(lua_State* L);
    static int SetVolume(lua_State* L);
    static int GetVolume(lua_State* L);
    static int HasAudioStream(lua_State* L);

    static int GetTexture(lua_State* L);

    static void Bind();
};

#endif
