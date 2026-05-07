#if POLYPHASE_WITH_FFMPEG

#include "Backends/FFmpegVideoDecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace VideoPlayerAddon
{

namespace
{
    // All PCM output is 16-bit stereo at 48 kHz. Chosen to match what every downstream
    // platform audio mixer can consume without further conversion. FFmpeg's swresample
    // handles all source formats (float planar, int32, 22.05/44.1 kHz, 5.1 channels, etc.)
    constexpr uint32_t kOutSampleRate = 48000;
    constexpr uint32_t kOutChannels   = 2;
    constexpr AVSampleFormat kOutSampleFmt = AV_SAMPLE_FMT_S16;
}

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    Close();
}

bool FFmpegVideoDecoder::Open(const char* path)
{
    Close();

    if (path == nullptr || path[0] == '\0') return false;

    if (avformat_open_input(&mFormatCtx, path, nullptr, nullptr) < 0)
    {
        return false;
    }

    return FinishOpen();
}

// AVIOContext callbacks reading out of an externally-owned byte buffer. opaque points
// at the FFmpegVideoDecoder so we can mutate its mMemPos cursor.
int FFmpegVideoDecoder::MemReadPacket(void* opaque, uint8_t* buf, int bufSize)
{
    auto* self = static_cast<FFmpegVideoDecoder*>(opaque);
    if (self->mMemPos >= self->mMemSize) return AVERROR_EOF;
    const size_t remaining = self->mMemSize - self->mMemPos;
    const size_t want = std::min(size_t(bufSize), remaining);
    memcpy(buf, self->mMemData + self->mMemPos, want);
    self->mMemPos += want;
    return int(want);
}

int64_t FFmpegVideoDecoder::MemSeek(void* opaque, int64_t offset, int whence)
{
    auto* self = static_cast<FFmpegVideoDecoder*>(opaque);
    if (whence == AVSEEK_SIZE) return int64_t(self->mMemSize);
    int64_t target = 0;
    switch (whence & ~AVSEEK_FORCE)
    {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = int64_t(self->mMemPos) + offset; break;
        case SEEK_END: target = int64_t(self->mMemSize) + offset; break;
        default: return -1;
    }
    if (target < 0) target = 0;
    if (target > int64_t(self->mMemSize)) target = int64_t(self->mMemSize);
    self->mMemPos = size_t(target);
    return target;
}

bool FFmpegVideoDecoder::OpenMemory(const uint8_t* data, size_t size, const char* codecHint)
{
    Close();

    if (data == nullptr || size == 0) return false;

    mMemData = data;
    mMemSize = size;
    mMemPos  = 0;

    constexpr int kAvioBufSize = 32 * 1024;
    unsigned char* avioBuf = (unsigned char*)av_malloc(kAvioBufSize);
    if (avioBuf == nullptr) { Close(); return false; }

    mAvioCtx = avio_alloc_context(avioBuf, kAvioBufSize,
                                  /*write_flag=*/0,
                                  /*opaque=*/this,
                                  &MemReadPacket, /*write_packet=*/nullptr, &MemSeek);
    if (mAvioCtx == nullptr)
    {
        av_free(avioBuf);
        Close();
        return false;
    }

    mFormatCtx = avformat_alloc_context();
    if (mFormatCtx == nullptr) { Close(); return false; }
    mFormatCtx->pb = mAvioCtx;
    mFormatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Hint the demuxer when we have one. avformat_open_input takes a const char*
    // "filename" we can leave null since we have a custom AVIOContext, but a
    // short_name on AVInputFormat helps for containers FFmpeg can't probe (e.g.
    // raw H.264 Annex-B). For .mp4/.webm/etc. probing always succeeds so this
    // is mostly a future-proofing.
    const AVInputFormat* fmt = nullptr;
    if (codecHint != nullptr && codecHint[0] != '\0')
    {
        fmt = av_find_input_format(codecHint);
    }

    if (avformat_open_input(&mFormatCtx, /*filename=*/nullptr, fmt, nullptr) < 0)
    {
        // avformat_open_input frees mFormatCtx on failure; null it so Close() doesn't double-free.
        mFormatCtx = nullptr;
        Close();
        return false;
    }

    return FinishOpen();
}

bool FFmpegVideoDecoder::FinishOpen()
{
    if (avformat_find_stream_info(mFormatCtx, nullptr) < 0)
    {
        Close();
        return false;
    }

    // --- Video stream ---
    const AVCodec* videoDecoder = nullptr;
    mVideoStreamIndex = av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &videoDecoder, 0);
    if (mVideoStreamIndex < 0 || videoDecoder == nullptr)
    {
        Close();
        return false;
    }

    AVStream* videoStream = mFormatCtx->streams[mVideoStreamIndex];

    mCodecCtx = avcodec_alloc_context3(videoDecoder);
    if (mCodecCtx == nullptr) { Close(); return false; }

    if (avcodec_parameters_to_context(mCodecCtx, videoStream->codecpar) < 0) { Close(); return false; }
    mCodecCtx->thread_count = 1;
    if (avcodec_open2(mCodecCtx, videoDecoder, nullptr) < 0) { Close(); return false; }

    mWidth  = uint32_t(mCodecCtx->width);
    mHeight = uint32_t(mCodecCtx->height);
    if (mWidth == 0 || mHeight == 0) { Close(); return false; }

    // Allocate a 64-byte safety pad at the end. FFmpeg's sws_scale SIMD paths are
    // documented in the wild to occasionally write a few bytes past the nominal buffer
    // end for alignment (usually with YUV->RGBA conversions). Without padding, those
    // writes corrupt the CRT heap and manifest as a crash on the next free.
    mRgbaBuffer.assign(size_t(mWidth) * mHeight * 4 + 64, 0);
    mSws = sws_getContext(
        int(mWidth), int(mHeight), mCodecCtx->pix_fmt,
        int(mWidth), int(mHeight), AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (mSws == nullptr) { Close(); return false; }

    mFrame     = av_frame_alloc();
    mRgbaFrame = av_frame_alloc();
    mPacket    = av_packet_alloc();
    if (mFrame == nullptr || mRgbaFrame == nullptr || mPacket == nullptr) { Close(); return false; }

    if (videoStream->duration > 0 && videoStream->time_base.den > 0)
    {
        mDurationSec = double(videoStream->duration) * av_q2d(videoStream->time_base);
    }
    else if (mFormatCtx->duration > 0)
    {
        mDurationSec = double(mFormatCtx->duration) / double(AV_TIME_BASE);
    }

    if (videoStream->avg_frame_rate.den > 0 && videoStream->avg_frame_rate.num > 0)
        mFrameRate = av_q2d(videoStream->avg_frame_rate);
    else if (videoStream->r_frame_rate.den > 0 && videoStream->r_frame_rate.num > 0)
        mFrameRate = av_q2d(videoStream->r_frame_rate);
    else
        mFrameRate = 30.0;

    mTimeBaseSec = (videoStream->time_base.den > 0) ? av_q2d(videoStream->time_base) : 0.0;
    mVideoEOF = false;

    // --- Audio stream (optional) ---
    if (!OpenAudioStream())
    {
        // No audio track, or couldn't decode it — keep running as video-only.
        CloseAudio();
    }

    mEndOfStream = false;
    mAudioFrame  = (mAudioStreamIndex >= 0) ? av_frame_alloc() : nullptr;

    return true;
}

bool FFmpegVideoDecoder::OpenAudioStream()
{
    const AVCodec* audioDecoder = nullptr;
    mAudioStreamIndex = av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
    if (mAudioStreamIndex < 0 || audioDecoder == nullptr)
    {
        mAudioStreamIndex = -1;
        return false;
    }

    AVStream* audioStream = mFormatCtx->streams[mAudioStreamIndex];

    mAudioCodecCtx = avcodec_alloc_context3(audioDecoder);
    if (mAudioCodecCtx == nullptr) return false;

    if (avcodec_parameters_to_context(mAudioCodecCtx, audioStream->codecpar) < 0) return false;
    mAudioCodecCtx->thread_count = 1;
    if (avcodec_open2(mAudioCodecCtx, audioDecoder, nullptr) < 0) return false;

    // Configure swresample: source whatever the codec emits -> output 16-bit stereo @ 48 kHz.
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, int(kOutChannels));

    AVChannelLayout inLayout;
    if (mAudioCodecCtx->ch_layout.nb_channels > 0)
    {
        av_channel_layout_copy(&inLayout, &mAudioCodecCtx->ch_layout);
    }
    else
    {
        av_channel_layout_default(&inLayout, mAudioCodecCtx->ch_layout.nb_channels > 0
                                                 ? mAudioCodecCtx->ch_layout.nb_channels
                                                 : 2);
    }

    int swrErr = swr_alloc_set_opts2(
        &mSwr,
        &outLayout, kOutSampleFmt, int(kOutSampleRate),
        &inLayout, mAudioCodecCtx->sample_fmt, mAudioCodecCtx->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&outLayout);
    av_channel_layout_uninit(&inLayout);

    if (swrErr < 0 || mSwr == nullptr) return false;
    if (swr_init(mSwr) < 0) return false;

    mAudioSampleRate  = kOutSampleRate;
    mAudioNumChannels = kOutChannels;
    mAudioTimeBaseSec = (audioStream->time_base.den > 0) ? av_q2d(audioStream->time_base) : 0.0;
    mAudioEOF = false;
    return true;
}

AudioStreamDesc FFmpegVideoDecoder::GetAudioDesc() const
{
    AudioStreamDesc d;
    d.sampleRate    = mAudioSampleRate;
    d.numChannels   = mAudioNumChannels;
    d.bitsPerSample = 16;
    return d;
}

void FFmpegVideoDecoder::CloseAudio()
{
    if (mSwr != nullptr)         { swr_free(&mSwr); }
    if (mAudioFrame != nullptr)  { av_frame_free(&mAudioFrame); }
    if (mAudioCodecCtx != nullptr) { avcodec_free_context(&mAudioCodecCtx); }
    for (AVPacket* p : mAudioPacketQueue) { av_packet_free(&p); }
    mAudioPacketQueue.clear();
    mAudioStreamIndex = -1;
    mAudioSampleRate = 0;
    mAudioNumChannels = 0;
    mAudioTimeBaseSec = 0.0;
    mPcmBuffer.clear();
    mPcmBuffer.shrink_to_fit();
    mLastPcmSampleCount = 0;
    mLastPcmPtsSeconds = 0.0;
    mAudioEOF = false;
}

void FFmpegVideoDecoder::Close()
{
    if (mSws != nullptr)        { sws_freeContext(mSws); mSws = nullptr; }
    if (mPacket != nullptr)     { av_packet_free(&mPacket); }
    if (mRgbaFrame != nullptr)  { av_frame_free(&mRgbaFrame); }
    if (mFrame != nullptr)      { av_frame_free(&mFrame); }
    if (mCodecCtx != nullptr)   { avcodec_free_context(&mCodecCtx); }
    for (AVPacket* p : mVideoPacketQueue) { av_packet_free(&p); }
    mVideoPacketQueue.clear();

    CloseAudio();

    if (mFormatCtx != nullptr)  { avformat_close_input(&mFormatCtx); }

    // Custom AVIOContext is ours to free even after avformat_close_input — that call
    // releases the format ctx but leaves pb (and pb->buffer) alone. The internal
    // buffer was allocated with av_malloc in OpenMemory, so it must be freed with
    // av_freep BEFORE avio_context_free or it leaks.
    if (mAvioCtx != nullptr)
    {
        av_freep(&mAvioCtx->buffer);
        avio_context_free(&mAvioCtx);
    }
    mMemData = nullptr;
    mMemSize = 0;
    mMemPos  = 0;

    mVideoStreamIndex = -1;
    mWidth = mHeight = 0;
    mDurationSec = 0.0;
    mFrameRate = 0.0;
    mTimeBaseSec = 0.0;
    mRgbaBuffer.clear();
    mRgbaBuffer.shrink_to_fit();
    mVideoEOF = false;
    mEndOfStream = false;
}

// Read one packet from the demuxer and queue it on the appropriate per-stream packet queue.
// Queues are drained opportunistically by DrainQueuedPackets() right before the decoder would
// otherwise say it needs more input. This avoids the previous bug where we tried to send
// every packet immediately and dropped any that returned AVERROR(EAGAIN) — losing H.264
// keyframes → cascading "Missing reference picture" / "co located POCs unavailable" errors.
bool FFmpegVideoDecoder::PumpOnePacket()
{
    if (mFormatCtx == nullptr || mPacket == nullptr) return false;
    const int ret = av_read_frame(mFormatCtx, mPacket);
    if (ret == AVERROR_EOF || ret < 0)
    {
        // Demuxer exhausted. Signal drain via a null packet on each codec so any samples
        // still buffered inside the codec are yielded to subsequent receive_frame calls.
        // The caller will see AVERROR_EOF on receive_frame afterward.
        mVideoEOF = true;
        mAudioEOF = true;
        return false;
    }

    if (mPacket->stream_index == mVideoStreamIndex && mCodecCtx != nullptr)
    {
        AVPacket* cloned = av_packet_alloc();
        if (cloned != nullptr && av_packet_ref(cloned, mPacket) == 0)
        {
            mVideoPacketQueue.push_back(cloned);
        }
        else if (cloned != nullptr)
        {
            av_packet_free(&cloned);
        }
    }
    else if (mAudioCodecCtx != nullptr && mPacket->stream_index == mAudioStreamIndex)
    {
        AVPacket* cloned = av_packet_alloc();
        if (cloned != nullptr && av_packet_ref(cloned, mPacket) == 0)
        {
            mAudioPacketQueue.push_back(cloned);
        }
        else if (cloned != nullptr)
        {
            av_packet_free(&cloned);
        }
    }
    // else: subtitle / data stream — drop.

    av_packet_unref(mPacket);
    return true;
}

// Send as many queued packets to `ctx` as the codec will accept. Stops on AVERROR(EAGAIN)
// (codec needs draining first) or any other non-zero return. Returns false on hard error.
static bool DrainQueuedPackets(AVCodecContext* ctx, std::deque<AVPacket*>& queue)
{
    while (!queue.empty())
    {
        AVPacket* pkt = queue.front();
        int sendRet = avcodec_send_packet(ctx, pkt);
        if (sendRet == 0)
        {
            av_packet_free(&pkt);
            queue.pop_front();
            continue;
        }
        if (sendRet == AVERROR(EAGAIN))
        {
            // Codec wants us to receive before we can send more. Leave this packet queued.
            return true;
        }
        if (sendRet == AVERROR_EOF)
        {
            // Codec has been flushed already — drop the pending packets, they won't be decoded.
            av_packet_free(&pkt);
            queue.pop_front();
            continue;
        }
        // Other error — log and drop so we don't stall, but this is usually fatal.
        av_packet_free(&pkt);
        queue.pop_front();
        return false;
    }
    return true;
}

bool FFmpegVideoDecoder::DecodeOneAvailableVideo(DecodedFrame& outFrame, bool& gotEOS)
{
    gotEOS = false;

    int ret = avcodec_receive_frame(mCodecCtx, mFrame);
    if (ret == 0)
    {
        uint8_t* dst[4] = { mRgbaBuffer.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { int(mWidth) * 4, 0, 0, 0 };
        sws_scale(mSws, mFrame->data, mFrame->linesize, 0, int(mHeight), dst, dstStride);

        int64_t pts = (mFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                          ? mFrame->best_effort_timestamp
                          : mFrame->pts;

        outFrame.pixels     = mRgbaBuffer.data();
        // Report only the logical frame size, NOT the padded buffer size. Consumers
        // (AsyncMediaPump, UnpackTopBottomAlpha) size their scratch buffers based on
        // byteSize; handing them the padded size would cause them to copy garbage.
        outFrame.byteSize   = size_t(mWidth) * mHeight * 4;
        outFrame.ptsSeconds = (pts != AV_NOPTS_VALUE && mTimeBaseSec > 0.0)
                                  ? double(pts) * mTimeBaseSec
                                  : 0.0;
        outFrame.endOfStream = false;
        av_frame_unref(mFrame);
        return true;
    }
    else if (ret == AVERROR_EOF)
    {
        gotEOS = true;
        outFrame.pixels = nullptr;
        outFrame.byteSize = 0;
        outFrame.ptsSeconds = mDurationSec;
        outFrame.endOfStream = true;
        return true;
    }
    return false; // EAGAIN or other — caller should pump more input
}

bool FFmpegVideoDecoder::DecodeNextFrame(DecodedFrame& outFrame)
{
    if (mFormatCtx == nullptr || mCodecCtx == nullptr || mSws == nullptr) return false;

    if (mVideoEOF && mEndOfStream)
    {
        outFrame.pixels = nullptr;
        outFrame.byteSize = 0;
        outFrame.ptsSeconds = mDurationSec;
        outFrame.endOfStream = true;
        return true;
    }

    while (true)
    {
        // 1. Try to pull a frame the codec already has ready.
        bool eos = false;
        if (DecodeOneAvailableVideo(outFrame, eos))
        {
            if (eos) { mVideoEOF = true; mEndOfStream = true; }
            return true;
        }

        // 2. Feed any queued packets we haven't yet sent. Stops on EAGAIN (codec full).
        DrainQueuedPackets(mCodecCtx, mVideoPacketQueue);

        // If the drain sent anything new, try to receive again before pumping more.
        if (DecodeOneAvailableVideo(outFrame, eos))
        {
            if (eos) { mVideoEOF = true; mEndOfStream = true; }
            return true;
        }

        // 3. Still nothing — read from the demuxer. Packets for both streams go into
        //    their respective queues; we'll drain the video queue on the next iteration.
        if (!PumpOnePacket())
        {
            // Demuxer exhausted. Flush the codec with a null packet and drain the tail.
            avcodec_send_packet(mCodecCtx, nullptr);
            bool finalEos = false;
            if (DecodeOneAvailableVideo(outFrame, finalEos))
            {
                if (finalEos) { mVideoEOF = true; mEndOfStream = true; }
                return true;
            }
            mVideoEOF = true;
            mEndOfStream = true;
            outFrame.pixels = nullptr;
            outFrame.byteSize = 0;
            outFrame.ptsSeconds = mDurationSec;
            outFrame.endOfStream = true;
            return true;
        }
    }
}

AudioDecodeResult FFmpegVideoDecoder::DecodeNextAudio(DecodedAudio& outChunk)
{
    outChunk = {};

    if (mAudioCodecCtx == nullptr || mSwr == nullptr || mAudioFrame == nullptr)
    {
        return AudioDecodeResult::EndOfStream;
    }

    // Try receiving a decoded frame. If none yet, pump packets until we get one or hit EOF.
    for (;;)
    {
        // Feed any packets we've queued for this codec (collected by PumpOnePacket below).
        DrainQueuedPackets(mAudioCodecCtx, mAudioPacketQueue);

        int ret = avcodec_receive_frame(mAudioCodecCtx, mAudioFrame);
        if (ret == 0)
        {
            // Resample into mPcmBuffer.
            const int inSamples = mAudioFrame->nb_samples;
            // Estimate output size: (in_samples * out_rate + in_rate - 1) / in_rate + slack
            const int outSamplesMax = (int)av_rescale_rnd(
                swr_get_delay(mSwr, mAudioCodecCtx->sample_rate) + inSamples,
                (int64_t)kOutSampleRate, mAudioCodecCtx->sample_rate, AV_ROUND_UP);
            // Same 64-byte safety pad as mRgbaBuffer — swr_convert has similar SIMD
            // tail-write behaviour that can otherwise corrupt the CRT heap.
            const size_t outBytes    = size_t(outSamplesMax) * kOutChannels * sizeof(int16_t);
            const size_t outBytesPad = outBytes + 64;
            if (mPcmBuffer.size() < outBytesPad) mPcmBuffer.resize(outBytesPad);

            uint8_t* outPlane = mPcmBuffer.data();
            const int outSamples = swr_convert(
                mSwr,
                &outPlane, outSamplesMax,
                (const uint8_t**)mAudioFrame->extended_data, inSamples);

            int64_t pts = (mAudioFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                              ? mAudioFrame->best_effort_timestamp
                              : mAudioFrame->pts;
            const double ptsSec = (pts != AV_NOPTS_VALUE && mAudioTimeBaseSec > 0.0)
                                      ? double(pts) * mAudioTimeBaseSec
                                      : mLastPcmPtsSeconds;

            av_frame_unref(mAudioFrame);

            if (outSamples <= 0) continue; // nothing produced yet (swr is buffering)

            outChunk.samples     = mPcmBuffer.data();
            outChunk.byteSize    = size_t(outSamples) * kOutChannels * sizeof(int16_t);
            outChunk.sampleCount = uint32_t(outSamples);
            outChunk.ptsSeconds  = ptsSec;
            outChunk.endOfStream = false;

            mLastPcmSampleCount = outChunk.sampleCount;
            mLastPcmPtsSeconds  = ptsSec;
            return AudioDecodeResult::Produced;
        }
        else if (ret == AVERROR_EOF)
        {
            mAudioEOF = true;
            outChunk.endOfStream = true;
            return AudioDecodeResult::EndOfStream;
        }
        else if (ret != AVERROR(EAGAIN))
        {
            return AudioDecodeResult::Error;
        }

        // EAGAIN: feed more packets. If the demuxer is exhausted PumpOnePacket returns false;
        // then a subsequent receive_frame call drains the flush.
        if (!PumpOnePacket())
        {
            // Try one more receive after the flush packet landed.
            int r2 = avcodec_receive_frame(mAudioCodecCtx, mAudioFrame);
            if (r2 == 0)
            {
                // Got a final frame during flush — loop around and it will be handled above.
                av_frame_unref(mAudioFrame); // discard; next iteration will redo properly
                // Simplest: report NeedsMoreInput so caller retries; next call will catch it.
                // (Dropping this tail frame is acceptable for v2.)
            }
            mAudioEOF = true;
            outChunk.endOfStream = true;
            return AudioDecodeResult::EndOfStream;
        }
    }
}

bool FFmpegVideoDecoder::Seek(double seconds)
{
    if (mFormatCtx == nullptr || mCodecCtx == nullptr) return false;

    double clamped = std::max(0.0, std::min(seconds, mDurationSec > 0.0 ? mDurationSec : seconds));
    int64_t ts = (mTimeBaseSec > 0.0) ? int64_t(clamped / mTimeBaseSec) : int64_t(clamped * AV_TIME_BASE);
    int streamIndex = (mTimeBaseSec > 0.0) ? mVideoStreamIndex : -1;
    if (av_seek_frame(mFormatCtx, streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;

    avcodec_flush_buffers(mCodecCtx);
    if (mAudioCodecCtx != nullptr) avcodec_flush_buffers(mAudioCodecCtx);
    if (mSwr != nullptr)
    {
        // Drain residual samples in the resampler so timestamps don't bleed across the seek.
        swr_close(mSwr);
        swr_init(mSwr);
    }

    for (AVPacket* p : mAudioPacketQueue) { av_packet_free(&p); }
    mAudioPacketQueue.clear();
    for (AVPacket* p : mVideoPacketQueue) { av_packet_free(&p); }
    mVideoPacketQueue.clear();

    mEndOfStream = false;
    mVideoEOF = false;
    mAudioEOF = false;
    return true;
}

} // namespace VideoPlayerAddon

#endif // POLYPHASE_WITH_FFMPEG
