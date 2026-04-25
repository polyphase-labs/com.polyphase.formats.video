# VideoPlayer — Polyphase Engine Native Addon

Plays video files in Polyphase Engine scenes. The current decoded frame is exposed
as a `Texture` that can be sampled by any material or assigned to a `Quad` widget.

## ## Setting up FFmpeg (Windows)

The addon builds without FFmpeg but only the null backend is available. To play real
video files, drop a prebuilt LGPL-clean shared FFmpeg into the addon:

1. Download the "shared" Windows build from `https://www.gyan.dev/ffmpeg/builds/`
   (the file named something like `ffmpeg-release-full-shared.7z`).
2. Extract it. Inside you'll find `include/`, `lib/`, and `bin/` directories.
3. Copy those three directories into `External/ffmpeg/` inside this addon so the
   layout looks like:
   
   ```
   Packages/com.polyphase.formats.video/
     External/
       ffmpeg/
         include/
         lib/
         bin/
   ```
4. Rebuild the addon. CMake logs `VideoPlayer: found FFmpeg at ...` on success.
5. The post-build step copies the FFmpeg DLLs next to `com.polyphase.formats.video.dll` so they
   resolve at load time.

Do **not** use FFmpeg builds configured with `--enable-gpl` or `--enable-nonfree`.
The stock LGPL build keeps the addon and your game redistribution clean. When
shipping a game that uses this addon, include FFmpeg's `LICENSE.txt` and make the
FFmpeg source available on request.

## Usage from Lua

```lua
function OnStart(self)
    self.player = self:SpawnChild("VideoPlayer", "Player")
    self.player:SetFilePath("Packages/com.polyphase.formats.video/Assets/test.mp4")
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

## Applying a video to a material

```lua
local mat = LoadAsset("Materials/VideoWall")
mat:SetTextureParameter("Albedo", self.player:GetTexture())
```

The texture handle is stable across frames; the pixel contents update automatically
each tick.

## Adding future backends

Implement `IVideoDecoder` in `Source/Backends/` and extend
`Source/Backends/VideoDecoderFactory.cpp` to select it for the relevant platform or
file extension. Non-FFmpeg backends (Theora on Wii, custom codecs on GameCube or
3DS, MediaCodec on Android, AVPlayer on PS5) should be gated on platform compile
flags.

## Next steps for other platforms

The current shipping target is **Windows x64** (FFmpeg + XAudio2 streaming voices +
async decode worker). Other platforms compile cleanly but degrade in known ways.
The following sections list the concrete work items to bring each platform up to
parity. Each item is tagged **[engine]** or **[addon]** to make clear which repo
owns the change.

### Linux (x64)

Status today: video, audio, and native-addon hot-reload all work via the
PulseAudio streaming backend and the dlopen-based factory strip. Build the
engine + addon with the standard CMake flow; PulseAudio and FFmpeg are picked
up via pkg-config.

Reference for what landed (each item below is now in-tree, kept here as a map):

1. **[engine] `StripFactoriesFromModule` for Linux** —
   `Engine/Source/Editor/Addons/NativeAddonManager.cpp`. Uses
   `dlinfo(handle, RTLD_DI_LINKMAP, &lm)` to get the loaded module's base, then
   `dladdr((void*)factory, &info)` per entry; drops factories whose
   `info.dli_fbase` matches the unloaded module's base. Mirrors the Windows
   `GetModuleInformation` + `lpBaseOfDll` range check.

2. **[engine] Streaming audio in `Audio/Linux/Audio_Linux.cpp`** — PulseAudio
   async API (`pa_threaded_mainloop` + one `pa_stream` per voice).
   `AUD_OpenStream` lazily inits the context so headless / pulse-less systems
   degrade to silent video. `AUD_GetStreamPlayedSamples` reads
   `pa_stream_get_time`, falling back to a submitted-sample counter before the
   first sample plays out so the audio-as-master sync doesn't wedge at zero.
   `libpulse` is pulled in via `pkg_check_modules` in `Engine/Source/CMakeLists.txt`.

3. **[engine] EngineEditor symbol export** —
   `Standalone/CMakeLists.txt` sets `ENABLE_EXPORTS` so addon `.so`s loaded via
   dlopen can resolve engine symbols against the executable.

4. **[addon] FFmpeg via system pkg-config** — `CMakeLists.txt` `elseif(UNIX)`
   branch. Install on Debian/Ubuntu:
   `apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev`.
   FFmpeg 4.0 is the minimum (`swr_alloc_set_opts2`); 5.x recommended.

5. **[addon] Verify hot-reload** — open a video, edit `VideoPlayer3D.cpp`,
   trigger Refresh Native Addons, confirm no duplicate factory crash and the
   next `Play()` works.

### Android (arm64-v8a)

Status today: addon does not build for Android. None of the v2 audio APIs have
Android backends. Engine itself does not have a current Android target in the
shipped build pipeline for native addons.

1. **[engine] Audio_Android.cpp backend** for the streaming voice API. Use
   **AAudio** (NDK 26+) with one `AAudioStream` per opened video at
   `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`. The data callback drains the addon's
   `mAudioQueue`; `GetPlayedFrames` is read via `AAudioStream_getFramesWritten`
   minus the device's `framesPerBurst` latency (or via `getTimestamp` for the
   accurate clock). Older devices on NDK < 26 fall back to **OpenSL ES**.

2. **[engine] Native addon loading on Android.** `dlopen` of a per-app `.so` from
   `lib/<abi>/` works at runtime, but the addon must be packaged into the APK's
   `lib/` dir. Extend the packager (`ActionManager::InjectNativeAddonSources` +
   the Android-specific build-glue) to copy each addon's compiled `.so` and any
   `copyBinaries` next to the engine `.so` in the APK staging tree. Symbol
   stripping is required to keep the APK small.

3. **[addon] Build for Android via NDK toolchain**:
   ```cmake
   if(ANDROID)
       set(CMAKE_ANDROID_STL_TYPE c++_shared)
       target_compile_definitions(com.polyphase.formats.video PRIVATE PLATFORM_ANDROID=1)
   endif()
   ```
   Use a prebuilt FFmpeg for Android (e.g. `arthenica/ffmpeg-kit` or
   `Khang-NT/ffmpeg-binary-android` arm64-v8a tarball). Place the `.so` files
   under `External/ffmpeg/android/arm64-v8a/` and update CMake to select the
   right subdir from `${ANDROID_ABI}`. Ensure FFmpeg is built with NEON
   (`--enable-asm`) — software H.264 decode is otherwise too slow on mid-range
   phones.

4. **[addon] (optional, recommended) MediaCodec backend** for hardware decode.
   Add `MediaCodecVideoDecoder.cpp` implementing `IVideoDecoder`, gated on
   `PLATFORM_ANDROID`. Use `AMediaCodec` + `AMediaExtractor` from
   `<media/NdkMediaCodec.h>`. Output a hardware-buffer-backed `AImage` and
   convert to RGBA8 once per frame. The factory selects MediaCodec ahead of
   FFmpeg on Android for `.mp4`/`.webm`. FFmpeg remains the fallback for codecs
   the device doesn't natively decode.

5. **[addon] Activity lifecycle hooks**. Pause the async decode worker and
   stream-audio submission when the engine fires the
   `OnPause`/`OnResume`-equivalent app event (existing engine plumbing). Without
   this the worker thread spins on a backgrounded app and drains battery.

6. **[addon] Asset path resolution**. Android assets live inside the APK or
   under `Context.getFilesDir()`. The current `FFmpegVideoDecoder::Open(path)`
   passes the path straight to `avformat_open_input`, which won't traverse APK
   storage. Either (a) require videos to be extracted to external storage at
   first run, or (b) use `AAssetManager` + a custom `AVIOContext` (`avio_alloc_context`)
   that reads from `AAsset_read`. Option (b) is cleaner and avoids the storage
   permission ask.

### Consoles (Switch, PS5, Xbox Series, …)

Status today: most console audio backends are stubs in the engine. Some console
SDKs forbid distributing FFmpeg DLLs at all; on others, LGPL dynamic-linking
isn't available because the OS doesn't expose user-mode `dlopen` semantics. So
the realistic path is one **custom backend per console**, not "FFmpeg
everywhere".

1. **[engine] Per-console streaming audio** in
   `Engine/Source/Audio/<Platform>/Audio_<Platform>.cpp`. Implement the same six
   `AUD_*` functions on top of:
   - **Switch**: `nn::audio::AudioOut` with a per-stream `AudioOutBuffer` chain
     (`nn::audio::AppendAudioOutBuffer`). `samplesPlayed` from
     `nn::audio::GetReleasedAudioOutBuffer` running totals.
   - **PS5**: `sceAudioOut2` streaming output port; framecount from
     `sceAudioOutGetLastOutputTime`.
   - **Xbox Series**: XAudio2 just like desktop Windows — the existing
     `Audio_Windows.cpp` should compile under the GDK with one or two
     `#if XBOX` adjustments to source-voice flags. This is the lowest-effort
     console.

2. **[addon] Per-console video decoder backends**. Add new files under
   `Source/Backends/`, each implementing `IVideoDecoder` and selected via
   `VideoDecoderFactory.cpp` under `#if PLATFORM_<X>`:
   - **Switch**: `nn::mmv::Movie` for H.264/AVC; outputs YUV420 surfaces, convert
     to RGBA8 in a compute shader. (`SwitchMovieDecoder.cpp`)
   - **PS5**: `sceAvPlayer` (handles demux+decode+resample in one API). Audio
     PCM is delivered alongside video frames; route it into the same
     `AsyncMediaPump::mAudioQueue` to keep the A/V sync code path identical.
     (`PS5AvPlayerDecoder.cpp`)
   - **Xbox Series**: FFmpeg static-linked is permitted under GDK NDA terms —
     reuse `FFmpegVideoDecoder` directly, just rebuild for that toolchain.
   - **3DS / Wii / GameCube**: see the dedicated subsection below — these are
     too memory-constrained for FFmpeg software decode and want
     hardware/native-format backends instead.

3. **[engine] Native addon loading**. Most console SDKs do allow shared library
   loading (Switch's `nn::ro`, PS5's `sceKernelLoadStartModule`), but the
   shipped-build path emits a static-registrar (`POLYPHASE_REGISTER_PLUGIN`) and
   links the addon into the game executable. **Use the static path on console**
   — don't bother extending the `dlopen`-style hot reload to those targets;
   shipping builds aren't reloaded anyway.

4. **[addon] Memory pinning / GPU-visible heaps**. Some consoles require texture
   upload sources to come from a specific heap (e.g. PS5 `sceKernelAllocateMainDirectMemory`
   with `SCE_KERNEL_WC_GARLIC` for streaming texture data). The current
   `AsyncMediaPump::VideoFrameSlot::pixels` is a plain `std::vector<uint8_t>`;
   wrap pixel allocation in a platform-specific allocator on those targets to
   avoid an extra copy on `Texture::UpdatePixels`. Defer until a console build
   exists to measure against.

5. **[addon] Codec licensing**. H.264 / H.265 / AAC have separate licensing on
   each console SKU. SDK-provided decoders (#2 above) cover their license inline.
   FFmpeg's H.264 decoder does not — if FFmpeg ends up in a shipped console
   build, the title needs MPEG-LA royalties handled at the publisher level.
   Avoid this by using SDK decoders on console.

### Homebrew / legacy Nintendo (3DS, Wii, GameCube)

These targets share the engine's `libctru` / `libogc` / devkitPro toolchains.
They are *severely* memory-constrained — FFmpeg software decode is a non-starter
for video larger than ~160p — so the realistic plan is **one purpose-built
backend per console**, sized to what each device can actually handle. None are
under NDA, and homebrew prior art exists for all three.

#### Nintendo 3DS (libctru)

Hardware: dual-screen 240×400 + 240×320, 96 MB RAM (Old3DS) / 256 MB (New3DS),
ARM11. Has a dedicated **H.264 hardware decoder** exposed via the `mvd`
service.

1. **[engine] Audio backend** in `Engine/Source/Audio/3DS/Audio_3DS.cpp` using
   **NDSP** (libctru's audio API). 24 channels available; allocate one per
   streaming voice. `ndspChnSetFormat(NDSP_FORMAT_STEREO_PCM16)`,
   `ndspChnWaveBufAdd` for queueing PCM chunks. Track samples played by
   accumulating completed `ndspWaveBuf` lengths in the per-channel `OnDone`
   callback.

2. **[addon] `MvdVideoDecoder.cpp`** using the `mvd` service for hardware
   H.264 decode. Lifecycle:
   - `mvdstdInit(MVDMODE_VIDEOPROCESSING, ...)` once at addon load.
   - Demux H.264 elementary stream from `.mp4`/`.h264` (a tiny stb-style mp4
     demuxer is enough — don't pull in FFmpeg).
   - `mvdstdProcessVideoFrame` per NAL unit; output is YUV420 in linear memory.
   - Convert YUV → RGBA on the GPU via a fragment shader using the existing
     `Y2R` service or a custom citro3d shader.
   Cap to 480×272 or 480×360 — anything larger overruns VRAM. This is the
   maximum target the homebrew Moonshell / VLC-3DS achieve.

3. **[addon] Audio**: 3DS H.264 .mp4 files typically have AAC audio. The 3DS
   has no hardware AAC decoder. Use **FAAD2** (small, MIT-compatible) compiled
   into the addon for AAC → PCM, or pre-encode video audio tracks as raw stereo
   16-bit PCM at 32 kHz to skip decode entirely.

4. **[addon] File I/O**: video files live in RomFS or SD card. Use
   `romfs:/...` paths via libctru's RomFS layer; the addon's
   `FFmpegVideoDecoder::Open(path)` is irrelevant on this backend.

5. **Memory budget**: ~8 MB total for video + audio + scratch on Old3DS, ~24 MB
   on New3DS. The async pump's `mAudioQueue` should be sized down to 2–3
   entries on this platform (vs. 8 on desktop).

#### Wii (libogc)

Hardware: 480p max, 88 MB total RAM (24 MB MEM1 + 64 MB MEM2), PowerPC Broadway.
**No hardware video decoder** for modern codecs. Native video is **Nintendo
THP** (Hollywood JPEG-like format).

1. **[engine] Audio backend** in `Engine/Source/Audio/Wii/Audio_Wii.cpp` using
   **AESND** (libogc) or the lower-level `AUDIO_*` voice API. AESND gives 32
   software voices with mixing on the DSP — allocate one per streaming voice
   and feed PCM via the user callback.

2. **[addon] `ThpVideoDecoder.cpp`** for native `.thp` files. THP is the
   format used by every commercial Wii title that streams video; spec is
   reverse-engineered and stable. Reference implementation: `thp` source in
   the **MPlayer-CE** homebrew project, MIT-compatible. Output is JPEG-ish
   per-frame — decode with libjpeg-turbo (already on devkitPPC), upload as
   RGBA. This is the supported path. Encoder: Nintendo's `THPConv` is
   redistributed in homebrew SDKs, or use ffmpeg with a custom muxer.

3. **[addon] (optional) `TheoraVideoDecoder.cpp`** for `.ogv` files using
   **libtheora** (BSD, builds clean on devkitPPC; prior art:
   *libtheoraplayer* / *Wii Theora Player*). Slower than THP — usable up to
   ~480×272 @ 24fps. Skip H.264 entirely; the CPU can't keep up.

4. **[addon] Audio**: pair THP video with THP-embedded ADPCM audio (decoder is
   ~150 lines of C). For Theora `.ogv`, pair with Vorbis (libvorbis on
   devkitPPC, or just ship pre-decoded PCM).

5. **Memory budget**: keep the async pump on Wii but split allocations between
   MEM1 (locked, fast — for video frames being uploaded to GX) and MEM2
   (audio queue, decoder scratch). The current `std::vector<uint8_t>`
   backing in `AsyncMediaPump::VideoFrameSlot` lands wherever the default
   allocator puts it — wrap in a placement allocator on Wii.

#### GameCube (libogc, also reachable via Dolphin)

Hardware: 480p max, **24 MB main + 16 MB ARAM**, PowerPC Gekko. Same toolchain
as Wii but tighter on memory. ARAM (auxiliary RAM) is DMA-only — not directly
addressable by the CPU — and is the *only* place there's room to buffer
streaming audio.

1. **[engine] Audio backend** in `Engine/Source/Audio/GameCube/Audio_GameCube.cpp`
   using libogc's `AUDIO_*` + `AR_*` (ARAM) APIs. Streaming PCM has to be
   uploaded to ARAM via `ARQ_PostRequest`, then DMA'd back into the audio
   FIFO chunk-by-chunk. This is more involved than Wii's AESND — there is no
   software mixer above it. Prior art: `MPlayer-GC` audio backend.

2. **[addon] `ThpVideoDecoder.cpp`** is the realistic option here too — same
   format, same code as Wii (share via `#if PLATFORM_WII || PLATFORM_GAMECUBE`).
   Cap resolution at **320×240** for GameCube; even THP at 480p exceeds the
   24 MB budget once you account for engine + scene + double-buffered audio.

3. **No FFmpeg**, **no Theora**, **no H.264** on GameCube. Anything outside
   THP plus a couple of frames of look-ahead won't fit. Document `.thp` as
   the only supported extension.

4. **Dolphin emulator note**: Dolphin runs the same GameCube/Wii build — there
   is no separate "Dolphin backend." When testing through Dolphin, expect
   audio latency to be higher than on hardware (Dolphin's DSP-HLE adds ~30 ms),
   which the audio-as-master sync absorbs cleanly. If you set
   Dolphin → Audio → DSP-LLE, latency drops but CPU cost rises. Test on
   real hardware before shipping; Dolphin's emulated AUDIO_*/ARAM timing isn't
   cycle-accurate.

5. **Out of scope for GameCube/Wii/3DS**: the editor doesn't run on these
   platforms (they're shipped-build targets only), so editor preview, hot
   reload, and the property panel for `VideoPlayer3D` don't need any work.
   Only the runtime path matters.

### Suggested order of attack

1. **Linux factory strip** ([engine], ~1 day) — already specified in v2 plan.
2. **Linux PulseAudio streaming** ([engine], ~2-3 days) — biggest user-visible
   gap on Linux.
3. **Xbox Series** ([engine], ~1-2 days) — easiest console; mostly recompile.
4. **Android AAudio + MediaCodec** ([engine] + [addon], ~1-2 weeks) — biggest
   user base for next-platform-after-PC.
5. **Switch / PS5** — only when there's a concrete title shipping; SDK NDAs make
   speculative work expensive.
6. **3DS / Wii / GameCube** — homebrew/legacy targets. Pick one based on which
   is being shipped to first; THP on Wii is the smallest scope (~3-5 days
   including audio), 3DS+MVD is the best-looking result (~2 weeks with AAC),
   GameCube is the worst ROI unless a specific title needs it.

## Out of scope for v2 (Windows)

- 3D spatial audio (stereo 2D only)
- Linux / console streaming audio (engine's mixers on those platforms are stubs;
  see "Next steps for other platforms" above)
- Playback speed != 1.0 with audio enabled (requires on-the-fly resampling)
- Editor content-pipeline video import
- Editor preview panel / thumbnails

## Known edge cases

- **Audio clock startup**: XAudio2's `SamplesPlayed` counter doesn't start
  incrementing until the first buffer actually reaches the output. The player
  holds the video playhead at 0 until the counter moves off 0 the first time;
  this typically takes 1–2 ticks and is imperceptible.
- **Seek under async**: A seek request is posted to the decode worker. Frames
  already decoded before the seek are flushed. The next displayed frame is the
  nearest keyframe at or after the requested time.
- **Playback speed + audio**: If you call `SetPlaybackSpeed(≠ 1.0)` with audio
  enabled, the video will still follow the audio clock at 1.0. Disable audio
  first (`SetAudioEnabled(false)`) to use playback speed.
