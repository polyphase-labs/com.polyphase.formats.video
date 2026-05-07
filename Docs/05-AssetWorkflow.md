# 05 — Asset Workflow

End-to-end: source video file → playable on hardware. What each step does, what gets stored where, what shows up in version control.

## The pipeline at a glance

```
  ┌───────────────┐       ┌──────────────┐       ┌─────────────────┐       ┌──────────┐
  │  source.mp4   │  ───▶ │  VideoClip   │  ───▶ │  Test Cook for  │  ───▶ │ Package  │
  │  (your asset) │       │  .oct        │       │  Platform       │       │ for      │
  └───────────────┘       │  + cook params│       │  (per platform) │       │ Platform │
                          └──────────────┘       └─────────────────┘       └──────────┘
                                 │                       │                        │
                                 │                       ▼                        │
                                 │                 sidecar file                   │
                                 │                 (THP / N3MV)                   │
                                 │                       │                        │
                                 └───────────────────────┴────────────────────────┘
                                                inlined into shipped .oct
```

## Step 1 — Import

Drag a video file (`.mp4`, `.mov`, `.webm`, `.mkv`, `.avi`, `.m4v`) into the editor's asset browser. The importer:

1. Probes the source via FFmpeg, extracts metadata (duration, dimensions, frame rate, audio rate / channels).
2. Stores the source bytes in the `VideoClip` asset's `mSourceData`.
3. Captures the codec hint (lower-case extension without dot) for the runtime decoder factory's PC path.
4. Sets default cook parameters (Balanced preset).

The result is a `.oct` file in your asset directory.

**On disk after import:**
```
Assets/Videos/
  IntroClip.oct        ← source bytes inline + cook params
  IntroClip.mp4.meta   ← (optional) per-asset platform mask + embed flag
```

`.oct` files are the canonical engine asset format; the original `.mp4` is no longer needed once imported.

## Step 2 — Set cook parameters

Click the imported `VideoClip` to open it in the inspector. You'll see:

- **Source info** (read-only): width, height, duration, FPS, codec hint, audio rate / channels.
- **Cook Preset:** dropdown → Custom / Tiny / Balanced / Quality. Picking a preset overwrites the cook knobs below it.
- **Cook Format:** dropdown → PCV1 / THP / N3MV. See [Formats](02-Formats.md).
- **Cook Width / Height / FPS / JPEG Quality / Audio Rate / Audio Channels:** the per-knob values. Editable individually for Custom mode.
- **Use Sidecar:** auto-defaults from Cook Format (THP / N3MV → true, PCV1 → false). Don't normally need to change manually.
- **Sidecar Path:** read-only, populated by Test Cook when sidecar mode is on.

Pick a preset (or set the knobs by hand for Custom). Save the asset (`Ctrl+S` or via the asset menu).

**Saving the asset alone does NOT cook.** It just persists the parameter values. Cooking happens in step 3.

## Step 3 — Test Cook for Platform

The cook is the slow step (it shells out to FFmpeg under the hood). It's a separate explicit action so you can iterate on cook parameters without re-cooking on every save.

1. Right-click the `VideoClip` in the asset browser → **Test Cook for Platform** → choose the target (GameCube, Wii, 3DS).
2. The cook runs. Progress streams to the editor's output panel ("Cooking VideoClip: ..." lines, then per-pass FFmpeg progress).
3. On success the cook either:
   - **Inlines the cooked payload into `mSourceData`** (PCV1 path), OR
   - **Writes a sidecar file next to the .oct** and clears `mSourceData` (THP / N3MV path). The sidecar lives at `Assets/Videos/IntroClip.cook.thp` (or `.cook.n3mv`).
4. The asset is marked dirty. Save it (`Ctrl+S`) to persist.

The cook is idempotent — re-cooking the same parameters produces the same bytes (give or take FFmpeg's encoder non-determinism, which the cook tolerates).

**On disk after a THP cook:**
```
Assets/Videos/
  IntroClip.oct          ← cook params, no payload bytes (mSourceData empty)
  IntroClip.cook.thp     ← cooked sidecar, version-controlled with the .oct
```

**On disk after a PCV1 cook:**
```
Assets/Videos/
  IntroClip.oct          ← cook params + cooked PCV1 bytes inline (mSourceData = PCV1 payload)
  (no sidecar)
```

## Step 4 — Package for Platform

When you "Build for Wii" / "Build for 3DS" / etc. via the editor's packaging window, the engine cooks every asset that hasn't been pre-cooked, copies the result into the build's RomFS / asset bundle, and links the executable.

For `VideoClip` specifically, the engine calls `VideoClip::SaveStream(stream, Platform::Wii)` (or whichever target). The `SaveStream` logic:

1. **If a sidecar exists at `mSidecarPath`** (from a prior Test Cook), reads its bytes and embeds them inline into the packaged `.oct`. The dev-machine sidecar path is *not* shipped — the runtime never tries to fopen it on the device.
2. **If `mSourceData` already contains cooked bytes** (PCV1 case, or a passthrough), embeds those.
3. **Otherwise**, runs the cook live during packaging and embeds the output.

Net result: the shipped `.oct` always contains everything the runtime needs. No external file dependencies on the device.

For PC (Windows / Linux) packaging, the cook is skipped entirely; `SaveStream` just writes the source bytes through and the runtime uses FFmpeg.

## Version control

What to commit:

- `IntroClip.oct` — yes. This is the canonical asset.
- `IntroClip.cook.thp` / `.cook.n3mv` — **yes**, if you've done a Test Cook. The sidecar lives next to the asset specifically so it gets version-controlled. Re-checking-out the project gives a coworker (or CI) the cooked bytes without having to re-run a slow cook.
- The original `.mp4` source — your call. It's not strictly needed once imported (the bytes live in the `.oct`), but keeping it lets you re-import with different settings later.

What's automatically excluded:

- `Intermediate/` — cook scratch space (FFmpeg's frame extraction, temp PCM, etc.). `gitignore`d.
- `Build/<Platform>/` — packaged build outputs.

## When to re-cook

Re-cook a clip when you:

- Change cook parameters (preset, format, dimensions, fps, JPEG quality, audio rate, channels).
- Change the source video (re-import).
- Pick up an addon update that changed the cook code (e.g. a bug fix in `ThpPacker.cpp` or `DspAdpcm.cpp`).

Existing cooked sidecars / inlined `.oct` payloads do *not* auto-update on cook code changes. Watch for an editor warning about cook-code-version mismatch (if implemented) or just re-cook everything when you pull a major addon update.

## Common workflow gotchas

- **You changed the cook preset and the build still ships the old quality.** You forgot to re-cook. Test Cook → save → repackage.
- **Test Cook produces a sidecar but the build complains "sidecar not found at M:\..."**. This was a real bug; the fix is in `VideoClip.cpp::SaveStream` (sidecar bytes get hydrated inline at packaging time). If you see this on a recent addon version, file an issue — the dev-machine sidecar path should never appear on the runtime device.
- **Audio is missing on hardware but the editor preview was fine.** The PC path uses the source `.mp4`'s audio directly. The console path uses cooked PCM / ADPCM. If the cook step skipped audio (FFmpeg failed silently on the audio extraction pass), the cooked clip has video-only. Re-run Test Cook and watch the editor log for "no audio" warnings.

## Related

- [02 — Formats](02-Formats.md) — pick the right format for the platform.
- [03 — Cook Presets](03-CookPresets.md) — preset values and per-knob meaning.
- [07 — Troubleshooting](07-Troubleshooting.md) — what to do when cooks fail or playback breaks.
