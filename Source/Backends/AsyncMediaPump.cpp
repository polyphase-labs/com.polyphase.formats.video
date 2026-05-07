#include "Backends/AsyncMediaPump.h"

#include "Log.h"

#include <cstring>

namespace VideoPlayerAddon
{

AsyncMediaPump::AsyncMediaPump() = default;

AsyncMediaPump::~AsyncMediaPump()
{
    Stop();
}

bool AsyncMediaPump::Start(std::unique_ptr<IVideoDecoder> decoder)
{
    if (mRunning.load() || mThread != nullptr)
    {
        LogWarning("AsyncMediaPump::Start called while already running");
        return false;
    }
    if (decoder == nullptr)
    {
        return false;
    }

    mDecoder = std::move(decoder);

    mMutex = SYS_CreateMutex();
    mError.store(false, std::memory_order_release);
    mEndOfStream.store(false, std::memory_order_release);
    mPendingSeekBits.store(kNoSeek, std::memory_order_release);
    mVideoQueue.clear();
    mAudioQueue.clear();

    mRunning.store(true, std::memory_order_release);
    mThread = SYS_CreateThread(&AsyncMediaPump::ThreadEntry, this);
    if (mThread == nullptr)
    {
        LogError("AsyncMediaPump::Start failed to create worker thread");
        mRunning.store(false, std::memory_order_release);
        SYS_DestroyMutex(mMutex);
        mMutex = nullptr;
        mDecoder.reset();
        return false;
    }

    return true;
}

void AsyncMediaPump::Stop()
{
    if (mThread != nullptr)
    {
        mRunning.store(false, std::memory_order_release);
        SYS_JoinThread(mThread);
        SYS_DestroyThread(mThread);
        mThread = nullptr;
    }

    if (mMutex != nullptr)
    {
        SYS_DestroyMutex(mMutex);
        mMutex = nullptr;
    }

    mDecoder.reset();
    mVideoQueue.clear();
    mAudioQueue.clear();
    mError.store(false, std::memory_order_release);
    mEndOfStream.store(false, std::memory_order_release);
    mPendingSeekBits.store(kNoSeek, std::memory_order_release);
}

void AsyncMediaPump::RequestSeek(double seconds)
{
    int64_t bits;
    std::memcpy(&bits, &seconds, sizeof(bits));
    mPendingSeekBits.store(bits, std::memory_order_release);
    // Clear EOS so the worker resumes decoding after the seek.
    mEndOfStream.store(false, std::memory_order_release);
}

bool AsyncMediaPump::PeekLatestVideoPts(double& outPtsSeconds)
{
    if (mMutex == nullptr) return false;
    SCOPED_LOCK(mMutex);
    if (mVideoQueue.empty()) return false;
    outPtsSeconds = mVideoQueue.front().ptsSeconds;
    return true;
}

bool AsyncMediaPump::TryPopLatestVideo(VideoFrameSlot& out)
{
    if (mMutex == nullptr) return false;

    SCOPED_LOCK(mMutex);
    if (mVideoQueue.empty()) return false;

    // Pop the oldest queued frame.
    out = std::move(mVideoQueue.front());
    mVideoQueue.pop_front();
    return true;
}

bool AsyncMediaPump::TryPopDueVideoFrame(double maxPtsSeconds, VideoFrameSlot& out)
{
    if (mMutex == nullptr) return false;

    SCOPED_LOCK(mMutex);
    if (mVideoQueue.empty()) return false;
    if (mVideoQueue.front().ptsSeconds > maxPtsSeconds) return false;

    // Consume frames from the front while they're "due" (pts <= maxPts). Keep only the
    // most recent one; any earlier ones would display in the past and cause visible
    // stutter. This is the catch-up path: a brief pause in the clock that skipped us
    // past several frames gets resolved by a single present of the latest in-time frame.
    VideoFrameSlot latest = std::move(mVideoQueue.front());
    mVideoQueue.pop_front();
    while (!mVideoQueue.empty() && mVideoQueue.front().ptsSeconds <= maxPtsSeconds)
    {
        // Older frame becomes garbage; take the newer one.
        latest = std::move(mVideoQueue.front());
        mVideoQueue.pop_front();
    }
    out = std::move(latest);
    return true;
}

bool AsyncMediaPump::TryPopAudioChunk(AudioChunk& out)
{
    if (mMutex == nullptr) return false;

    SCOPED_LOCK(mMutex);
    if (mAudioQueue.empty()) return false;

    out = std::move(mAudioQueue.front());
    mAudioQueue.pop_front();
    return true;
}

ThreadFuncRet AsyncMediaPump::ThreadEntry(void* arg)
{
    AsyncMediaPump* self = static_cast<AsyncMediaPump*>(arg);
    self->WorkerLoop();
    THREAD_RETURN();
}

void AsyncMediaPump::HandleSeek(double seconds)
{
    if (mDecoder == nullptr) return;

    if (!mDecoder->Seek(seconds))
    {
        LogWarning("AsyncMediaPump: seek to %.3f failed", seconds);
    }

    // Drop any stale frames queued from before the seek.
    SCOPED_LOCK(mMutex);
    mVideoQueue.clear();
    mAudioQueue.clear();
}

bool AsyncMediaPump::ProduceOneVideoFrame()
{
    if (mDecoder == nullptr) return false;

    DecodedFrame frame{};
    if (!mDecoder->DecodeNextFrame(frame))
    {
        mError.store(true, std::memory_order_release);
        return false;
    }

    if (frame.endOfStream)
    {
        mEndOfStream.store(true, std::memory_order_release);
        return false;
    }

    if (frame.pixels == nullptr || frame.byteSize == 0)
    {
        // Non-fatal: codec may need more packets. The caller's loop will try again.
        return true;
    }

    // Push onto the back of the small FIFO. Copies are unavoidable — the decoder
    // will overwrite its internal buffer on the next call. If the FIFO is full we
    // drop the oldest entry to make room; under normal pacing this won't trigger
    // because consumer drain matches production rate.
    const VideoFrameDesc desc = mDecoder->GetFrameDesc();
    VideoFrameSlot slot;
    slot.pixels.assign(frame.pixels, frame.pixels + frame.byteSize);
    slot.ptsSeconds = frame.ptsSeconds;
    slot.width      = desc.width;
    slot.height     = desc.height;
    slot.valid      = true;

    SCOPED_LOCK(mMutex);
    while (mVideoQueue.size() >= kVideoQueueMax)
    {
        mVideoQueue.pop_front();
    }
    mVideoQueue.push_back(std::move(slot));
    return true;
}

void AsyncMediaPump::WorkerLoop()
{
    while (mRunning.load(std::memory_order_acquire))
    {
        // Service a pending seek first so decoded frames after this point are post-seek.
        const int64_t seekBits = mPendingSeekBits.exchange(kNoSeek, std::memory_order_acq_rel);
        if (seekBits != kNoSeek)
        {
            double seekSeconds = 0.0;
            std::memcpy(&seekSeconds, &seekBits, sizeof(seekSeconds));
            HandleSeek(seekSeconds);
        }

        // If we already hit an error or EOS, idle until told to seek or stop.
        if (mError.load(std::memory_order_acquire) || mEndOfStream.load(std::memory_order_acquire))
        {
            SYS_Sleep(5);
            continue;
        }

        // --- Audio: produce ahead of video so the audio stream voice never starves ---
        // Poll the audio queue depth; if there's room, pull chunks until the decoder says
        // NeedsMoreInput (in which case pumping video next implicitly pumps the demuxer).
        bool audioQueueFull = false;
        {
            SCOPED_LOCK(mMutex);
            audioQueueFull = mAudioQueue.size() >= kAudioQueueMax;
        }
        if (!audioQueueFull && mDecoder != nullptr && mDecoder->HasAudio())
        {
            DecodedAudio audio{};
            const AudioDecodeResult res = mDecoder->DecodeNextAudio(audio);
            if (res == AudioDecodeResult::Produced && audio.samples != nullptr && audio.byteSize > 0)
            {
                AudioChunk chunk;
                chunk.samples.assign(audio.samples, audio.samples + audio.byteSize);
                chunk.sampleCount = audio.sampleCount;
                chunk.ptsSeconds  = audio.ptsSeconds;

                SCOPED_LOCK(mMutex);
                // Drop-oldest under pressure (bounded FIFO; caller should be draining).
                while (mAudioQueue.size() >= kAudioQueueMax) mAudioQueue.pop_front();
                mAudioQueue.push_back(std::move(chunk));
            }
            else if (res == AudioDecodeResult::Error)
            {
                mError.store(true, std::memory_order_release);
                continue;
            }
            // NeedsMoreInput / EndOfStream — fall through to video production below.
        }

        // --- Video ---
        bool videoQueueFull = false;
        {
            SCOPED_LOCK(mMutex);
            videoQueueFull = mVideoQueue.size() >= kVideoQueueMax;
        }
        if (videoQueueFull)
        {
            SYS_Sleep(1);
            continue;
        }

        if (!ProduceOneVideoFrame())
        {
            // Error or EOS flag was set; loop head will park us until shutdown / seek.
            continue;
        }
    }
}

} // namespace VideoPlayerAddon
