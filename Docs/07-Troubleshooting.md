# 07 — Troubleshooting

Symptoms → likely causes → fixes. If your issue isn't here, check `.dev/` in the addon for ongoing investigations, or open an issue.

## Editor / PC playback

### "Decoder open failed for VideoClip 'X'"

Causes:
- FFmpeg DLLs not present. On Windows, check that `External/ffmpeg/bin/*.dll` exists and that they got copied next to `com.polyphase.formats.video.dll` after build. CMake should log `VideoPlayer: found FFmpeg at ...` on a clean build.
- Source `.mp4` is corrupt. Try opening it in VLC.
- Codec hint missing. The factory falls through to the null decoder if it can't route by extension AND the bytes don't carry a known magic. Re-import with the source extension intact.

### Black quad, no error

Likely causes:
- Quad has no texture set, or you set the texture before `OnReady` fired. Connect to `OnReady` and `SetTexture` in the callback.
- Quad's UV is fully outside the texture's visible area (3DS PoT-rounding). Call `quad:SetUVScale(player:GetTexture():GetUVMax())` after `OnReady`.
- Quad has the wrong dimensions or is off-screen. Set explicit `SetDimensions(width, height)` and verify the quad's transform.

### Audio doesn't play in editor

- `SetAudioEnabled(false)` was called, or the engine's audio backend isn't initialized for your platform. Try a sound-effect playback in the same scene to confirm audio in general is working.
- The clip has no audio track. Re-import and check the inspector's source-info section.

## Console packaging / playback

### Build fails: "VideoClip cook failed for 'X'"

Causes:
- Source `.mp4` has an unusual codec FFmpeg can't decode in your install. Test Cook in the editor first to confirm the cook succeeds before triggering a build.
- Cook params are invalid for the chosen format (e.g. odd dimensions for N3MV's H.264 path; you'll see a specific error from x264 in the cook log).
- For THP / N3MV: the cook step in `SaveStream` failed silently because `mSourceData` was empty. Re-run Test Cook so a sidecar exists, then re-package.

### Build succeeds but on hardware video doesn't play

In order of frequency:

1. **The runtime can't find the cooked bytes.** Earlier versions of the addon shipped the dev-machine sidecar path baked into the asset; on hardware that path didn't resolve. Fixed: `SaveStream` now embeds the sidecar bytes inline into the packaged `.oct`. If you see this on a recent addon version, check the editor's packaging log for "hydrated sidecar bytes (N) inline for hardware packaging" — that line means the fix is active.
2. **Cook never ran.** The asset was saved without a `Test Cook for Platform`, so the packaged `.oct` has source bytes but no cooked payload, and the runtime decoder doesn't know what to do with `.mp4` magic bytes on a console. Re-run Test Cook.
3. **Wrong format for the platform.** E.g. shipped a THP-cooked `.oct` to a 3DS build. The cook normally falls through to PCV1 for mismatched format/platform combos, but if you ran an older addon version, double-check the cook log for the format actually emitted.

### Wii / GameCube: video plays but audio is wrong

Common audio symptoms and what they mean:

| Symptom | Cause | Fix |
|---|---|---|
| Static / pops at regular intervals (~50 Hz) | Audio queue draining between main-thread ticks | Should be fixed in current addon — the engine's submit pipeline uses a 50 ms high-water-mark policy that keeps ~100 ms in ASND's ring. If you still hear this, the main thread is running far below 30 Hz; profile rendering / decoding load. |
| Pitched up / sounds like alien speech but recognizable | Decoded PCM sample byte order wrong | Should be fixed via `WriteSampleLE` in `ThpVideoDecoder.cpp`. PowerPC is BE and ASND expects LE; `int16_t` writes need explicit endian fixup. Check `ThpVideoDecoder.cpp` for `WriteSampleLE` use at the interleave step. |
| Thin / mostly high-end / mildly distorted | THP DSP-ADPCM encoder using Nintendo's generic fixed coefficient table | Known limitation. **Workaround: use PCV1 instead of THP.** The proper fix (per-clip LPC analysis) is implemented but disabled — see `.dev/thp_lpc_encoder_bug.md`. |
| Shrieking / Nyquist-rate noise / "modem tones" | THP LPC encoder enabled with bad coefficients | The LPC path is disabled in current code; if you see this, someone re-enabled `ComputeClipCoefs`'s LPC pipeline. Revert to the `kFallbackCoefs` `memcpy` until the bug is fixed. |
| Video plays but audio is silent | Cook step missed audio (FFmpeg's `-vn` audio extraction failed); or `mAudioStreamId` is 0 | Re-run Test Cook and watch the editor log for "no audio" warnings. Verify the source has an audio track at all. |

### 3DS: colors look wrong (red / magenta tint)

The 3DS GPU stores RGBA8 textures in **ABGR** byte order in memory. The runtime upload step swaps RGBA → ABGR per pixel. If colors are red-tinted, that swap got skipped — check `Graphics_C3D.cpp::GFX_UpdateTextureResourcePixels` for the channel-swap loop.

### 3DS: video is upside-down

Set `GX_TRANSFER_FLIP_VERT(1)` in the `GX_DisplayTransfer` call that uploads the linear-buffer RGBA frame to the tiled GPU texture. This is in the engine's 3DS graphics backend, not the addon. If you see upside-down video on 3DS in current code, it's an engine regression — re-check the flag.

### 3DS: video shows in only the top-left corner

The texture is power-of-two-sized but the visible area is the original cook dimensions. Apply UV cropping:

```lua
quad:SetUVScale(player:GetTexture():GetUVMax())
```

Or use the `Quad` inspector's "Crop Texture" / "Match Texture Size" buttons. See the demo `Scripts/video_demo.lua`.

## Sync / timing

### Video runs faster than audio (or audio leads video)

The player uses the audio clock as master when audio is enabled. If you see drift:
- Audio playback rate ≠ cooked sample rate (some platform-specific bug). Compare `SetVoice` pitch parameter against the cook's `audioSampleRate` field.
- Audio buffer is dropping samples (engine returns short writes). Should not happen with current submit pipeline.

### Loop doesn't fire

- `SetLoop(true)` not set. Check.
- The audio clock isn't reaching the clip duration. The engine's streaming voice has to retire its last queued buffer before `mPlayedFrames` reaches `numSamples`. With the current submit policy this happens reliably; older versions had a bug where the last 1-2 buffers stayed in the slot indefinitely (libogc2's `ASND_TestPointer` is sticky on the final drained buffer). Status-based retire fix is in place — `ASND_StatusVoice() != SND_WORKING` retires all active buffers.
- The clip's `numSamples` field doesn't match the actual encoded data. Re-cook.

### Pause / Seek behavior is weird

- `Pause()` doesn't reset the playhead. To go back to start, call `Stop()` (which seeks to 0) or `Seek(0)`.
- After `Seek(t)`, the next *displayed* frame is the nearest keyframe at or after `t` — for non-keyframe formats like MJPEG/PCV1 every frame is a keyframe, but for H.264 / N3MV there's a coarser grain.
- `Seek` while paused: the seek is pending until the next `Play()`.

## Misc

### Can I change cook params at runtime?

No. The cooked bytes are baked into the `.oct` at packaging time. The runtime decoder reads whatever was cooked; there's no "re-cook on the fly" path.

### Why do I get a "VideoPlayer3D: looped" log line every Tick?

You don't — the engine logs that exactly once per loop event. If you're seeing it every Tick, the loop trigger is firing every frame, which means the audio clock is somehow at exactly the duration every frame. Check that the duration field on the clip matches the cooked sample count.

### Where does the cook leave its temp files?

`<projectDir>/Intermediate/VideoClipCook/` — extracted JPEG frames, raw PCM audio, intermediate ADPCM. `gitignore`d. The cook overwrites this directory each run.

### The cook is slow

Most of the time goes to FFmpeg's frame extraction + JPEG encoding. For a 30 s clip at 30 fps this is ~900 JPEG encodes. Performance varies with source resolution and your machine. If you're iterating, drop to Tiny preset for the dev cycle.

## Still stuck?

- Check the editor's output panel during cook + play for warnings.
- Read `.dev/thp_lpc_encoder_bug.md` if the symptom is THP-audio-related.
- Read the addon's `MEMORY.md` (in the engine's `.claude/` memory) for documented gotchas — there are entries on libogc2 ASND lifecycle, runtime PCM endianness, addon hot-reload behavior, etc.
- File an issue with: source clip metadata (duration, codec, audio config), cook params used, target platform, the editor cook log, and what you hear / see vs what you expect.
