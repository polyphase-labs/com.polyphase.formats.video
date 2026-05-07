#pragma once

#if POLYPHASE_WITH_FFMPEG

#include "Backends/IVideoDecoder.h"

#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVIOContext;
struct SwsContext;
struct SwrContext;

#include <deque>

namespace VideoPlayerAddon
{

class FFmpegVideoDecoder : public IVideoDecoder
{
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder() override;

    bool Open(const char* path) override;
    bool OpenMemory(const uint8_t* data, size_t size, const char* codecHint) override;
    void Close() override;

    VideoFrameDesc GetFrameDesc() const override { return {mWidth, mHeight}; }
    double GetDurationSeconds() const override { return mDurationSec; }
    double GetFrameRate() const override { return mFrameRate; }

    bool DecodeNextFrame(DecodedFrame& outFrame) override;
    bool Seek(double seconds) override;

    // --- Audio ---
    bool HasAudio() const override            { return mAudioStreamIndex >= 0; }
    AudioStreamDesc GetAudioDesc() const override;
    AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk) override;

private:
    // Shared post-avformat_open_input setup (find_stream_info, video + audio
    // codec init, sws/swr alloc). Used by both Open(path) and OpenMemory(buf).
    bool FinishOpen();

    // AVIOContext callbacks for OpenMemory. Static so they have a C-compatible
    // signature; opaque is the FFmpegVideoDecoder*.
    static int     MemReadPacket(void* opaque, uint8_t* buf, int bufSize);
    static int64_t MemSeek(void* opaque, int64_t offset, int whence);

    bool DecodeOneAvailableVideo(DecodedFrame& outFrame, bool& gotEOS);
    bool OpenAudioStream();
    void CloseAudio();
    // Pumps av_read_frame once and routes the packet to the video codec, the audio codec,
    // or an internal buffer. Returns false on fatal read error; EOF is signalled via the
    // corresponding flag (mVideoEOF / mAudioEOF).
    bool PumpOnePacket();

    AVFormatContext* mFormatCtx  = nullptr;

    // In-memory source state. mAvioCtx is non-null only when the asset was opened via
    // OpenMemory; Close() tears it down. mMemPos cursors into the externally-owned
    // mMemData buffer (size mMemSize).
    AVIOContext*     mAvioCtx    = nullptr;
    const uint8_t*   mMemData    = nullptr;
    size_t           mMemSize    = 0;
    size_t           mMemPos     = 0;

    // Video pipeline
    AVCodecContext*  mCodecCtx   = nullptr;
    AVFrame*         mFrame      = nullptr;
    AVFrame*         mRgbaFrame  = nullptr;
    AVPacket*        mPacket     = nullptr;
    SwsContext*      mSws        = nullptr;
    int mVideoStreamIndex = -1;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    double mDurationSec = 0.0;
    double mFrameRate = 0.0;
    double mTimeBaseSec = 0.0;
    std::vector<uint8_t> mRgbaBuffer;
    bool mVideoEOF = false;

    // Audio pipeline
    AVCodecContext*  mAudioCodecCtx = nullptr;
    AVFrame*         mAudioFrame    = nullptr;
    SwrContext*      mSwr           = nullptr;
    int mAudioStreamIndex = -1;
    uint32_t mAudioSampleRate = 0;
    uint32_t mAudioNumChannels = 0;
    double mAudioTimeBaseSec = 0.0;
    // Output PCM buffer: int16 stereo interleaved at mAudioSampleRate, sized for the largest
    // frame swresample has produced so far.
    std::vector<uint8_t> mPcmBuffer;
    uint32_t mLastPcmSampleCount = 0;
    double mLastPcmPtsSeconds = 0.0;
    bool mAudioEOF = false;

    // Shared
    std::deque<AVPacket*> mAudioPacketQueue;
    std::deque<AVPacket*> mVideoPacketQueue;
    bool mEndOfStream = false; // legacy / deprecated — use mVideoEOF && mAudioEOF for true EOS
};

} // namespace VideoPlayerAddon

#endif // POLYPHASE_WITH_FFMPEG
