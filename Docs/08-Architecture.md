# 08 — Architecture (for addon developers)

For addon devs / contributors. Skip if you're just shipping a game with the addon.

## High-level shape

The addon has two halves:

1. **Cook side** (`Source/Cook/*`, `Source/Assets/VideoClip*`) — runs in the editor only (`#if EDITOR`). Takes a source `.mp4` and produces a per-platform cooked container (PCV1 / THP / N3MV). Lives in the editor build, runs at asset import / package time.
2. **Runtime side** (`Source/Backends/*`, `Source/Nodes/VideoPlayer3D*`) — runs everywhere. Decodes the cooked container, hands per-frame RGBA8 to the engine's `Texture::UpdatePixels`, hands per-chunk PCM to the engine's streaming voice API.

Plus glue: `Source/VideoPlayer.cpp` (plugin entry / factory registration), `Source/Lua/*` (Lua bindings), `Source/Util/*`.

```
                       ┌─────────────────┐
       editor only ──▶ │  Cook pipeline  │
                       │  (Source/Cook)  │
                       └────────┬────────┘
                                │
                                ▼
                       ┌─────────────────┐
                       │  VideoClip.oct  │
                       │  + sidecar?     │
                       └────────┬────────┘
                                │
                                ▼
   shipping runtime ───▶ ┌─────────────────────────────┐
                         │  VideoDecoderFactory        │  routes by magic
                         │  (Source/Backends)          │
                         └────────┬────────────────────┘
                                  │
                                  ▼
                         ┌─────────────────────────────┐
                         │  IVideoDecoder impls        │  PCV1 / THP / N3MV / FFmpeg
                         │  (Source/Backends)          │
                         └────────┬────────────────────┘
                                  │
                  per-frame RGBA8 │      per-chunk PCM
                  + audio chunks  │
                                  ▼
                         ┌─────────────────────────────┐
                         │  AsyncMediaPump             │  decode worker thread
                         │  (Source/Backends)          │
                         └────────┬────────────────────┘
                                  │
                                  ▼
                         ┌─────────────────────────────┐
                         │  VideoPlayer3D node         │  drives the audio clock
                         │  (Source/Nodes)             │  + pulls from pump per Tick
                         └────────┬────────────────────┘
                                  │
                                  │ Texture::UpdatePixels      AUD_SubmitStreamBuffer
                                  ▼                            (engine API)
                         ┌─────────────────┐         ┌──────────────────────────────┐
                         │  Texture        │         │  Engine streaming voice      │
                         │  (engine asset) │         │  (ASND/NDSP/XAudio/PulseAudio)│
                         └─────────────────┘         └──────────────────────────────┘
```

## File tree

```
Packages/com.polyphase.formats.video/
├── package.json                  Addon metadata + per-platform deps
├── CMakeLists.txt                Build (per-platform variant)
├── Source/
│   ├── VideoPlayer.cpp           Plugin entry, FORCE_LINK_CALL for asset/node types
│   ├── EngineAPIAccess.h         Shim for engine functions the addon needs
│   │
│   ├── Assets/
│   │   ├── VideoClip.{h,cpp}     The asset type. Import / SaveStream / LoadStream / cook params.
│   │
│   ├── Backends/
│   │   ├── IVideoDecoder.h       Interface — every decoder implements this.
│   │   ├── VideoDecoderFactory.{h,cpp}  Routes by magic bytes / file extension.
│   │   ├── NullVideoDecoder.{h,cpp}     Test bars (when no real decoder routes).
│   │   ├── FFmpegVideoDecoder.{h,cpp}   PC backend (Windows / Linux).
│   │   ├── PcvVideoDecoder.{h,cpp}      PCV1 — software decode, all consoles.
│   │   ├── ThpVideoDecoder.{h,cpp}      THP — software decode, GameCube / Wii.
│   │   ├── MvdVideoDecoder.{h,cpp}      N3MV — 3DS hardware H.264 (currently scaffold-only).
│   │   ├── AsyncMediaPump.{h,cpp}       Worker-thread shell around any IVideoDecoder.
│   │
│   ├── Cook/                     editor-only (#if EDITOR)
│   │   ├── DolphinVideoCook.{h,cpp}    PCV1 cook (FFmpeg → MJPEG frames + raw PCM).
│   │   ├── ThpPacker.{h,cpp}           THP cook (FFmpeg → MJPEG frames + DSP-ADPCM audio + THP container).
│   │   ├── N3dsMvdCook.{h,cpp}         N3MV cook (FFmpeg → H.264 NAL stream + raw PCM + N3MV container).
│   │   ├── DspAdpcm.{h,cpp}            DSP-ADPCM encoder/decoder shared by THP path.
│   │
│   ├── Nodes/
│   │   ├── VideoPlayer3D.{h,cpp}       The Node3D that ties it all together.
│   │
│   ├── Lua/
│   │   ├── VideoPlayer3D_Lua.{h,cpp}   Lua bindings (auto-generated stubs).
│   │
│   └── Util/                     small helpers (paths, exec, etc.)
│
├── Scripts/                      example / demo Lua scripts
├── External/                     vendored deps (FFmpeg on Windows, sb_image, …)
└── Docs/                         (you are here)
```

## Cook pipeline

Editor-only. `#if EDITOR` everywhere.

Entry point is `VideoClip::SaveStream(stream, platform)`:

1. **Header.** Write codec hint, source dimensions, duration, frame rate, source audio rate / channels.
2. **Per-platform payload.**
   - PC (Windows / Linux): write source bytes through. Runtime FFmpeg decodes from those bytes.
   - GameCube / Wii / 3DS: route to the appropriate cook function based on `mCookFormat` (and validate the format / platform combo). Cook function returns a `std::vector<uint8_t>` of cooked bytes; those get written inline into the stream.
3. **Sidecar hydration.** If the asset has `mSidecarPath` set (from a prior `TestCookForPlatform`) and `mSourceData` is empty (TestCook cleared it), read the sidecar bytes from disk and embed them inline. This is what makes hardware builds work — the runtime never has to find a sidecar at a dev-machine path.
4. **Cook params trailer.** Magic-prefixed (`0xC0017AC1`) so `LoadStream` can detect old asset versions. Carries Cook Width / Height / FPS / JPEG Quality / Audio Rate / Audio Channels / Format / Preset / Use Sidecar / Sidecar Path. **For hardware platforms, sidecar path is persisted as empty in the trailer** so the runtime can't try to fopen the dev path.

The cook functions all follow a common pattern (see `DolphinVideoCook.cpp` for the cleanest example):

1. Resolve FFmpeg binary path.
2. Pass 1: extract MJPEG frames at target resolution / fps / quality into a temp directory.
3. Pass 2: extract raw PCM audio at target rate / channels.
4. Read the JPEG frames + PCM into memory.
5. Container muxing: write header + frame table + interleaved frame data + audio.
6. Return the cooked bytes.

THP and N3MV add additional steps (DSP-ADPCM encoding for THP; H.264 NAL framing for N3MV).

## Runtime pipeline

`VideoDecoderFactory::CreateVideoDecoderForClip(VideoClip*)`:

- Sniff the first 4 bytes of `clip->GetSourceData()`.
- Match against `PCV1` / `THP\0` / `N3MV` and instantiate the right decoder.
- Fall back to FFmpeg on PC if no magic matches and the codec hint is a known PC format.
- Fall back to `NullVideoDecoder` (test bars) if nothing routes.

The decoder's interface is `IVideoDecoder`:

```cpp
class IVideoDecoder {
public:
    virtual bool Open(const char* path)                                = 0;
    virtual bool OpenMemory(const uint8_t* data, size_t size,
                            const char* codecHint)                     = 0;
    virtual void Close()                                               = 0;
    virtual bool DecodeNextFrame(DecodedFrame& outFrame)               = 0;
    virtual AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk)  = 0;
    virtual bool Seek(double seconds)                                  = 0;
    virtual VideoStreamDesc GetVideoDesc() const                       = 0;
    virtual AudioStreamDesc GetAudioDesc() const                       = 0;
};
```

The decoder is wrapped by `AsyncMediaPump`, which runs the decode methods on a worker thread and exposes ring-buffered video frames + audio chunks to the main thread. The pump handles seek requests, end-of-stream signalling, and back-pressure (small bounded queues).

`VideoPlayer3D::Tick()` (main thread):

1. Read the audio clock from the engine streaming voice (`AUD_GetStreamPlayedSamples`).
2. Compute current playhead seconds.
3. Pop a video frame from the pump if one is due (`pump->TryPopDueVideoFrame(playheadSec + tolerance)`).
4. Upload the frame to the `Texture` (`GFX_UpdateTextureResourcePixels`).
5. Pop audio chunks from the pump and submit to the streaming voice (`AUD_SubmitStreamBuffer`).
6. Check loop condition; if `pump->IsEndOfStream() && playheadSec >= lastFrameSec + (1/fps)` and looping is on, seek the pump to 0 and reset the audio voice.

## Per-platform considerations

### PC (Windows / Linux)

`FFmpegVideoDecoder` does standard FFmpeg things: `avformat_open_input` → demux → `avcodec_send_packet` / `avcodec_receive_frame` → `sws_scale` to RGBA8 / `swr_convert` to int16 stereo PCM. Nothing console-specific. The runtime always uses this on PC.

### GameCube / Wii (PowerPC, libogc2)

`PLATFORM_DOLPHIN` is set. PCV1 and THP decoders both compile here. Key gotchas:

- **Endianness.** PowerPC is BE. Cooked containers are mostly BE on disk (THP follows Nintendo's spec). PCV1 is explicitly LE on disk and the runtime reads multi-byte fields with explicit `ReadU32LE` calls so it's endian-independent.
- **Audio sample byte order.** ASND is configured with `VOICE_*_LE`. Decoded PCM samples MUST be stored little-endian in memory. PCV1 satisfies this naturally (samples were x86-LE at cook time). THP runtime decoder reconstructs samples on-device, so it explicitly uses a `WriteSampleLE` helper to force LE byte order regardless of host endian. See `ThpVideoDecoder.cpp`.
- **ASND lifecycle.** Drained streaming voices (no callback) go to `SND_UNUSED`, not `SND_WAITING`. `ASND_AddVoice` rejects `SND_UNUSED` voices. The engine handles this: `Audio_Dolphin.cpp` registers a no-op IRQ callback so drained voices stay in `SND_WAITING`, and falls back to `SetVoice` when the status is `SND_UNUSED`. Don't change this without reading the memory entry on libogc2 ASND lifecycle in the engine's `.claude/` notes.
- **Submit pipeline.** Engine batches audio submits to a 50 ms high-water-mark policy so the 2-slot ASND ring carries ~100 ms — enough to ride out main-thread tick jitter. Don't lower the threshold without re-testing on hardware.

### 3DS (ARM, libctru)

`PLATFORM_3DS` is set. PCV1 decoder compiles here. N3MV decoder is scaffold-only.

- **Endianness.** ARM is LE; same as cook host. No swaps needed for in-memory data.
- **GPU.** Uses Citro3D. Streaming RGBA8 textures must be **PoT-padded** (the GPU is PoT-only). The engine handles padding + UV-cropping; addon side just hands raw RGBA8 frames.
- **Color order.** GPU stores RGBA8 textures in **ABGR** byte order in memory. The engine's `GFX_UpdateTextureResourcePixels` (3DS backend) does the per-pixel swap on upload.
- **Vertical flip.** `GX_DisplayTransfer` sets `GX_TRANSFER_FLIP_VERT(1)` so source-order frames end up upright.
- **Audio.** NDSP voices via the engine's audio API. Decoded PCM samples are host-endian (LE on 3DS) which matches NDSP's expectation directly. No swap needed.

## Adding a new platform

You'll need:

1. **Engine side:** a streaming voice backend in `Engine/Source/Audio/<Platform>/Audio_<Platform>.cpp` implementing `AUD_OpenStream` / `AUD_SubmitStreamBuffer` / `AUD_GetStreamPlayedSamples` / `AUD_FlushStream` / `AUD_CloseStream` / `AUD_SetStreamVolume` / `AUD_SetStreamPaused`. The submit pipeline shape (residue accumulation + high-water-mark) is a useful template — see `Audio_Dolphin.cpp`.
2. **Addon side:** if a platform-specific video decoder is needed (hardware decode, native container), add a new file under `Source/Backends/<Platform>VideoDecoder.{h,cpp}` implementing `IVideoDecoder`. Register it with `VideoDecoderFactory` under `#if PLATFORM_<X>`.
3. **package.json:** extend `nativePerPlatform` with the platform's defines / include dirs / lib dirs / linker libs.
4. **CMakeLists.txt:** add a per-platform `if(PLATFORM_<X>)` branch with any platform-specific compile / link flags.

## Adding a new container format

If the existing PCV1 / THP / N3MV / FFmpeg coverage doesn't fit:

1. Implement `IVideoDecoder` in `Source/Backends/MyVideoDecoder.{h,cpp}`.
2. Pick a 4-byte magic for your container. Add a branch to `VideoDecoderFactory::CreateVideoDecoderForClip()` to route by it.
3. Add a cook function in `Source/Cook/MyCook.{h,cpp}` that produces bytes starting with that magic.
4. Add a cook-format dropdown entry in `VideoClip`'s inspector (`HandlePropChange` / `GatherProperties`).
5. Wire `VideoClip::SaveStream` to call your cook function for the right platform / format combo.

The runtime decoder factory routes by **bytes**, not by what the cook *should* have produced — so the runtime is forgiving of mismatched format / platform combos at the cost of a possible fall-through to test bars.

## Memory / performance budget

The decode path is on the worker thread, so per-frame decode time mostly doesn't affect the main thread. What does affect the main thread:

- **Texture upload.** Per-frame `GFX_UpdateTextureResourcePixels` runs on the main thread. On Wii / GameCube this is a software 4×4 swizzle into GX_TF_RGBA8 layout (~10 ms for a 320×180 frame on Wii). On 3DS it's a CPU RGBA→ABGR swap + `GX_DisplayTransfer` blit. Keep this in mind when picking cook resolution.
- **Audio submit.** `AUD_SubmitStreamBuffer` per-tick on the main thread. Cheap.
- **`AdvanceStream` (engine audio).** Polled per tick; cheap.

The engine's submit pipeline is sized for 30 Hz main thread on Wii. Faster main thread is no problem; slower (< 25 Hz) might produce audible audio gaps.

## Testing

The addon currently has minimal unit tests — most testing is integration-level via packaging for each platform and running on hardware / emulator. For audio path testing specifically:

- Editor preview: PC FFmpeg path (Windows + Linux).
- Wii hardware: actual hardware via SD card / wad install.
- Wii emulator: Dolphin (DSP-HLE for fast iteration; DSP-LLE for accuracy testing).
- 3DS hardware: actual hardware via 3DSX / CIA.
- 3DS emulator: Azahar / Citra.

When picking up the THP audio quality bug (`.dev/thp_lpc_encoder_bug.md`), build a self-contained round-trip test on PC (no hardware needed; the cook code is portable C++) before iterating on hardware. See the bug doc for the test signal recipe.

## Related

- `.dev/thp_lpc_encoder_bug.md` — open bug investigation handoff for the LPC encoder.
- Engine's `.claude/memory/` — gotchas memorialized: libogc2 ASND lifecycle, runtime PCM endianness, addon DLL CRT fingerprint, addon hot-reload cleanup. Read these before changing audio or hot-reload code.
- Addon root `README.md` — historical platform-bringup planning.
