# 06 — Runtime API

How to drive video playback from script. Mostly Lua — the same methods exist in C++ but games typically wire video from script.

## The `VideoPlayer3D` node

The addon registers a `VideoPlayer3D` node type, derived from `Node3D`. It owns:

- A `VideoClip` reference (asset-backed).
- An optional file path (legacy, pre-`VideoClip` path).
- A streaming `Texture` representing the current decoded frame.
- A streaming audio voice via the engine's audio API.
- An async decode worker (`AsyncMediaPump`) that runs on its own thread.

You add it like any other node — via the editor's node-tree add menu, or via `SpawnChild` from script.

## Methods

### Source selection

```lua
player:SetVideoClip(clip)        -- preferred: an AssetRef<VideoClip>
player:GetVideoClip()            -- returns the current VideoClip, or nil

player:SetFilePath(path)         -- legacy: a string path (only useful for PC editor builds)
player:GetFilePath()
```

If both are set, the `VideoClip` wins. Set the file path to `""` to fall through to the clip.

### Playback control

```lua
player:Play()                    -- starts playback from the current playhead
player:Pause()                   -- freezes playback; does not reset the playhead
player:Stop()                    -- stops + rewinds to t=0
player:Seek(seconds)             -- jumps to the given time; the next displayed frame
                                 --   is the nearest keyframe at or after that time

player:IsPlaying()               -- bool
player:IsPaused()                -- bool
player:GetPlayheadSeconds()      -- current playhead position in seconds
player:GetDurationSeconds()      -- clip duration in seconds (after Open succeeds)
```

### Looping

```lua
player:SetLoop(true)             -- when the clip ends, seek back to 0 and continue
player:GetLoop()
```

The loop trigger fires when `playheadSeconds >= lastFrameSeconds + (1 / fps)` AND the decode pump has signalled end-of-stream. With audio enabled the audio clock is the master clock; the loop trigger waits for the audio playback time to catch up to the clip duration before re-seeking. (This is what makes loop work cleanly across all platforms regardless of the decoder's frame-pacing accuracy.)

### Audio

```lua
player:SetAudioEnabled(true)     -- default true
player:GetAudioEnabled()
player:SetVolume(0.0 .. 1.0)
player:GetVolume()
```

Disabling audio also disables the audio-as-master clock; the playhead then advances via deltaTime instead. This is the one case where `SetPlaybackSpeed` works (see below).

### Playback speed (audio-disabled only)

```lua
player:SetPlaybackSpeed(1.0)     -- 1.0 = normal, 0.5 = half, 2.0 = double
player:GetPlaybackSpeed()
```

If audio is enabled, `SetPlaybackSpeed` is silently ignored (the clip will follow the audio clock at 1.0×). To use playback speed, disable audio first.

### Output

```lua
player:GetTexture()              -- a Texture handle that updates per-frame
                                 --   while playing. Stable across frames; assign
                                 --   it once and the contents update on their own.
```

The texture's pixel format is RGBA8. Its pixel dimensions match the cooked frame dimensions. On 3DS the texture is internally rounded up to power-of-two; use `Texture::GetUVMax()` if you need to know the visible area:

```lua
local tex = player:GetTexture()
local uv = tex:GetUVMax()       -- vec2: { x = visibleWidth/texWidth, y = ... }
quad:SetUVScale(uv)             -- crop to visible area
```

The `Quad` widget's "Match Texture Size" / "Crop Texture" buttons in the inspector handle this for you.

## Signals

`VideoPlayer3D` emits these signals; connect via `ConnectSignal`:

```lua
player:ConnectSignal("OnReady", self, function(self)
    -- Decoder has opened the clip and the first frame is available.
    -- Safe point to call player:GetTexture() and Play().
end)

player:ConnectSignal("OnFinished", self, function(self)
    -- Playback reached end-of-stream and looping is OFF.
    -- (If looping is on, OnFinished does NOT fire — the player just seeks back.)
end)

player:ConnectSignal("OnError", self, function(self, message)
    -- Decoder failed (couldn't open the clip, codec mismatch, etc.).
    -- `message` is a human-readable string.
end)
```

Connect signals before calling `Play()`. Disconnect via `DisconnectSignal` when destroying your wrapper.

## Typical patterns

### Play once and signal completion

```lua
function OnStart(self)
    self.player = self:SpawnChild("VideoPlayer3D", "Player")
    self.player:SetVideoClip(LoadAsset("Videos/IntroClip"))
    self.player:SetLoop(false)

    self.quad = self:SpawnChild("Quad", "VideoQuad")
    self.quad:SetDimensions(640, 360)

    self.player:ConnectSignal("OnReady", self, function(s)
        s.quad:SetTexture(s.player:GetTexture())
        s.quad:SetUVScale(s.player:GetTexture():GetUVMax())
        s.player:Play()
    end)

    self.player:ConnectSignal("OnFinished", self, function(s)
        -- Advance to the next scene, fade out, whatever.
        s:DispatchEvent("IntroFinished")
    end)
end
```

### Looping background video on a UI element

```lua
function OnStart(self)
    self.player = self:SpawnChild("VideoPlayer3D", "BgVideo")
    self.player:SetVideoClip(LoadAsset("Videos/MenuLoop"))
    self.player:SetLoop(true)
    self.player:SetAudioEnabled(false)        -- silent BG video

    self.player:ConnectSignal("OnReady", self, function(s)
        s.background:SetTexture(s.player:GetTexture())
        s.player:Play()
    end)
end
```

### Apply video to a 3D material

```lua
local mat = LoadAsset("Materials/VideoWall")
self.player:ConnectSignal("OnReady", self, function(s)
    mat:SetTextureParameter("Albedo", s.player:GetTexture())
    s.player:Play()
end)
```

The texture handle is stable across frames; the pixel contents update automatically each tick. You don't have to re-set the parameter each frame.

## C++ API

If you need to drive playback from C++ (e.g. from a custom node or game state machine), include `Source/Nodes/VideoPlayer3D.h` and use the same method names — they're declared on the C++ class and bound 1:1 to Lua via the addon's auto-generated bindings.

```cpp
#include "Nodes/VideoPlayer3D.h"

VideoPlayerAddon::VideoPlayer3D* player = ...;
player->SetVideoClip(myClip);
player->SetLoop(true);
player->Play();
```

Do not call `Play()` from `Tick()` — call it once from a setup callback (`OnReady`, `Begin Play`, etc.). Calling Play repeatedly from Tick will reset the decoder every frame.

## Related

- [Getting Started](01-GettingStarted.md) — for a walk-through.
- [Asset Workflow](05-AssetWorkflow.md) — how to get a `VideoClip` to assign in the first place.
- `Scripts/video_demo.lua` (in this addon) — a complete runnable Lua example.
