#pragma once

#include <cstddef>
#include <cstdint>

namespace VideoPlayerAddon
{

struct VideoFrameDesc
{
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DecodedFrame
{
    const uint8_t* pixels = nullptr; // RGBA8, tightly packed, row-major, width * height * 4 bytes
    size_t byteSize = 0;
    double ptsSeconds = 0.0;
    bool endOfStream = false;
};

struct AudioStreamDesc
{
    uint32_t sampleRate = 0;   // frames per second (post-resample if the decoder resamples)
    uint32_t numChannels = 0;  // 1 = mono, 2 = stereo (others unsupported by engine mixer)
    uint32_t bitsPerSample = 16;
};

struct DecodedAudio
{
    const uint8_t* samples = nullptr;  // int16 interleaved PCM at AudioStreamDesc::sampleRate
    size_t byteSize = 0;
    uint32_t sampleCount = 0;          // frames (samples per channel)
    double ptsSeconds = 0.0;
    bool endOfStream = false;
};

enum class AudioDecodeResult : int32_t
{
    NeedsMoreInput = 0, // call DecodeNextFrame or pump the demuxer; not an error
    Produced       = 1,
    EndOfStream    = 2,
    Error          = -1,
};

class IVideoDecoder
{
public:
    virtual ~IVideoDecoder() = default;

    virtual bool Open(const char* path) = 0;

    // Open from a memory buffer instead of a filesystem path. Used when playing back a
    // VideoClip asset whose source bytes live inside the cooked .oct stream rather than
    // as a loose file. `codecHint` is a lower-cased extension string ("mp4", "webm", ...)
    // used by demuxers that need a format hint when no file extension is available; may
    // be nullptr/empty if the demuxer can probe the container itself.
    // The buffer must remain valid until Close(); the decoder does not take ownership.
    // Backends that don't support in-memory playback can leave the default false return.
    virtual bool OpenMemory(const uint8_t* data, size_t size, const char* codecHint)
    {
        (void)data; (void)size; (void)codecHint;
        return false;
    }

    virtual void Close() = 0;

    virtual VideoFrameDesc GetFrameDesc() const = 0;
    virtual double GetDurationSeconds() const = 0;
    virtual double GetFrameRate() const = 0;

    // Pull the next decoded frame. Returns true and populates outFrame on success.
    // On end-of-stream, returns true with outFrame.endOfStream == true.
    // On error, returns false.
    // The returned pixel pointer is owned by the decoder and remains valid until the next
    // call to DecodeNextFrame, Seek, or Close.
    virtual bool DecodeNextFrame(DecodedFrame& outFrame) = 0;

    // Seek to the given presentation time. The next DecodeNextFrame call returns the
    // nearest decodable frame at or after the requested time.
    virtual bool Seek(double seconds) = 0;

    // ---- Audio (optional; default implementations report "no audio") ----

    // Whether the currently-open media has a decodable audio stream.
    virtual bool HasAudio() const { return false; }

    // Describes the post-resample PCM format the decoder will emit in DecodeNextAudio.
    virtual AudioStreamDesc GetAudioDesc() const { return {}; }

    // Pull the next decoded audio chunk. Returns Produced + populates outChunk, or
    // NeedsMoreInput (caller should pump the demuxer via DecodeNextFrame which also
    // consumes audio packets), or EndOfStream, or Error.
    // The returned sample pointer is owned by the decoder until the next call.
    virtual AudioDecodeResult DecodeNextAudio(DecodedAudio& outChunk)
    {
        (void)outChunk;
        return AudioDecodeResult::EndOfStream;
    }
};

} // namespace VideoPlayerAddon
