#pragma once

// DSP-ADPCM software codec — Nintendo's GameCube/Wii audio compression format.
// Compression ratio: 14 PCM samples (28 bytes) -> 8 bytes ADPCM = 28% of original
// (~3.5:1). Fixed coefficient set; LPC analysis is skipped for implementation
// simplicity. Quality is acceptable for game audio (speech and music both),
// noticeably worse than well-tuned ADPCM.
//
// Container layout we serialize per channel (matches libogc DspHeader / Nintendo's
// .dsp file convention enough to round-trip ourselves; we do NOT promise binary
// compatibility with Nintendo SDK toolchain output — Nintendo's header is 96 bytes
// with extra trailing padding; ours is 76 bytes packed tight):
//
//   DspChannelHeader (76 bytes, big-endian) — see DspChannelHeader struct
//   uint8_t adpcmData[ceil(numFrames / 14) * 8]
//
// THP wraps these per-channel blobs into a stereo audio component; PCV1 audio with
// codec=ADPCM stores the two channel blobs back-to-back (left, then right). Either
// way, each channel is decoded independently.
//
// All multi-byte fields in the on-disk header are big-endian (Nintendo platforms
// are PowerPC). Use the DspWrite/Read helpers on cook/runtime sides.

#include <cstdint>
#include <vector>

namespace VideoPlayerAddon
{

#pragma pack(push, 1)
struct DspChannelHeader
{
    uint32_t numSamples;        // 0x00 - PCM samples (per channel)
    uint32_t numAdpcmNibbles;   // 0x04 - PCM samples * (8/14), padded to block
    uint32_t sampleRate;        // 0x08 - Hz
    uint16_t loopFlag;          // 0x0C - 0 = no loop
    uint16_t format;            // 0x0E - 0 = ADPCM
    uint32_t loopStartNibble;   // 0x10 - 2 if non-loop
    uint32_t loopEndNibble;     // 0x14
    uint32_t currentNibble;     // 0x18 - 2 (start of audio data)
    int16_t  coef[16];          // 0x1C - 8 (predictor, history2) pairs in Q11
    uint16_t gain;              // 0x3C - always 0
    uint16_t ps;                // 0x3E - predictor/scale of FIRST frame
    int16_t  yn1;               // 0x40 - history t-1
    int16_t  yn2;               // 0x42 - history t-2
    uint16_t loopPs;            // 0x44 - 0 when no loop
    int16_t  loopYn1;           // 0x46
    int16_t  loopYn2;           // 0x48
    uint16_t pad;               // 0x4A - 1 word padding
};                              // total: 0x4C = 76 bytes
#pragma pack(pop)

static_assert(sizeof(DspChannelHeader) == 76, "DspChannelHeader must be 76 bytes");

// Encode one PCM channel to DSP-ADPCM. inSamples are signed 16-bit, mono. The
// resulting outAdpcm bytes are exactly ceil(numSamples / 14) * 8 bytes long.
// outHeader is filled in with the DspChannelHeader (in HOST byte order — caller
// must endian-swap fields when writing to a big-endian wire format like THP).
void DspEncode(
    const int16_t* inSamples,
    uint32_t numSamples,
    uint32_t sampleRate,
    DspChannelHeader& outHeader,
    std::vector<uint8_t>& outAdpcm);

// Decode one DSP-ADPCM channel back to PCM. The header drives the decode;
// numSamples in the header is honored even if the adpcm buffer is longer (the
// last block may carry padding samples that get stripped).
void DspDecode(
    const DspChannelHeader& header,
    const uint8_t* adpcm,
    uint32_t adpcmByteSize,
    int16_t* outSamples,
    uint32_t outCapacitySamples);

// Big-endian byte-swap helper for THP-style wire serialization. Both the cook
// side (write THP) and the runtime side (read THP) call this to flip fields
// between host (LE on PC) and big-endian wire format. Idempotent for fields
// already in BE on a BE host (no-op).
DspChannelHeader DspHeaderToBE(const DspChannelHeader& h);
DspChannelHeader DspHeaderFromBE(const DspChannelHeader& h);

} // namespace VideoPlayerAddon
