// Single-TU build of stb_image with everything but JPEG turned off. The PcvVideoDecoder
// only ever needs to decode baseline JPEGs (cooked by FFmpeg's mjpeg encoder) so we cut
// the rest of stb_image out to keep the addon DLL lean. The engine DLL also compiles
// stb_image but doesn't export its symbols, so the addon needs its own copy.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO

#include <stb_image.h>
