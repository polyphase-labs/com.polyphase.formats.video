#pragma once

#include <memory>
#include <string>

namespace VideoPlayerAddon
{

class IVideoDecoder;

// Selects a video decoder backend based on the file extension (and compile-time
// availability of backends). Returns nullptr if no backend can handle the file.
// The returned decoder is not yet opened; call Open(path) next.
std::unique_ptr<IVideoDecoder> CreateVideoDecoder(const std::string& path);

} // namespace VideoPlayerAddon
