#include "Lua/VideoPlayer3D_Lua.h"

#include "LuaBindings/Node3d_Lua.h"
#include "LuaBindings/Asset_Lua.h"
#include "LuaBindings/LuaUtils.h"

#include "Assets/Texture.h"
#include "Assets/VideoClip.h"

#if LUA_ENABLED

int VideoPlayer3D_Lua::Play(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    vp->Play();
    return 0;
}

int VideoPlayer3D_Lua::Pause(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    vp->Pause();
    return 0;
}

int VideoPlayer3D_Lua::Stop(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    vp->Stop();
    return 0;
}

int VideoPlayer3D_Lua::Seek(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    double t = CHECK_NUMBER(L, 2);
    vp->Seek(t);
    return 0;
}

int VideoPlayer3D_Lua::GetTime(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushnumber(L, vp->GetTime());
    return 1;
}

int VideoPlayer3D_Lua::GetDuration(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushnumber(L, vp->GetDuration());
    return 1;
}

int VideoPlayer3D_Lua::IsPlaying(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->IsPlaying() ? 1 : 0);
    return 1;
}

int VideoPlayer3D_Lua::IsReady(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->IsReady() ? 1 : 0);
    return 1;
}

int VideoPlayer3D_Lua::SetFilePath(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    const char* path = CHECK_STRING(L, 2);
    vp->SetFilePath(path ? path : "");
    return 0;
}

int VideoPlayer3D_Lua::GetFilePath(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushstring(L, vp->GetFilePath().c_str());
    return 1;
}

int VideoPlayer3D_Lua::SetVideoClip(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    Asset* asset = nullptr;
    if (!lua_isnil(L, 2))
    {
        asset = CHECK_ASSET(L, 2);
    }
    // Allow either a real VideoClip or nil (clear). Reject mismatched asset types
    // by silently treating them as nil so script bugs don't crash playback.
    VideoClip* clip = (asset != nullptr && asset->GetType() == VideoClip::GetStaticType())
                        ? static_cast<VideoClip*>(asset)
                        : nullptr;
    vp->SetVideoClip(clip);
    return 0;
}

int VideoPlayer3D_Lua::GetVideoClip(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    Asset_Lua::Create(L, vp->GetVideoClip(), true /*allowNull*/);
    return 1;
}

int VideoPlayer3D_Lua::SetAutoPlay(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    bool v = CHECK_BOOLEAN(L, 2);
    vp->SetAutoPlay(v);
    return 0;
}

int VideoPlayer3D_Lua::GetAutoPlay(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->GetAutoPlay() ? 1 : 0);
    return 1;
}

int VideoPlayer3D_Lua::SetLoop(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    bool v = CHECK_BOOLEAN(L, 2);
    vp->SetLoop(v);
    return 0;
}

int VideoPlayer3D_Lua::GetLoop(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->GetLoop() ? 1 : 0);
    return 1;
}

int VideoPlayer3D_Lua::SetPlaybackSpeed(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    float v = CHECK_NUMBER(L, 2);
    vp->SetPlaybackSpeed(v);
    return 0;
}

int VideoPlayer3D_Lua::GetPlaybackSpeed(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushnumber(L, vp->GetPlaybackSpeed());
    return 1;
}

int VideoPlayer3D_Lua::SetAlphaMode(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    int mode = (int)CHECK_INTEGER(L, 2);
    if (mode < 0 || mode >= int(VideoAlphaMode::Count)) mode = 0;
    vp->SetAlphaMode(VideoAlphaMode(mode));
    return 0;
}

int VideoPlayer3D_Lua::GetAlphaMode(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushinteger(L, int(vp->GetAlphaMode()));
    return 1;
}

int VideoPlayer3D_Lua::GetTexture(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    Texture* tex = vp->GetTexture();
    Asset_Lua::Create(L, tex, true /*allowNull*/);
    return 1;
}

int VideoPlayer3D_Lua::SetAudioEnabled(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    bool v = CHECK_BOOLEAN(L, 2);
    vp->SetAudioEnabled(v);
    return 0;
}

int VideoPlayer3D_Lua::IsAudioEnabled(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->IsAudioEnabled() ? 1 : 0);
    return 1;
}

int VideoPlayer3D_Lua::SetVolume(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    float v = (float)CHECK_NUMBER(L, 2);
    vp->SetVolume(v);
    return 0;
}

int VideoPlayer3D_Lua::GetVolume(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushnumber(L, vp->GetVolume());
    return 1;
}

int VideoPlayer3D_Lua::HasAudioStream(lua_State* L)
{
    VideoPlayer3D* vp = CHECK_VIDEO_PLAYER_3D(L, 1);
    lua_pushboolean(L, vp->HasAudioStream() ? 1 : 0);
    return 1;
}

void VideoPlayer3D_Lua::Bind()
{
    lua_State* L = GetLua();
    const int startTop = lua_gettop(L);

    int mtIndex = CreateClassMetatable(
        VIDEO_PLAYER_3D_LUA_NAME,
        VIDEO_PLAYER_3D_LUA_FLAG,
        NODE_3D_LUA_NAME);

    Node_Lua::BindCommon(L, mtIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, Play);
    REGISTER_TABLE_FUNC(L, mtIndex, Pause);
    REGISTER_TABLE_FUNC(L, mtIndex, Stop);
    REGISTER_TABLE_FUNC(L, mtIndex, Seek);

    REGISTER_TABLE_FUNC(L, mtIndex, GetTime);
    REGISTER_TABLE_FUNC(L, mtIndex, GetDuration);
    REGISTER_TABLE_FUNC(L, mtIndex, IsPlaying);
    REGISTER_TABLE_FUNC(L, mtIndex, IsReady);

    REGISTER_TABLE_FUNC(L, mtIndex, SetFilePath);
    REGISTER_TABLE_FUNC(L, mtIndex, GetFilePath);
    REGISTER_TABLE_FUNC(L, mtIndex, SetVideoClip);
    REGISTER_TABLE_FUNC(L, mtIndex, GetVideoClip);
    REGISTER_TABLE_FUNC(L, mtIndex, SetAutoPlay);
    REGISTER_TABLE_FUNC(L, mtIndex, GetAutoPlay);
    REGISTER_TABLE_FUNC(L, mtIndex, SetLoop);
    REGISTER_TABLE_FUNC(L, mtIndex, GetLoop);
    REGISTER_TABLE_FUNC(L, mtIndex, SetPlaybackSpeed);
    REGISTER_TABLE_FUNC(L, mtIndex, GetPlaybackSpeed);
    REGISTER_TABLE_FUNC(L, mtIndex, SetAlphaMode);
    REGISTER_TABLE_FUNC(L, mtIndex, GetAlphaMode);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAudioEnabled);
    REGISTER_TABLE_FUNC(L, mtIndex, IsAudioEnabled);
    REGISTER_TABLE_FUNC(L, mtIndex, SetVolume);
    REGISTER_TABLE_FUNC(L, mtIndex, GetVolume);
    REGISTER_TABLE_FUNC(L, mtIndex, HasAudioStream);

    REGISTER_TABLE_FUNC(L, mtIndex, GetTexture);

    lua_pop(L, 1);
    // Engine's Lua bindings assert lua_gettop == 0 at the end of Bind(). For addons
    // RegisterScriptFuncs may be called with items already on the stack, so assert
    // net-zero (no leak, no over-pop) relative to the starting depth instead.
    OCT_ASSERT(lua_gettop(L) == startTop);
}

#endif
