# 04 — Platform Support

What works, what's experimental, what's stubbed.

## At a glance

| Platform | Video | Audio | Status |
|---|---|---|---|
| Windows x64 | FFmpeg passthrough | XAudio2 streaming voices | **Stable** |
| Linux x64 | FFmpeg passthrough | PulseAudio streaming voices | **Stable** |
| Wii | PCV1 / THP software decode | ASND streaming voices (libogc2) | **PCV1 stable; THP plays but audio quality limited** |
| GameCube | PCV1 / THP software decode | ASND streaming voices (libogc2) | **Same as Wii** |
| Nintendo 3DS | PCV1 software decode | NDSP streaming voices (libctru) | **PCV1 stable; N3MV scaffolded but not finished** |
| Android | — | — | Not supported |
| Switch / PS5 / Xbox | — | — | Not supported |

## Per-platform detail

### Windows / Linux

The PC path is the reference implementation:

- Decoder: `FFmpegVideoDecoder` reads the source `.mp4` / `.mov` / `.webm` / `.mkv` / `.avi` / `.m4v` directly. No cook step needed.
- Audio: streamed at 48 kHz stereo through the engine's per-platform streaming voice API.
- Editor preview uses the same path.

You don't need to think about cook formats on PC. They only come into play when you package for a console.

**Windows-specific:** ships FFmpeg DLLs alongside the addon binary; see the addon's main `README.md` for FFmpeg install instructions. **Linux-specific:** FFmpeg is picked up via `pkg-config` — install `libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev` (or your distro's equivalent).

### Wii (libogc2)

- **Video:** PCV1 plays clean. THP plays clean for video; the audio path has a known quality limitation (see THP note below).
- **Audio:** uses libogc2's ASND streaming voices. The engine configures voices with `VOICE_*_LE` and the addon is careful to write decoded PCM samples in little-endian byte order (PowerPC is big-endian, so a naïve `int16_t` store would produce BE bytes that ASND would byte-swap into garbage — see `WriteSampleLE` in `ThpVideoDecoder.cpp`).
- **Audio buffer pipeline:** the engine queues audio in 50 ms chunks (high-water-mark policy), keeping ~100 ms in ASND's 2-slot ring. This handles main-thread tick jitter on Wii's modest CPU without producing audible gaps. Tested at 30 Hz tick rate under heavy GX upload + JPEG decode load.
- **Memory:** PCV1 is fully RAM-resident. With Tiny preset (~1 MB per 30 s clip) you have generous headroom on Wii's 88 MB. With Quality preset (~10–15 MB) you're still fine but check your total scene budget.

**THP audio quality limitation:** the in-house DSP-ADPCM encoder uses Nintendo's generic fixed coefficient table. On dense / wide-bandwidth music you'll hear thinness and crackling. Speech and simple music sound OK. The proper fix (per-clip LPC analysis) is implemented but disabled — see `Cook/DspAdpcm.cpp` and `.dev/thp_lpc_encoder_bug.md` for the bug investigation handoff. **Recommendation: prefer PCV1 on Wii.**

### GameCube (libogc2)

Same code as Wii (shared `PLATFORM_DOLPHIN` define). The differences are:

- 24 MB main RAM (vs Wii's 88 MB total). Be conservative on cook resolution / bitrate.
- Same ASND audio backend, same THP audio quality limitation.
- All cook outputs are tested via Dolphin emulator first; real-hardware testing is recommended before shipping (Dolphin is not cycle-accurate on AUDIO_* / ARAM timing).

### Nintendo 3DS (libctru)

- **Video:** PCV1 is the only working option today. N3MV scaffolding exists in `Backends/MvdVideoDecoder.cpp` but the runtime is a stub — the cook produces N3MV bytes but the decoder doesn't currently feed them through the `mvd` service.
- **Audio:** NDSP streaming voices via the engine's audio API. Decoded samples are written in host byte order; 3DS's ARM is little-endian so no swap is needed.
- **GPU:** uses Citro3D. The runtime rounds the decoded RGBA8 frame up to the next power-of-two dimension (the 3DS GPU is PoT-only). The padded area is masked off via UV scaling on the `Quad` (`Texture::GetUVMax()` returns the visible-area fraction). Color order is ABGR in GPU memory (the runtime swaps RGBA → ABGR on each frame upload).
- **Vertical flip:** `GX_DisplayTransfer` in the runtime sets `GX_TRANSFER_FLIP_VERT(1)` so the source-order frame ends up upright on the 3DS top screen.

**Old-3DS vs New-3DS:** Old-3DS has 96 MB RAM, New-3DS has 256 MB. PCV1 with Tiny preset works on both. PCV1 with Balanced is fine on New-3DS, tight on Old-3DS for long clips. PoT-rounding cost is the same on both.

### Other platforms

- **Android:** not supported. The audio backend stub exists but the build doesn't currently target Android.
- **Switch / PS5 / Xbox / consoles under NDA:** not supported.

If you need one of these, the work splits into engine (per-platform streaming audio backend) + addon (per-platform decoder). See the addon's main `README.md` "Next steps for other platforms" section for an outline; expect ~1–4 weeks per console depending on complexity.

## Build flags

The addon is gated by these defines, set automatically by the engine's per-platform build glue:

| Define | Set on |
|---|---|
| `POLYPHASE_WITH_FFMPEG=1` | Windows, Linux |
| `PLATFORM_WINDOWS=1` | Windows |
| `PLATFORM_LINUX=1` | Linux |
| `PLATFORM_DOLPHIN=1` | Wii + GameCube (shared) |
| `PLATFORM_3DS=1` | 3DS |
| `PLATFORM_GAMECUBE=1` | GameCube only (sub-flag of `PLATFORM_DOLPHIN`) |
| `PLATFORM_WII=1` | Wii only (sub-flag of `PLATFORM_DOLPHIN`) |

You generally don't have to set these by hand. They flow from the project's "Build for Platform" step.

## Per-platform deps

`package.json` declares per-platform native dependencies via `nativePerPlatform`. Currently:

- **Windows:** FFmpeg shared libs from `External/ffmpeg/{include,lib,bin}`, copied to the binary output at build time.
- **Linux:** FFmpeg via system `pkg-config`. No bundled binaries.
- **GameCube / Wii / 3DS:** no extra deps; uses devkitPro's bundled libraries (libogc2, libctru) which the engine already links against.

If you add a new platform, extend `nativePerPlatform` with that platform's defines / include dirs / lib dirs / link libs.

## Related

- [Formats](02-Formats.md) — what's playable on each platform.
- [Architecture](08-Architecture.md) — where the per-platform code lives.
- Addon root `README.md` — historical platform-bringup planning notes (some still relevant).
