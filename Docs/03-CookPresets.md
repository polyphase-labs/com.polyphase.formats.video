# 03 — Cook Presets

A preset is a one-click bundle of cook parameters: video resolution, frame rate, JPEG quality (PCV1 / THP) or H.264 quality (N3MV), audio sample rate, audio channels. The inspector exposes the preset as a dropdown plus the raw knobs underneath, so you can pick a preset as your starting point and tweak.

## The four presets

| Preset | Video | Frame rate | JPEG q | Audio rate | Channels | Use case |
|---|---|---|---|---|---|---|
| **Custom** | (whatever you set) | — | — | — | — | escape hatch when no preset fits |
| **Tiny** | 240 × 135 | 15 fps | 13 (low) | 11025 Hz | 1 (mono) | UI stings, splash logos, short loops, anything where size matters more than fidelity |
| **Balanced** | 320 × 180 | 30 fps | 7 (med) | 22050 Hz | 2 (stereo) | **Default** for most content. Looks reasonable on TV-resolution displays. Audio is full stereo, decent quality. |
| **Quality** | 512 × 288 | 30 fps | 3 (high) | 22050 Hz | 2 (stereo) | Cinematic / hero content. Recognizable detail at viewing distance. Bigger file, slower decode. |

(`Custom` is what you get if you change any individual knob after picking a preset — the inspector just re-labels it as Custom and stops applying bundled changes.)

## Knob reference

If you go Custom, here's what each knob does:

### Cook Width / Cook Height

The output resolution. The cook scales the source via FFmpeg's `lanczos` filter then crops to these dimensions.

Constraints:
- Both dimensions are clamped to **1024 max**. Bigger doesn't help and overruns hardware memory budgets.
- For **3DS PCV1 / N3MV**, the runtime rounds up to a power-of-two for the GPU texture (240 × 135 cooks → 256 × 256 GPU texture, with the unused area UV-cropped at draw time). Don't try to cook 480 × 272 for 3DS — the rounded-up texture is 512 × 512, which doesn't fit at all.
- For **GameCube** specifically, anything above ~320 × 240 risks running out of MEM1 once you account for the rest of the engine. PCV1 frames are decoded to RGBA8 (= 4 bytes/pixel) so a 480p frame is 470 KB just for the working buffer.
- **N3MV (3DS H.264)** requires both dimensions to be even (rounded down internally to satisfy x264).

### Cook FPS

Output frame rate. The cook uses `ffmpeg -r N` to resample. Common values:
- 15: Tiny preset's choice. Acceptable for non-action content; ~half the data of 30 fps.
- 24: cinematic feel.
- 30: smooth, ~30 % more data than 24.
- 60: only useful for fast action; not all consoles can sustain decode + render at this rate.

### Cook JPEG Quality

PCV1 and THP both use MJPEG video. This is the FFmpeg `-qscale:v` value. **Lower is better quality / bigger file.** Range: 2–31.

- 2–4 (Quality preset uses 3): visually near-lossless. Big files.
- 5–8 (Balanced uses 7): noticeable but unobtrusive compression. Good default.
- 9–13 (Tiny uses 13): visible blocking on dense scenes. Acceptable for thumbnails and loops.
- 14+: ugly. Don't.

Doesn't apply to N3MV (which uses H.264 with a different quality knob, not exposed yet).

### Cook Audio Rate

Hz. Range: 8000 – 48000. Common values:

- 11025: Tiny. Tinny on music; OK on speech.
- 22050: Balanced / Quality. Sounds full enough for most game content.
- 32000: THP's "native" rate (Nintendo's commercial THPs often use this).
- 44100 / 48000: CD / DAT quality. Overkill for most game content; doubles the audio size of 22050.

### Cook Audio Channels

1 (mono) or 2 (stereo). Mono halves audio size at the cost of stereo imaging. Stereo doubles audio size but is what most source content actually is.

### Cook Format

The container to produce. See [Formats](02-Formats.md) for the full discussion. Possible values: `0` = PCV1, `1` = THP, `2` = N3MV. Mismatched format / platform combinations fall back to PCV1 automatically.

## Picking a preset

Decision tree:

1. **It's a UI element / loop / sting / thumbnail-style thing under 5 seconds:** Tiny.
2. **It's the main content** (cinematic, intro, gameplay video shown to the player): start with Balanced. If it looks/sounds insufficient, bump to Quality. If file size is a problem, go Custom and drop one knob (usually JPEG quality 7 → 10) before resorting to Tiny.
3. **You have a strict size budget across many clips:** Tiny across the board, then bump individual hero clips to Balanced. Don't try to half-step with Custom; the bundled balance of preset knobs is usually right.

## What changes when you change preset

The preset selector writes the per-knob values into the asset and marks it dirty. **It does NOT auto-cook.** You still have to run *Test Cook for Platform* (see [Asset Workflow](05-AssetWorkflow.md)) to regenerate the cooked payload at the new settings.

If you forget to re-cook after changing the preset, the packaged build will still ship the *previous* cook — the cooked bytes live in the `.oct` (or in a sidecar for THP/N3MV) and are independent of the cook-knob values stored alongside them.

## Per-platform notes

- **Windows / Linux:** the PC backend never reads the cook params (it plays the source `.mp4` directly). The values are still saved on the asset for portability when packaging for console.
- **3DS:** see the PoT-rounding note above. Tiny preset (240 × 135) rounds to 256 × 256 = ~262 KB GPU texture. Balanced (320 × 180) → 512 × 256 = ~520 KB. Quality (512 × 288) → 512 × 512 = ~1 MB. Pick Tiny or Balanced unless you have a specific need.
- **Wii / GameCube:** Quality preset is fine on Wii's 88 MB RAM. On GameCube's tighter 24 MB main RAM budget, Quality is risky once you have other assets loaded — measure first.

## Related

- [02 — Formats](02-Formats.md)
- [05 — Asset Workflow](05-AssetWorkflow.md)
