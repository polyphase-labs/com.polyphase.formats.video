#include "Cook/DspAdpcm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace VideoPlayerAddon
{

namespace
{
    // Nintendo's standard fixed coefficient set, used as a fallback when LPC
    // analysis isn't viable (silence, very short clips, degenerate input). For
    // real content the encoder ships per-clip coefficients computed below.
    constexpr int16_t kFallbackCoefs[16] = {
        0,        0,
        2048,     0,
        0,     2048,
        1024,  1024,
        4096, -2048,
        3584, -1536,
        3072, -1024,
        4608, -2560,
    };

    inline int16_t Clamp16(int32_t v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return int16_t(v);
    }

    inline int8_t ClampNibble(int32_t v)
    {
        if (v >  7) return  7;
        if (v < -8) return -8;
        return int8_t(v);
    }

    // Predict next sample. Q11 fixed-point (>>11) matches GC/Wii DSP convention;
    // +1024 gives nearest rounding. Coefs come from the per-clip table.
    inline int32_t Predict(const int16_t* coefs, int predictor, int32_t yn1, int32_t yn2)
    {
        const int32_t c1 = coefs[predictor * 2 + 0];
        const int32_t c2 = coefs[predictor * 2 + 1];
        return ((c1 * yn1) + (c2 * yn2) + 1024) >> 11;
    }

    // Encode one 14-sample block with a fixed (predictor, scale) using `coefs`
    // for prediction. Returns sum-of-squared reconstruction error. Updates
    // yn1/yn2 to the DECODED outputs so encoder/decoder stay in lock-step.
    int64_t EncodeBlockTrial(
        const int16_t* in, uint32_t numIn,
        const int16_t* coefs,
        int predictor, int scale,
        int32_t inYn1, int32_t inYn2,
        int32_t* outYn1, int32_t* outYn2,
        uint8_t* outBlock /* 8 bytes; null = trial-only */)
    {
        int32_t yn1 = inYn1;
        int32_t yn2 = inYn2;
        int64_t errSqSum = 0;
        const int32_t scaleHalf = (scale > 0) ? (1 << (scale - 1)) : 0;

        if (outBlock != nullptr)
        {
            outBlock[0] = uint8_t((predictor << 4) | (scale & 0x0F));
            std::memset(outBlock + 1, 0, 7);
        }

        for (uint32_t i = 0; i < 14; ++i)
        {
            const int32_t sample = (i < numIn) ? int32_t(in[i]) : 0;
            const int32_t pred   = Predict(coefs, predictor, yn1, yn2);

            const int32_t diff = sample - pred;
            int32_t nib = (diff >= 0)
                ? ((diff + scaleHalf) >> scale)
                : -(((-diff) + scaleHalf) >> scale);
            nib = ClampNibble(nib);

            const int32_t recon = Clamp16(pred + (nib << scale));
            yn2 = yn1;
            yn1 = recon;

            const int64_t e = int64_t(sample) - int64_t(recon);
            errSqSum += e * e;

            if (outBlock != nullptr)
            {
                const uint32_t byteIdx = 1 + (i / 2);
                if ((i & 1) == 0)
                    outBlock[byteIdx] = uint8_t((nib & 0x0F) << 4);
                else
                    outBlock[byteIdx] |= uint8_t(nib & 0x0F);
            }
        }

        if (outYn1 != nullptr) *outYn1 = yn1;
        if (outYn2 != nullptr) *outYn2 = yn2;
        return errSqSum;
    }

    // ===================================================================
    // Per-clip coefficient analysis (LPC + k-means).
    //
    // The 8-pair coefficient TABLE in the channel header is what makes
    // DSP-ADPCM quality scale or fail. With Nintendo's generic table the
    // encoder is stuck picking from 8 fixed predictors that may or may
    // not match the clip's spectral content; signals with strong bass +
    // bright transients lose either bass (too few low-freq predictors)
    // or highs (too few alternation predictors), leaving you with the
    // "thin / distorted / only-the-high-end" sound my old encoder
    // produced.
    //
    // Real Nintendo encoders compute the table FROM the audio:
    //   1. Window the signal, compute the optimal 2nd-order linear
    //      predictor (c1, c2) for each window via the normal equations
    //      c1*R0 + c2*R1 = R1
    //      c1*R1 + c2*R0 = R2
    //   2. Cluster all those (c1, c2) points into 8 groups (k-means in
    //      2D) — the centroids become the 8 predictor pairs.
    //   3. Embed the 8 pairs in the channel header. Decoder reads the
    //      table from the header, so encode/decode stay in lock-step
    //      automatically.
    //
    // The block-search loop is unchanged — it just picks among the
    // clip-tuned 8 predictors instead of the generic ones.
    // ===================================================================

    // Compute the optimal normalized (c1, c2) for a small frame of PCM via the
    // standard 2nd-order LPC normal equations. Returns (1.0, 0.0) — i.e. "next
    // sample equals previous" — for degenerate input (silence, < 3 samples,
    // singular system).
    void FrameLpcNorm(const int16_t* frame, uint32_t n, double& outC1, double& outC2)
    {
        if (n < 3) { outC1 = 1.0; outC2 = 0.0; return; }

        double r0 = 0.0, r1 = 0.0, r2 = 0.0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const double x = double(frame[i]);
            r0 += x * x;
        }
        for (uint32_t i = 0; i + 1 < n; ++i)
        {
            r1 += double(frame[i]) * double(frame[i + 1]);
        }
        for (uint32_t i = 0; i + 2 < n; ++i)
        {
            r2 += double(frame[i]) * double(frame[i + 2]);
        }

        // System: [R0 R1; R1 R0] * [c1; c2] = [R1; R2]
        // Solution: c1 = (R0*R1 - R1*R2)/(R0^2 - R1^2)
        //           c2 = (R0*R2 - R1*R1)/(R0^2 - R1^2)
        const double det = r0 * r0 - r1 * r1;
        if (r0 < 1.0 || std::fabs(det) < 1e-9)
        {
            outC1 = 1.0; outC2 = 0.0;
            return;
        }

        const double c1 = (r0 * r1 - r1 * r2) / det;
        const double c2 = (r0 * r2 - r1 * r1) / det;
        outC1 = c1;
        outC2 = c2;
    }

    // K-means clustering in 2D with k=8. Initialized from 8 evenly-spaced
    // points along the input list (deterministic, reproducible cooks). Stops
    // when no point changes assignment, or after kMaxIters.
    void KMeans2D8(
        const std::vector<std::pair<double, double>>& points,
        std::array<std::pair<double, double>, 8>& centroids)
    {
        const size_t n = points.size();
        if (n == 0)
        {
            for (auto& c : centroids) { c.first = 0.0; c.second = 0.0; }
            return;
        }

        // Deterministic init: pick 8 points evenly spaced through the input.
        // (Random init makes cook output non-deterministic across runs, which
        // breaks reproducible builds.)
        for (size_t k = 0; k < 8; ++k)
        {
            const size_t idx = (n >= 8) ? ((k * n) / 8) : (k % n);
            centroids[k] = points[idx];
        }

        std::vector<int> assign(n, -1);

        constexpr int kMaxIters = 30;
        for (int iter = 0; iter < kMaxIters; ++iter)
        {
            bool changed = false;
            for (size_t i = 0; i < n; ++i)
            {
                int    bestK = 0;
                double bestD = 1e30;
                for (int k = 0; k < 8; ++k)
                {
                    const double dx = points[i].first  - centroids[k].first;
                    const double dy = points[i].second - centroids[k].second;
                    const double d  = dx * dx + dy * dy;
                    if (d < bestD) { bestD = d; bestK = k; }
                }
                if (assign[i] != bestK) { assign[i] = bestK; changed = true; }
            }
            if (!changed) break;

            std::array<double, 8> sumX{}, sumY{};
            std::array<int,    8> count{};
            for (size_t i = 0; i < n; ++i)
            {
                const int k = assign[i];
                sumX[k] += points[i].first;
                sumY[k] += points[i].second;
                count[k]++;
            }
            for (int k = 0; k < 8; ++k)
            {
                if (count[k] > 0)
                {
                    centroids[k].first  = sumX[k] / double(count[k]);
                    centroids[k].second = sumY[k] / double(count[k]);
                }
                // Orphan centroids stay where they are. Encoder never picks
                // them, which is fine — we still have ≥1 useful centroid.
            }
        }
    }

    // Stability test for the AR(2) predictor x[n] = c1*x[n-1] + c2*x[n-2].
    // Strictly stable iff both poles of (z^2 - c1*z - c2) lie inside the unit
    // circle, which gives the Schur-Cohn conditions:
    //   |c2| < 1
    //   |c1| < 1 - c2
    // The ADPCM codec works with marginally-stable predictors too (clamp16 +
    // residual nib correction prevent true divergence), and Nintendo's
    // standard table sits right on / past the boundary — e.g. (c1=2.25,
    // c2=-1.25). What's NOT acceptable is predictors with poles well outside
    // the unit circle, because those produce predictions that grow so fast
    // (factors of 3-10 between samples) that the 4-bit residual nib can't
    // compensate. The encoder's trial loop is forced to "best of bad", and
    // the chosen reconstruction saturates at ±32767 oscillating at sample
    // rate / 2 — audible as a high-pitched shriek that doesn't track the
    // source. Allow Nintendo's range plus a little slack; reject anything
    // beyond.
    inline bool IsAcceptableCoef(double c1, double c2)
    {
        if (!std::isfinite(c1) || !std::isfinite(c2)) return false;
        if (std::fabs(c2) > 1.5) return false;
        if (std::fabs(c1) > 2.5) return false;
        // Triangle test with slack — Nintendo's (2.25, -1.25) sits at
        // |c1| = 2.25, 1 - c2 = 2.25 (exact boundary). 2.6 gives a hair
        // of margin past that for LPC outputs that are essentially the
        // same predictor with floating-point noise.
        if (std::fabs(c1) > 2.6 - c2) return false;
        return true;
    }

    // Predictor table selection.
    //
    // STATUS: per-clip LPC analysis is implemented above (FrameLpcNorm +
    // KMeans2D8 + IsAcceptableCoef) but currently *disabled* — every cook
    // attempt with LPC produced audibly-broken output (Nyquist screech, then
    // modem-like tones + scratches even with stability filtering and
    // fallback-replacement of unstable centroids). The implementation has a
    // bug somewhere between the LPC math, the k-means clustering, and how
    // the resulting coefficients interact with the per-block encoder, that
    // I haven't been able to find by inspection alone.
    //
    // Reverting to Nintendo's fixed coefficient table here. That gets us
    // back to the previous baseline ("OK with crackling, not nightmarish")
    // immediately. The LPC machinery is left in the file so that a future
    // pass — with actual debug visibility into per-block predictor
    // selection, encoded bytes, and decoded samples — can either fix it or
    // replace it with a reference implementation (Nintendo's dspadpcm.exe
    // or vgmstream's encoder).
    void ComputeClipCoefs(const int16_t* /*samples*/, uint32_t /*numSamples*/,
                         int16_t outCoefs[16])
    {
        std::memcpy(outCoefs, kFallbackCoefs, sizeof(kFallbackCoefs));
    }
} // namespace

void DspEncode(
    const int16_t* inSamples,
    uint32_t numSamples,
    uint32_t sampleRate,
    DspChannelHeader& outHeader,
    std::vector<uint8_t>& outAdpcm)
{
    // Step 1: derive the 8-pair predictor table from the source PCM. This is
    // what differentiates a "fixed-table" encoder (audible distortion on
    // varied content) from a real Nintendo-grade encoder (clean playback).
    int16_t coefs[16];
    ComputeClipCoefs(inSamples, numSamples, coefs);

    const uint32_t numBlocks   = (numSamples + 13) / 14;
    const uint32_t outByteSize = numBlocks * 8;
    outAdpcm.assign(outByteSize, 0);

    int32_t  yn1     = 0;
    int32_t  yn2     = 0;
    uint16_t firstPs = 0;

    for (uint32_t b = 0; b < numBlocks; ++b)
    {
        const uint32_t sampleStart = b * 14;
        const uint32_t sampleEnd   = std::min<uint32_t>(sampleStart + 14, numSamples);
        const uint32_t numIn       = sampleEnd - sampleStart;

        // Step 2: per-block search over (predictor, scale). Same search as
        // before — but now the 8 predictors are clip-tuned, so the lowest-
        // error pick actually represents the signal well.
        int     bestPred  = 0;
        int     bestScale = 0;
        int64_t bestErr   = INT64_MAX;
        int32_t bestYn1   = yn1;
        int32_t bestYn2   = yn2;

        for (int p = 0; p < 8; ++p)
        {
            for (int s = 0; s <= 12; ++s)
            {
                int32_t tYn1 = 0, tYn2 = 0;
                int64_t err = EncodeBlockTrial(
                    inSamples + sampleStart, numIn,
                    coefs, p, s, yn1, yn2,
                    &tYn1, &tYn2,
                    /*outBlock=*/nullptr);
                if (err < bestErr)
                {
                    bestErr   = err;
                    bestPred  = p;
                    bestScale = s;
                    bestYn1   = tYn1;
                    bestYn2   = tYn2;
                }
            }
        }

        // Emit the chosen block.
        int32_t emitYn1 = 0, emitYn2 = 0;
        EncodeBlockTrial(
            inSamples + sampleStart, numIn,
            coefs, bestPred, bestScale, yn1, yn2,
            &emitYn1, &emitYn2,
            outAdpcm.data() + b * 8);

        if (b == 0)
        {
            firstPs = uint16_t(outAdpcm[0]);
        }

        yn1 = bestYn1;
        yn2 = bestYn2;
    }

    // Channel header. All fields HOST byte order — caller swaps to BE for
    // wire format via DspHeaderToBE.
    std::memset(&outHeader, 0, sizeof(outHeader));
    outHeader.numSamples       = numSamples;
    outHeader.numAdpcmNibbles  = numBlocks * 16;
    outHeader.sampleRate       = sampleRate;
    outHeader.loopFlag         = 0;
    outHeader.format           = 0;
    outHeader.loopStartNibble  = 2;
    outHeader.loopEndNibble    = (numBlocks * 16) - 1;
    outHeader.currentNibble    = 2;
    // Embed the per-clip coefficient table — the decoder reads from
    // header.coef[], not a hardcoded constant, so encode/decode round-trip
    // automatically with whatever table we put here.
    std::memcpy(outHeader.coef, coefs, sizeof(coefs));
    outHeader.gain    = 0;
    outHeader.ps      = firstPs;
    outHeader.yn1     = 0;
    outHeader.yn2     = 0;
    outHeader.loopPs  = 0;
    outHeader.loopYn1 = 0;
    outHeader.loopYn2 = 0;
    outHeader.pad     = 0;
}

void DspDecode(
    const DspChannelHeader& header,
    const uint8_t* adpcm,
    uint32_t adpcmByteSize,
    int16_t* outSamples,
    uint32_t outCapacitySamples)
{
    const uint32_t numSamples = std::min(header.numSamples, outCapacitySamples);
    const uint32_t numBlocks  = adpcmByteSize / 8;

    int32_t  yn1     = header.yn1;
    int32_t  yn2     = header.yn2;
    uint32_t emitted = 0;

    for (uint32_t b = 0; b < numBlocks && emitted < numSamples; ++b)
    {
        const uint8_t  ps        = adpcm[b * 8];
        const int      predictor = (ps >> 4) & 0x07;
        const int      scale     = ps & 0x0F;
        const int32_t  c1        = header.coef[predictor * 2 + 0];
        const int32_t  c2        = header.coef[predictor * 2 + 1];

        for (uint32_t i = 0; i < 14 && emitted < numSamples; ++i)
        {
            const uint32_t byteIdx = b * 8 + 1 + (i / 2);
            const uint8_t  byte    = adpcm[byteIdx];
            int32_t nib = ((i & 1) == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
            if (nib & 0x08) nib |= 0xFFFFFFF0;

            const int32_t pred  = ((c1 * yn1) + (c2 * yn2) + 1024) >> 11;
            const int32_t recon = Clamp16(pred + (nib << scale));

            outSamples[emitted++] = int16_t(recon);
            yn2 = yn1;
            yn1 = recon;
        }
    }

    while (emitted < outCapacitySamples)
    {
        outSamples[emitted++] = 0;
    }
}

namespace
{
    inline uint16_t Swap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
    inline uint32_t Swap32(uint32_t v)
    {
        return ( v >> 24)               |
               ((v >>  8) & 0x0000FF00u) |
               ((v <<  8) & 0x00FF0000u) |
               ( v << 24);
    }
    inline int16_t  SwapI16(int16_t v)  { uint16_t u; std::memcpy(&u, &v, 2); u = Swap16(u); int16_t r; std::memcpy(&r, &u, 2); return r; }
}

DspChannelHeader DspHeaderToBE(const DspChannelHeader& h)
{
    DspChannelHeader o = h;
    o.numSamples      = Swap32(o.numSamples);
    o.numAdpcmNibbles = Swap32(o.numAdpcmNibbles);
    o.sampleRate      = Swap32(o.sampleRate);
    o.loopFlag        = Swap16(o.loopFlag);
    o.format          = Swap16(o.format);
    o.loopStartNibble = Swap32(o.loopStartNibble);
    o.loopEndNibble   = Swap32(o.loopEndNibble);
    o.currentNibble   = Swap32(o.currentNibble);
    for (int i = 0; i < 16; ++i) o.coef[i] = SwapI16(o.coef[i]);
    o.gain    = Swap16(o.gain);
    o.ps      = Swap16(o.ps);
    o.yn1     = SwapI16(o.yn1);
    o.yn2     = SwapI16(o.yn2);
    o.loopPs  = Swap16(o.loopPs);
    o.loopYn1 = SwapI16(o.loopYn1);
    o.loopYn2 = SwapI16(o.loopYn2);
    o.pad     = Swap16(o.pad);
    return o;
}

DspChannelHeader DspHeaderFromBE(const DspChannelHeader& h)
{
    return DspHeaderToBE(h); // byte-swap is its own inverse
}

} // namespace VideoPlayerAddon
