# VideoPlayer Addon — Documentation

Plays video files in Polyphase Engine scenes. Decoded frames are exposed as a `Texture` you can sample from any material or assign to a `Quad` widget. Audio is streamed through the engine's per-platform streaming voice API.

## For end users (game devs)

Start here if you're shipping a game and want to play video from your scene.

- **[01 — Getting Started](01-GettingStarted.md)** — import an `.mp4`, drop it on a `VideoPlayer3D`, hit Play.
- **[02 — Formats: PCV1 / THP / N3MV](02-Formats.md)** — *what compression to pick for which platform.* Read this before cooking anything for console.
- **[03 — Cook Presets](03-CookPresets.md)** — `Tiny` / `Balanced` / `Quality` / `Custom`, what each preset means, file size vs quality trade-offs.
- **[04 — Platform Support](04-PlatformSupport.md)** — Windows / Linux / Wii / GameCube / 3DS — what's working, what's stubbed, what's known-limited.
- **[05 — Asset Workflow](05-AssetWorkflow.md)** — import → cook → save → package. Sidecar files. Version control. What gets shipped to each platform.
- **[06 — Runtime API](06-RuntimeAPI.md)** — `VideoPlayer3D` node, Lua bindings, signals (`OnReady`, `OnFinished`, `OnError`).
- **[07 — Troubleshooting](07-Troubleshooting.md)** — black screen, wrong colors, audio glitches, sync drift, looping issues.

## For addon developers

Start here if you're extending the addon — adding a platform, adding a codec, fixing a bug.

- **[08 — Architecture](08-Architecture.md)** — cook pipeline, runtime pipeline, `IVideoDecoder` interface, `VideoDecoderFactory`, `AsyncMediaPump`, where each piece lives.

## When in doubt

If you just want video to *work* on a console without thinking about it, **use PCV1 with the `Balanced` preset.** It plays cleanly on every supported platform with no encoder gotchas. See [Formats](02-Formats.md) for the longer answer.
