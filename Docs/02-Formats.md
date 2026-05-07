# 02 — Formats: PCV1 / THP / N3MV

This is the most important decision you'll make as an end user. Pick the wrong format and your build won't play; pick a suboptimal one and audio quality or file size suffers. The good news: there's a clear default per platform and a clear escape hatch when the default isn't a fit.

## Quick answer

| Platform | Default format | When to consider an alternative |
|---|---|---|
| Windows / Linux | **Source passthrough** (FFmpeg) | Never — the PC backend just plays the original `.mp4` directly. No cook needed. |
| GameCube / Wii | **PCV1** | Use THP only if you specifically need the audio compression (~3.5×) and you can tolerate reduced audio quality. |
| Nintendo 3DS | **PCV1** (and currently the only working option) | N3MV is scaffolded for hardware H.264 via MVD but not finished — see [Platform Support](04-PlatformSupport.md). |

If you only read one paragraph: **for console builds, cook to PCV1 with the Balanced preset.** That's the recommended default.

## What each format actually is

### PCV1 — "Polyphase Video 1"

A custom container designed for Polyphase consoles. Layout: a small header, a raw uncompressed PCM audio block, and a sequence of MJPEG-compressed video frames. Software-decoded at runtime by `PcvVideoDecoder`.

- **Pros:** simple, robust, identical decode path on every console (one source of truth). Audio is uncompressed PCM so it sounds exactly like the source. MJPEG video looks decent at any preset and decodes fast on PowerPC / ARM. The format is endian-independent (the runtime reads multi-byte fields explicitly LE, so x86-cooked bytes work on PowerPC and ARM consoles).
- **Cons:** no audio compression, so longer clips get bigger than they need to be. Video compression is per-frame only (no inter-frame prediction), so high-motion content gets larger than a real codec would produce.
- **Storage shape:** the entire cooked payload is **inlined into the `VideoClip` `.oct` file**. No sidecar files. Loaded into RAM at clip open.

PCV1 is the safe choice. Use it whenever you can.

### THP — "Hollywood THP" (GameCube / Wii native)

Nintendo's stock streaming-video container, used by every commercial Wii / GameCube title that plays cinematics. Per-frame MJPEG video plus DSP-ADPCM compressed audio (~3.5× compression vs raw PCM). Software-decoded at runtime by `ThpVideoDecoder`.

- **Pros:** smaller file size than PCV1 (audio is compressed), file-streaming-from-disk supported (so a long clip doesn't have to be RAM-resident).
- **Cons:** **audio quality is currently limited.** The in-house DSP-ADPCM encoder uses Nintendo's generic fixed coefficient table. On dense / wide-bandwidth music (lots of high-frequency content + bass) you'll hear thinness and mild crackling. Speech and simple music sound OK. The "real" fix — per-clip LPC analysis to compute the coefficient table from the source PCM — is implemented but **disabled** because it currently produces broken audio (see `.dev/thp_lpc_encoder_bug.md` if you're picking up the bug).
- **Storage shape:** at editor / development time the cooked THP bytes are written to a **sidecar file** next to the `.oct` (e.g. `MyClip.oct.cook.thp`). The sidecar gets version-controlled alongside the asset. **At packaging time the sidecar bytes are read back and embedded inline into the .oct** that ships with the build, so the runtime never has to find a sidecar at a hardware-side path.

Use THP only if file size is a real constraint. For most projects, PCV1 is fine and sounds better.

### N3MV — "Nintendo 3DS Movie Video"

A custom 3DS-targeted container designed around the 3DS's hardware H.264 decoder (the `mvd` service). Header + per-frame H.264 NAL payload + raw PCM audio.

- **Pros:** would deliver hardware-accelerated H.264 decode on 3DS — substantially better quality at lower CPU cost than PCV1's MJPEG.
- **Cons:** **the runtime decoder is currently scaffolding only.** The cook produces valid N3MV bytes; runtime playback isn't wired through `mvdstd*`. Use PCV1 on 3DS until N3MV is finished.
- **Storage shape:** sidecar at edit time (same pattern as THP), inlined into `.oct` at packaging time.

Don't pick N3MV for shipping content yet.

## How the runtime picks the decoder

When you call `Play()` on a `VideoPlayer3D` with a `VideoClip` set, `VideoDecoderFactory::CreateVideoDecoderForClip()` sniffs the first 4 bytes of the clip's payload:

| Magic | Decoder | Used by |
|---|---|---|
| `PCV1` | `PcvVideoDecoder` | All consoles (and editor on consoles' platform tab) |
| `THP\0` | `ThpVideoDecoder` | GameCube / Wii |
| `N3MV` | `MvdVideoDecoder` | 3DS (currently a stub) |
| (none) | `FFmpegVideoDecoder` | Windows / Linux source-passthrough |

The factory routes by **bytes**, not by file extension or by what the cook *should* have produced — so an asset that says it was cooked as THP but is actually PCV1 (e.g. cook fell back) will still play; the runtime trusts the magic.

## Will the wrong format break my build?

The cook validates: if you set Cook Format = THP but the platform is 3DS, the cook automatically falls through to PCV1 (`useThp` requires GameCube/Wii). If you set Cook Format = N3MV but the platform is GameCube, same fall-through. So you can't accidentally ship a THP file in a 3DS build.

## Storage / RAM trade-offs

| Format | 30 s clip @ Tiny preset | 30 s clip @ Balanced preset | 30 s clip @ Quality preset |
|---|---|---|---|
| PCV1 | ~1 MB | ~3–5 MB | ~10–15 MB |
| THP | ~700 KB | ~2–3 MB | ~6–9 MB |
| N3MV | ~500 KB | ~1.5–2 MB | ~4–6 MB *(if it worked)* |

These are rough — exact size depends heavily on motion in the clip. Treat them as orders of magnitude, not targets.

PCV1 is fully RAM-resident. THP supports file-streaming on Wii (lower peak RAM for long clips), but the addon currently embeds the THP bytes inline into the `.oct` for hardware shipping anyway, so streaming-from-disk is a future feature, not a present one.

## Picking a format — flowchart

1. Shipping for Windows / Linux only? → don't cook. Just play the source `.mp4`. Done.
2. Shipping for 3DS? → **PCV1.** N3MV isn't finished.
3. Shipping for Wii / GameCube and total clip size doesn't matter? → **PCV1.**
4. Shipping for Wii / GameCube and you specifically need to compress the audio? → **THP**, but listen to a cooked sample on actual hardware before committing — quality is content-dependent.
5. Shipping a project where every byte matters? → split decision per clip: speech / simple music → THP, dense / orchestral / dynamic music → PCV1 even if it's bigger.

## Related

- [03 — Cook Presets](03-CookPresets.md) — the per-preset numbers that actually drive size/quality.
- [05 — Asset Workflow](05-AssetWorkflow.md) — how to set the format on a clip and run a test cook.
- [04 — Platform Support](04-PlatformSupport.md) — what's stable, what's experimental, what's stubbed.
