# VideoPlayer — Polyphase Engine Native Addon

Plays video files in Polyphase Engine scenes. The current decoded frame is exposed as a `Texture` that can be sampled by any material or assigned to a `Quad` widget. Audio streams through the engine's per-platform streaming voice API and acts as the master clock for video playback (so loop / sync work cleanly across all platforms).

Supports **Windows / Linux** (FFmpeg passthrough — no cook needed) and **Wii / GameCube / 3DS** (cook to a console-decodable container at packaging time).

## Status

| Platform | Status |
|---|---|
| Windows x64 | Stable |
| Linux x64 | Stable |
| Wii (libogc2) | Stable — PCV1 recommended; THP plays but audio quality is limited |
| GameCube (libogc2) | Stable — same as Wii |
| Nintendo 3DS (libctru) | Stable — PCV1 only; N3MV (hardware H.264) scaffolded but not finished |
| Android / Switch / PS5 / Xbox | Not supported |

## Getting Started

The 30-second version. (Full walkthrough: [Docs/01-GettingStarted.md](Docs/01-GettingStarted.md).)

1. **Drag a video file** (`.mp4` / `.mov` / `.webm` / `.mkv` / `.avi` / `.m4v`) into the editor's asset browser. The importer creates a `VideoClip` asset.
2. **Spawn a `VideoPlayer3D` node** in your scene and assign the `VideoClip` in the inspector.
3. **Connect `OnReady`** to a callback that sets the player's texture on a `Quad` and calls `Play()`:

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
end
```

See `Scripts/video_demo.lua` for a complete runnable example.

For console targets, you also need to **Test Cook for Platform** before packaging — see [Docs/05-AssetWorkflow.md](Docs/05-AssetWorkflow.md).

## FFmpeg setup (PC only)

Windows / Linux playback uses FFmpeg shared libraries. Console builds don't need FFmpeg — they use built-in software decoders.

### Windows

1. Download the "shared" build from `https://www.gyan.dev/ffmpeg/builds/` (the file named like `ffmpeg-release-full-shared.7z`). Use only LGPL builds — **don't** use builds configured with `--enable-gpl` or `--enable-nonfree`.
2. Extract and copy the `include/`, `lib/`, and `bin/` directories into `External/ffmpeg/` inside this addon, so you have:
   ```
   Packages/com.polyphase.formats.video/External/ffmpeg/{include,lib,bin}
   ```
3. Rebuild the addon. CMake logs `VideoPlayer: found FFmpeg at ...` on success. The post-build step copies the FFmpeg DLLs next to the addon binary so they resolve at load time.
4. When shipping a game that uses this addon, include FFmpeg's `LICENSE.txt` and make the FFmpeg source available on request.

### Linux

FFmpeg is picked up via `pkg-config`. Install dev packages from your distro:

```bash
# Debian / Ubuntu
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libswscale-dev libswresample-dev
```

Minimum FFmpeg version: 4.0. 5.x recommended.

## Documentation

End user / game dev documentation lives in [`Docs/`](Docs/):

- [Docs/README.md](Docs/README.md) — documentation index with brief signposts.
- [Docs/01-GettingStarted.md](Docs/01-GettingStarted.md) — full quickstart.
- [Docs/02-Formats.md](Docs/02-Formats.md) — **PCV1 / THP / N3MV: when to use which.** Read this before cooking anything for console.
- [Docs/03-CookPresets.md](Docs/03-CookPresets.md) — `Tiny` / `Balanced` / `Quality` / `Custom` per-knob reference.
- [Docs/04-PlatformSupport.md](Docs/04-PlatformSupport.md) — what works on which platform.
- [Docs/05-AssetWorkflow.md](Docs/05-AssetWorkflow.md) — import → cook → save → package, sidecars, version control.
- [Docs/06-RuntimeAPI.md](Docs/06-RuntimeAPI.md) — `VideoPlayer3D` node, Lua bindings, signals.
- [Docs/07-Troubleshooting.md](Docs/07-Troubleshooting.md) — symptom → cause → fix.
- [Docs/08-Architecture.md](Docs/08-Architecture.md) — for addon devs: cook pipeline, runtime pipeline, how to add a platform / format.

## When in doubt

If you just want video to *work* on a console without thinking about it:

> **Use PCV1 with the Balanced preset.** It plays cleanly on every supported platform with no encoder gotchas.

See [Docs/02-Formats.md](Docs/02-Formats.md) for the longer answer.

## Project layout

```
Packages/com.polyphase.formats.video/
├── README.md                ← you are here
├── Docs/                    end user / dev documentation
├── package.json             addon metadata + per-platform deps
├── CMakeLists.txt           build (per-platform variant)
├── Source/                  addon source
│   ├── Assets/              VideoClip asset type
│   ├── Backends/            IVideoDecoder + per-format decoders + AsyncMediaPump
│   ├── Cook/                editor-only: cook to PCV1 / THP / N3MV containers
│   ├── Nodes/               VideoPlayer3D node
│   ├── Lua/                 Lua bindings
│   ├── Util/                small helpers
│   └── VideoPlayer.cpp      plugin entry point
├── Scripts/                 example Lua scripts
└── External/                vendored deps (FFmpeg on Windows, sb_image, ...)
```

## Known issues / open work

- **THP audio quality.** The in-house DSP-ADPCM encoder uses Nintendo's generic fixed coefficient table; on dense / wide-bandwidth audio this produces audible thinness and crackling. The proper fix (per-clip LPC analysis) is implemented but disabled — see [`.dev/thp_lpc_encoder_bug.md`](.dev/thp_lpc_encoder_bug.md) for the bug investigation handoff. Workaround: prefer PCV1 on Wii / GameCube.
- **N3MV (3DS hardware H.264).** Cook produces valid N3MV bytes; runtime decoder is scaffold-only (not wired through `mvdstd*`). Use PCV1 on 3DS.

## License

Addon code: same license as the Polyphase Engine.

FFmpeg (when installed for PC builds) is LGPL — when shipping, include FFmpeg's `LICENSE.txt` and make FFmpeg source available on request. Don't ship GPL or non-free FFmpeg variants.
