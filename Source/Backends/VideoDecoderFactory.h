#pragma once

#include <memory>
#include <string>

class VideoClip;

namespace VideoPlayerAddon
{

class IVideoDecoder;

// Selects a video decoder backend based on the file extension (and compile-time
// availability of backends). Returns nullptr if no backend can handle the file.
// The returned decoder is not yet opened; call Open(path) next.
std::unique_ptr<IVideoDecoder> CreateVideoDecoder(const std::string& path);

// Selects a video decoder backend for a VideoClip asset (in-memory bytes plus
// codec hint). Mirrors CreateVideoDecoder but routes per-platform — PC uses the
// FFmpeg backend over the asset's source bytes; consoles will pick THP / MVD
// when those backends land. The returned decoder is not yet opened; call
// OpenMemory(clip->GetSourceData().data(), ..., clip->GetCodecHint().c_str()).
std::unique_ptr<IVideoDecoder> CreateVideoDecoderForClip(const VideoClip* clip);

} // namespace VideoPlayerAddon
