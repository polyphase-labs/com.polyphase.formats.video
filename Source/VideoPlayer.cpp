/**
 * @file VideoPlayer.cpp
 * @brief Native addon: VideoPlayer
 *
 * Plugin entry point. Registers the VideoPlayer3D node's Lua binding so scripts can
 * construct and control video playback. Custom node registration happens automatically
 * via DEFINE_NODE's static initializer running on DLL load.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "EngineAPIAccess.h"
#include "Lua/VideoPlayer3D_Lua.h"
#include "Assets/VideoClip.h"

#if EDITOR
#include "AssetManager.h"
#endif

// Cached engine API accessor. Exposed so any addon file can reach the API without
// threading the pointer through constructors. See EngineAPIAccess.h.
namespace VideoPlayerAddon
{
    static PolyphaseEngineAPI* sEngineAPI = nullptr;
    PolyphaseEngineAPI* GetEngineAPI()            { return sEngineAPI; }
    void                SetEngineAPI(PolyphaseEngineAPI* api) { sEngineAPI = api; }
}

static int OnLoad(PolyphaseEngineAPI* api)
{
    VideoPlayerAddon::SetEngineAPI(api);

    // Pull the addon's asset types into the link so their static initializers
    // register the class with the AssetManager. Without this the optimiser may
    // discard the translation unit since nothing else references its symbols.
    FORCE_LINK_CALL(VideoClip);

#if EDITOR
    // Teach the editor's import dispatcher which extensions belong to VideoClip.
    // Lower-case + leading dot to match the comparison style in
    // ActionManager::ImportAsset. Re-registered on every addon reload, which is
    // fine: the map just overwrites.
    TypeId vt = VideoClip::GetStaticType();
    RegisterImportExtension(".mp4",  vt);
    RegisterImportExtension(".mov",  vt);
    RegisterImportExtension(".webm", vt);
    RegisterImportExtension(".mkv",  vt);
    RegisterImportExtension(".avi",  vt);
    RegisterImportExtension(".m4v",  vt);
#endif

    if (api != nullptr && api->LogDebug != nullptr)
    {
        api->LogDebug("VideoPlayer addon loaded");
    }
    return 0;
}

static void OnUnload()
{
    PolyphaseEngineAPI* api = VideoPlayerAddon::GetEngineAPI();
    if (api != nullptr && api->LogDebug != nullptr)
    {
        api->LogDebug("VideoPlayer addon unloaded");
    }
    VideoPlayerAddon::SetEngineAPI(nullptr);
}

static void RegisterTypes(void* /*nodeFactory*/)
{
    // VideoPlayer3D registers itself via DEFINE_NODE's static initializer when the DLL
    // loads, so no explicit work is needed here. If the engine later provides an
    // explicit registration entry point via `nodeFactory`, hook it in here.
}

static void RegisterScriptFuncs(lua_State* /*L*/)
{
#if LUA_ENABLED
    VideoPlayer3D_Lua::Bind();
#endif
}

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* /*hooks*/, uint64_t /*hookId*/)
{
    // No editor UI for v1.
}
#endif

// Core descriptor-fill. Same body, two different entry-point names depending on build mode.
// - Editor build: `PolyphasePlugin_GetDesc` is the dllexport the NativeAddonManager looks up
//   via GetProcAddress.
// - Shipped build: the addon's sources are compiled directly into the game exe, where multiple
//   addons coexist. Each addon gets a unique entry-point name (suffixed with its package.json
//   id) so the generated `POLYPHASE_REGISTER_PLUGIN(...)` line can reference it without
//   symbol collisions.
static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "VideoPlayer";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
#else
    desc->RegisterEditorUI = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
// Shipped build: named by addon id so the generated registrar can `extern "C"` it.
extern "C" int PolyphasePlugin_GetDesc_com_polyphase_formats_video(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
