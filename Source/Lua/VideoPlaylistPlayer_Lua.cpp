#include "Lua/VideoPlaylistPlayer_Lua.h"

#include "Playlist/Playlist.h"
#include "LuaBindings/Asset_Lua.h"
#include "Assets/Texture.h"

#if LUA_ENABLED

using VideoPlayerAddon::PlaylistMode;
using VideoPlayerAddon::ParsePlaylistMode;
using VideoPlayerAddon::PlaylistModeToString;

int VideoPlaylistPlayer_Lua::Play(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    self->Play();
    return 0;
}

int VideoPlaylistPlayer_Lua::Stop(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    self->Stop();
    return 0;
}

int VideoPlaylistPlayer_Lua::Next(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    self->Next();
    return 0;
}

int VideoPlaylistPlayer_Lua::Previous(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    self->Previous();
    return 0;
}

int VideoPlaylistPlayer_Lua::JumpTo(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    int32_t idx = (int32_t)CHECK_INTEGER(L, 2);
    self->JumpTo(idx);
    return 0;
}

int VideoPlaylistPlayer_Lua::IsPlaying(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushboolean(L, self->IsPlaying() ? 1 : 0);
    return 1;
}

int VideoPlaylistPlayer_Lua::GetCurrentIndex(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushinteger(L, self->GetCurrentIndex());
    return 1;
}

int VideoPlaylistPlayer_Lua::AddVideo(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    const char* path = CHECK_STRING(L, 2);
    int32_t id = self->AddVideo(path ? path : "");
    lua_pushinteger(L, id);
    return 1;
}

int VideoPlaylistPlayer_Lua::RemoveVideo(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    int32_t itemId = (int32_t)CHECK_INTEGER(L, 2);
    lua_pushboolean(L, self->RemoveVideo(itemId) ? 1 : 0);
    return 1;
}

int VideoPlaylistPlayer_Lua::ClearPlaylist(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    self->ClearPlaylist();
    return 0;
}

int VideoPlaylistPlayer_Lua::SetPlaylistMode(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    const char* modeStr = CHECK_STRING(L, 2);
    PlaylistMode m;
    if (!ParsePlaylistMode(modeStr ? modeStr : "", m))
    {
        LogWarning("VideoPlaylistPlayer:SetPlaylistMode unknown mode '%s'", modeStr ? modeStr : "");
        lua_pushboolean(L, 0);
        return 1;
    }
    self->SetPlaybackMode(m);
    lua_pushboolean(L, 1);
    return 1;
}

int VideoPlaylistPlayer_Lua::GetPlaylistMode(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushstring(L, PlaylistModeToString(self->GetPlaybackMode()));
    return 1;
}

int VideoPlaylistPlayer_Lua::GetPlaylistCount(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushinteger(L, self->GetPlaylistCount());
    return 1;
}

int VideoPlaylistPlayer_Lua::SetPlaylistName(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    const char* name = CHECK_STRING(L, 2);
    self->SetPlaylistName(name ? name : "");
    return 0;
}

int VideoPlaylistPlayer_Lua::GetPlaylistName(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushstring(L, self->GetPlaylistName().c_str());
    return 1;
}

int VideoPlaylistPlayer_Lua::SetAudioEnabled(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    bool v = CHECK_BOOLEAN(L, 2);
    self->SetAudioEnabled(v);
    return 0;
}

int VideoPlaylistPlayer_Lua::IsAudioEnabled(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushboolean(L, self->IsAudioEnabled() ? 1 : 0);
    return 1;
}

int VideoPlaylistPlayer_Lua::SetVolume(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    float v = (float)CHECK_NUMBER(L, 2);
    self->SetVolume(v);
    return 0;
}

int VideoPlaylistPlayer_Lua::GetVolume(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    lua_pushnumber(L, self->GetVolume());
    return 1;
}

int VideoPlaylistPlayer_Lua::GetCurrentTexture(lua_State* L)
{
    VideoPlaylistPlayer* self = CHECK_VIDEO_PLAYLIST_PLAYER(L, 1);
    Texture* tex = self->GetCurrentTexture();
    Asset_Lua::Create(L, tex, true /*allowNull*/);
    return 1;
}

void VideoPlaylistPlayer_Lua::Bind()
{
    lua_State* L = GetLua();
    const int startTop = lua_gettop(L);

    int mtIndex = CreateClassMetatable(
        VIDEO_PLAYLIST_PLAYER_LUA_NAME,
        VIDEO_PLAYLIST_PLAYER_LUA_FLAG,
        NODE_3D_LUA_NAME);

    Node_Lua::BindCommon(L, mtIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, Play);
    REGISTER_TABLE_FUNC(L, mtIndex, Stop);
    REGISTER_TABLE_FUNC(L, mtIndex, Next);
    REGISTER_TABLE_FUNC(L, mtIndex, Previous);
    REGISTER_TABLE_FUNC(L, mtIndex, JumpTo);
    REGISTER_TABLE_FUNC(L, mtIndex, IsPlaying);
    REGISTER_TABLE_FUNC(L, mtIndex, GetCurrentIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, AddVideo);
    REGISTER_TABLE_FUNC(L, mtIndex, RemoveVideo);
    REGISTER_TABLE_FUNC(L, mtIndex, ClearPlaylist);
    REGISTER_TABLE_FUNC(L, mtIndex, SetPlaylistMode);
    REGISTER_TABLE_FUNC(L, mtIndex, GetPlaylistMode);
    REGISTER_TABLE_FUNC(L, mtIndex, GetPlaylistCount);

    REGISTER_TABLE_FUNC(L, mtIndex, SetPlaylistName);
    REGISTER_TABLE_FUNC(L, mtIndex, GetPlaylistName);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAudioEnabled);
    REGISTER_TABLE_FUNC(L, mtIndex, IsAudioEnabled);
    REGISTER_TABLE_FUNC(L, mtIndex, SetVolume);
    REGISTER_TABLE_FUNC(L, mtIndex, GetVolume);

    REGISTER_TABLE_FUNC(L, mtIndex, GetCurrentTexture);

    lua_pop(L, 1);
    OCT_ASSERT(lua_gettop(L) == startTop);
}

#endif
