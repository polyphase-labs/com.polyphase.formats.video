# 01 — Getting Started

Goal: play a video on screen in 5 minutes.

## Prerequisites

- Polyphase Engine, this addon installed (`com.polyphase.formats.video`).
- For Windows / Linux PC playback: FFmpeg shared libraries placed under `Packages/com.polyphase.formats.video/External/ffmpeg/{include,lib,bin}`. See the addon's main `README.md` for FFmpeg install instructions. Console builds do NOT need FFmpeg — they use built-in software decoders.

## Quick path: drop a clip into a scene

1. **Import the source video.** Drag an `.mp4` (or `.mov`, `.webm`, `.mkv`, `.avi`, `.m4v`) onto the editor's asset browser. The importer creates a `VideoClip` asset (`.oct`).
2. **Inspect the asset.** In the asset browser, click the new `VideoClip`. The inspector shows source dimensions, duration, frame rate, audio config, plus cook parameters (preset, format, dimensions, fps, JPEG quality, audio rate, channels).
3. **Drop a `VideoPlayer3D` node into your scene.** Either via the node tree's add menu, or via Lua:
   ```lua
   self.player = self:SpawnChild("VideoPlayer3D", "Player")
   ```
4. **Wire the clip.** In the inspector, drag the `VideoClip` asset into the `Video Clip` slot on `VideoPlayer3D`. (Or via Lua: `self.player:SetVideoClip(LoadAsset("Videos/MyClip"))`.)
5. **Show it on screen.** Add a `Quad` widget child, connect `VideoPlayer3D::OnReady` to a function that does:
   ```lua
   quad:SetTexture(player:GetTexture())
   player:Play()
   ```
   See `Scripts/video_demo.lua` in this addon for a complete runnable example.
6. **Press Play in the editor.** The video should play full-rate with audio.

## What about consoles?

PC works above. For Wii / GameCube / 3DS the same flow works in the editor (which always renders via the PC backend), but **before packaging for the console you must cook the clip for that target.** Cooking compresses the source `.mp4` into a console-decodable container (PCV1 / THP / N3MV). See [Asset Workflow](05-AssetWorkflow.md) for the cook step and [Formats](02-Formats.md) for which container to pick.

If you skip cooking, the packaged build will silently fail to play (the runtime decoder factory has nothing to route to).

## Minimal Lua example (verbose, no editor wiring)

```lua
function OnStart(self)
    self.player = self:SpawnChild("VideoPlayer3D", "Player")
    self.player:SetVideoClip(LoadAsset("Videos/IntroClip"))
    self.player:SetLoop(false)

    self.quad = self:SpawnChild("Quad", "VideoQuad")
    self.quad:SetDimensions(640, 360)

    self.player:ConnectSignal("OnReady", self, function(s)
        s.quad:SetTexture(s.player:GetTexture())
        s.player:Play()
    end)

    self.player:ConnectSignal("OnFinished", self, function(s)
        print("Clip finished")
    end)
end
```

## What "should" happen on first run

- Editor (Windows/Linux): video plays at source resolution, audio synced via the audio clock, no glitches.
- Console (after a packaging build): video plays at the **cooked** resolution and frame rate — not the source. If you cooked at `Tiny` (240×135 @ 15 fps), that's what plays. Audio plays through the platform's streaming voice (ASND on Wii/GC, NDSP on 3DS).

If any of that doesn't happen, see [Troubleshooting](07-Troubleshooting.md).
