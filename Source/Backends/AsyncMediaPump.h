#pragma once

#include "Backends/IVideoDecoder.h"
#include "System/System.h"
#include "System/SystemTypes.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace VideoPlayerAddon
{

// Runs an IVideoDecoder on a worker thread and hands decoded frames off to the
// game thread via a drop-oldest single-slot queue (video) and a bounded FIFO (audio).
//
// Threading contract:
//   - Start/Stop are called from the game thread only.
//   - TryPopLatestVideo / TryPopAudio / RequestSeek are called from the game thread.
//   - The worker thread owns the IVideoDecoder; the game thread never touches it after Start.
//   - HasError / IsEndOfStream are atomic reads safe from any thread.
//
// Drop-oldest policy: if the worker decodes faster than the consumer pops, older frames
// are overwritten. This is the desired behavior for video under CPU pressure — prefer
// the freshest frame over a stale backlog.
class AsyncMediaPump
{
public:
    struct VideoFrameSlot
    {
        std::vector<uint8_t> pixels;       // RGBA8, width * height * 4 bytes
        double ptsSeconds = 0.0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;
    };

    struct AudioChunk
    {
        std::vector<uint8_t> samples;      // int16 stereo interleaved at engine rate
        uint32_t sampleCount = 0;          // frames (samples per channel)
        double ptsSeconds = 0.0;
    };

    AsyncMediaPump();
    ~AsyncMediaPump();

    AsyncMediaPump(const AsyncMediaPump&) = delete;
    AsyncMediaPump& operator=(const AsyncMediaPump&) = delete;

    // Takes ownership of the decoder. Worker thread begins producing frames.
    // Returns false on failure (decoder already started, thread creation failed).
    bool Start(std::unique_ptr<IVideoDecoder> decoder);

    // Signals the worker to exit and joins the thread. Safe to call multiple times.
    void Stop();

    // Drains the current video slot. Returns true and moves the frame into `out` if
    // a new frame is available. Clears the slot (marking it consumed).
    bool TryPopLatestVideo(VideoFrameSlot& out);

    // Non-destructive peek — returns the PTS of the NEXT (oldest) pending video frame
    // in the queue without consuming it.
    bool PeekLatestVideoPts(double& outPtsSeconds);

    // Pop every queued frame whose PTS is <= `maxPtsSeconds`, returning the most recent
    // of those via `out`. Frames older than `out` are discarded (they'd display in the
    // past, which flashes stale content). If no frame is due yet, returns false without
    // modifying the queue.
    //
    // This is the preferred drain method under audio-master sync: on ticks where the
    // playhead jumps (e.g. after a brief audio-clock hiccup), we catch up by showing
    // the latest in-time frame instead of displaying every intermediate one.
    bool TryPopDueVideoFrame(double maxPtsSeconds, VideoFrameSlot& out);

    // Drains one audio chunk in FIFO order. Returns false if no chunk is queued.
    // (Unused until the audio-decode workstream lands; exists so VideoPlayer3D can be
    // updated in a single pass without revisiting the interface.)
    bool TryPopAudioChunk(AudioChunk& out);

    // Schedules a seek on the worker. The worker flushes its decoder and discards any
    // in-flight frames. The next TryPopLatestVideo will return the first frame at-or-after
    // `seconds` once the worker catches up.
    void RequestSeek(double seconds);

    bool HasError() const       { return mError.load(std::memory_order_acquire); }
    bool IsEndOfStream() const  { return mEndOfStream.load(std::memory_order_acquire); }
    bool IsRunning() const      { return mRunning.load(std::memory_order_acquire); }

    // Safe to call any time — the decoder is immutable after Start (only worker mutates
    // decode state; descriptor queries just read fields set during Open).
    const IVideoDecoder* GetDecoder() const { return mDecoder.get(); }

private:
    static ThreadFuncRet ThreadEntry(void* arg);
    void WorkerLoop();
    void HandleSeek(double seconds);
    bool ProduceOneVideoFrame(); // returns false on error or EOS (flags set)

    std::unique_ptr<IVideoDecoder> mDecoder;

    ThreadObject* mThread = nullptr;
    MutexObject*  mMutex  = nullptr;

    std::atomic<bool> mRunning { false };
    std::atomic<bool> mError   { false };
    std::atomic<bool> mEndOfStream { false };
    // -1 sentinel = no pending seek. atomic<double> isn't lock-free on every platform
    // so we use int64 holding the double bits (still acceptable for a seek request).
    std::atomic<int64_t> mPendingSeekBits { kNoSeek };
    static constexpr int64_t kNoSeek = 0x7FFFFFFFFFFFFFFFLL;

    // Guarded by mMutex:
    // Small FIFO of ready-to-present video frames. Worker pushes to the back; consumer
    // pops from the front when the frame's pts is due. ~4 frames at 30fps = ~133ms of
    // lookahead — enough to absorb decoder jitter without exceeding the pump's desired
    // latency envelope.
    std::deque<VideoFrameSlot> mVideoQueue;
    static constexpr size_t kVideoQueueMax = 4;
    std::deque<AudioChunk> mAudioQueue;
    static constexpr size_t kAudioQueueMax = 8;
};

} // namespace VideoPlayerAddon
