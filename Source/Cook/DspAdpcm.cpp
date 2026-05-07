#include "Cook/DspAdpcm.h"

#include "Log.h"

#include <algorithm>
#include <array>
#include <cfloat>
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
    // Per-clip coefficient analysis — DSPCorrelateCoefs.
    //
    // Direct port of Nintendo's documented DSP-ADPCM coefficient analysis
    // algorithm from jackoalan/gc-dspadpcm-encode (grok.c, MIT). The same
    // algorithm is used by Thealexbarney/VGAudio / DspTool. Both upstream
    // implementations trace back to BrawlLib's AudioConverter.cs.
    //
    // The algorithm produces 8 (c1, c2) predictor pairs from the source PCM:
    //   1. Frame the input into 0x3800-sample (1024-block) analysis windows.
    //   2. Per-frame: compute autocorrelation, solve the normal equations
    //      via partial-pivot Gaussian elimination, extract a 2nd-order
    //      reflection-coefficient pair via Levinson-style recursion.
    //   3. Cluster all per-frame records into 8 representative pairs through
    //      iterative refinement (FilterRecords). The split metric is a
    //      contrast function that approximates ADPCM reconstruction error
    //      (not pure LPC prediction error) — that's why a naive LPC +
    //      k-means pipeline doesn't produce equivalent quality.
    //   4. Quantize the 8 pairs to int16 Q11 and embed them in the channel
    //      header. The decoder reads from header.coef[] so encode/decode
    //      round-trip is automatic with whatever table we write here.
    //
    // The block-search loop in DspEncode is unchanged — it picks among the
    // clip-tuned 8 predictors instead of the generic fallback table.
    //
    // Function names match the upstream reference verbatim so future audits
    // can diff against grok.c / DspEncoder.cs without translation.
    // ===================================================================

    using TVec = std::array<double, 3>;

    inline void InnerProductMerge(TVec& vecOut, const int16_t* pcmBuf)
    {
        for (int i = 0; i <= 2; ++i)
        {
            vecOut[i] = 0.0;
            for (int x = 0; x < 14; ++x)
                vecOut[i] -= double(pcmBuf[x - i]) * double(pcmBuf[x]);
        }
    }

    inline void OuterProductMerge(TVec mtxOut[3], const int16_t* pcmBuf)
    {
        for (int x = 1; x <= 2; ++x)
        {
            for (int y = 1; y <= 2; ++y)
            {
                mtxOut[x][y] = 0.0;
                for (int z = 0; z < 14; ++z)
                    mtxOut[x][y] += double(pcmBuf[z - x]) * double(pcmBuf[z - y]);
            }
        }
    }

    bool AnalyzeRanges(TVec mtx[3], int* vecIdxsOut)
    {
        double recips[3] = { 0.0, 0.0, 0.0 };
        double val, tmp, mn, mx;

        for (int x = 1; x <= 2; ++x)
        {
            val = std::max(std::fabs(mtx[x][1]), std::fabs(mtx[x][2]));
            if (val < DBL_EPSILON)
                return true;
            recips[x] = 1.0 / val;
        }

        int maxIndex = 0;
        for (int i = 1; i <= 2; ++i)
        {
            for (int x = 1; x < i; ++x)
            {
                tmp = mtx[x][i];
                for (int y = 1; y < x; ++y)
                    tmp -= mtx[x][y] * mtx[y][i];
                mtx[x][i] = tmp;
            }

            val = 0.0;
            for (int x = i; x <= 2; ++x)
            {
                tmp = mtx[x][i];
                for (int y = 1; y < i; ++y)
                    tmp -= mtx[x][y] * mtx[y][i];
                mtx[x][i] = tmp;
                tmp = std::fabs(tmp) * recips[x];
                if (tmp >= val)
                {
                    val = tmp;
                    maxIndex = x;
                }
            }

            if (maxIndex != i)
            {
                for (int y = 1; y <= 2; ++y)
                {
                    tmp = mtx[maxIndex][y];
                    mtx[maxIndex][y] = mtx[i][y];
                    mtx[i][y] = tmp;
                }
                recips[maxIndex] = recips[i];
            }

            vecIdxsOut[i] = maxIndex;
            if (mtx[i][i] == 0.0)
                return true;

            if (i != 2)
            {
                tmp = 1.0 / mtx[i][i];
                for (int x = i + 1; x <= 2; ++x)
                    mtx[x][i] *= tmp;
            }
        }

        mn = 1.0e10;
        mx = 0.0;
        for (int i = 1; i <= 2; ++i)
        {
            tmp = std::fabs(mtx[i][i]);
            if (tmp < mn) mn = tmp;
            if (tmp > mx) mx = tmp;
        }
        if (mn / mx < 1.0e-10)
            return true;

        return false;
    }

    void BidirectionalFilter(TVec mtx[3], const int* vecIdxs, TVec& vecOut)
    {
        double tmp;
        for (int i = 1, x = 0; i <= 2; ++i)
        {
            int index = vecIdxs[i];
            tmp = vecOut[index];
            vecOut[index] = vecOut[i];
            if (x != 0)
            {
                for (int y = x; y <= i - 1; ++y)
                    tmp -= vecOut[y] * mtx[i][y];
            }
            else if (tmp != 0.0)
            {
                x = i;
            }
            vecOut[i] = tmp;
        }

        for (int i = 2; i > 0; --i)
        {
            tmp = vecOut[i];
            for (int y = i + 1; y <= 2; ++y)
                tmp -= vecOut[y] * mtx[i][y];
            vecOut[i] = tmp / mtx[i][i];
        }

        vecOut[0] = 1.0;
    }

    bool QuadraticMerge(TVec& inOutVec)
    {
        const double v2  = inOutVec[2];
        const double tmp = 1.0 - (v2 * v2);

        if (tmp == 0.0)
            return true;

        const double v0 = (inOutVec[0] - (v2 * v2)) / tmp;
        const double v1 = (inOutVec[1] - (inOutVec[1] * v2)) / tmp;

        inOutVec[0] = v0;
        inOutVec[1] = v1;
        return std::fabs(v1) > 1.0;
    }

    void FinishRecord(TVec& in, TVec& out)
    {
        for (int z = 1; z <= 2; ++z)
        {
            if (in[z] >=  1.0) in[z] =  0.9999999999;
            else if (in[z] <= -1.0) in[z] = -0.9999999999;
        }
        out[0] = 1.0;
        out[1] = (in[2] * in[1]) + in[1];
        out[2] = in[2];
    }

    void MatrixFilter(const TVec& src, TVec& dst)
    {
        TVec mtx[3] = { TVec{0,0,0}, TVec{0,0,0}, TVec{0,0,0} };

        mtx[2][0] = 1.0;
        for (int i = 1; i <= 2; ++i)
            mtx[2][i] = -src[i];

        for (int i = 2; i > 0; --i)
        {
            const double val = 1.0 - (mtx[i][i] * mtx[i][i]);
            for (int y = 1; y <= i; ++y)
                mtx[i - 1][y] = ((mtx[i][i] * mtx[i][y]) + mtx[i][y]) / val;
        }

        dst[0] = 1.0;
        for (int i = 1; i <= 2; ++i)
        {
            dst[i] = 0.0;
            for (int y = 1; y <= i; ++y)
                dst[i] += mtx[i][y] * dst[i - y];
        }
    }

    void MergeFinishRecord(const TVec& src, TVec& dst)
    {
        TVec   tmp{ 0.0, 0.0, 0.0 };
        double val = src[0];

        dst[0] = 1.0;
        for (int i = 1; i <= 2; ++i)
        {
            double v2 = 0.0;
            for (int y = 1; y < i; ++y)
                v2 += dst[y] * src[i - y];

            if (val > 0.0)
                dst[i] = -(v2 + src[i]) / val;
            else
                dst[i] = 0.0;

            tmp[i] = dst[i];

            for (int y = 1; y < i; ++y)
                dst[y] += dst[i] * dst[i - y];

            val *= 1.0 - (dst[i] * dst[i]);
        }

        FinishRecord(tmp, dst);
    }

    double ContrastVectors(const TVec& source1, const TVec& source2)
    {
        const double val =
            (source2[2] * source2[1] + -source2[1]) / (1.0 - source2[2] * source2[2]);
        const double val1 =
            (source1[0] * source1[0]) + (source1[1] * source1[1]) + (source1[2] * source1[2]);
        const double val2 = (source1[0] * source1[1]) + (source1[1] * source1[2]);
        const double val3 = source1[0] * source1[2];
        return val1
             + (2.0 * val * val2)
             + (2.0 * (-source2[1] * val + -source2[2]) * val3);
    }

    void FilterRecords(TVec vecBest[8], int exp,
                       const std::vector<TVec>& records, int recordCount)
    {
        TVec   bufferList[8] = {};
        int    buffer1[8]    = { 0, 0, 0, 0, 0, 0, 0, 0 };
        TVec   buffer2{ 0.0, 0.0, 0.0 };

        int    index;
        double value;
        double tempVal = 0.0;

        for (int x = 0; x < 2; ++x)
        {
            for (int y = 0; y < exp; ++y)
            {
                buffer1[y] = 0;
                for (int i = 0; i <= 2; ++i)
                    bufferList[y][i] = 0.0;
            }

            for (int z = 0; z < recordCount; ++z)
            {
                index = 0;
                value = 1.0e30;
                for (int i = 0; i < exp; ++i)
                {
                    tempVal = ContrastVectors(vecBest[i], records[size_t(z)]);
                    if (tempVal < value)
                    {
                        value = tempVal;
                        index = i;
                    }
                }
                buffer1[index]++;
                MatrixFilter(records[size_t(z)], buffer2);
                for (int i = 0; i <= 2; ++i)
                    bufferList[index][i] += buffer2[i];
            }

            for (int i = 0; i < exp; ++i)
            {
                if (buffer1[i] > 0)
                {
                    for (int y = 0; y <= 2; ++y)
                        bufferList[i][y] /= double(buffer1[i]);
                }
            }

            for (int i = 0; i < exp; ++i)
                MergeFinishRecord(bufferList[i], vecBest[i]);
        }
    }

    void DSPCorrelateCoefs(const int16_t* source, int samples, int16_t coefsOut[16])
    {
        constexpr int kFrameSize = 0x3800;
        const int     numFrames  = (samples + 13) / 14;

        std::vector<int16_t> blockBuffer(size_t(kFrameSize), int16_t(0));

        // 2 rows of 14 samples each: row 0 = previous block, row 1 = current.
        // Inner/OuterProductMerge take pcmHistBuffer + 14 and access negative
        // indices like pcmBuf[x - i] which wrap into row 0. The 28-element
        // contiguous layout makes that valid.
        int16_t pcmHistBuffer[28] = {};

        TVec  vec1{ 0.0, 0.0, 0.0 };
        TVec  vec2{ 0.0, 0.0, 0.0 };
        TVec  mtx[3]      = { TVec{0,0,0}, TVec{0,0,0}, TVec{0,0,0} };
        int   vecIdxs[3]  = { 0, 0, 0 };

        std::vector<TVec> records(size_t(numFrames) * 2, TVec{ 0.0, 0.0, 0.0 });
        int               recordCount = 0;

        TVec vecBest[8] = {};

        int            remaining = samples;
        const int16_t* srcCursor = source;
        while (remaining > 0)
        {
            int frameSamples;
            if (remaining > kFrameSize)
            {
                frameSamples = kFrameSize;
                remaining   -= kFrameSize;
            }
            else
            {
                frameSamples = remaining;
                for (int z = 0; z < 14 && z + frameSamples < kFrameSize; ++z)
                    blockBuffer[size_t(frameSamples + z)] = 0;
                remaining = 0;
            }

            std::memcpy(blockBuffer.data(), srcCursor,
                        size_t(frameSamples) * sizeof(int16_t));
            srcCursor += frameSamples;

            for (int i = 0; i < frameSamples; )
            {
                for (int z = 0; z < 14; ++z)
                    pcmHistBuffer[z] = pcmHistBuffer[14 + z];
                for (int z = 0; z < 14; ++z)
                    pcmHistBuffer[14 + z] = blockBuffer[size_t(i++)];

                InnerProductMerge(vec1, pcmHistBuffer + 14);
                if (std::fabs(vec1[0]) > 10.0)
                {
                    OuterProductMerge(mtx, pcmHistBuffer + 14);
                    if (!AnalyzeRanges(mtx, vecIdxs))
                    {
                        BidirectionalFilter(mtx, vecIdxs, vec1);
                        if (!QuadraticMerge(vec1))
                        {
                            FinishRecord(vec1, records[size_t(recordCount)]);
                            recordCount++;
                        }
                    }
                }
            }
        }

        // No usable analysis windows (e.g. silence): fall back to Nintendo's
        // generic coefficient table. This preserves the prior baseline.
        if (recordCount == 0)
        {
            std::memcpy(coefsOut, kFallbackCoefs, sizeof(kFallbackCoefs));
            return;
        }

        // Initial centroid: the average of MatrixFilter(records[z]) over all
        // frames. vecBest[0] is the seed for the iterative split.
        vec1 = TVec{ 1.0, 0.0, 0.0 };
        for (int z = 0; z < recordCount; ++z)
        {
            MatrixFilter(records[size_t(z)], vecBest[0]);
            for (int y = 1; y <= 2; ++y)
                vec1[y] += vecBest[0][y];
        }
        for (int y = 1; y <= 2; ++y)
            vec1[y] /= double(recordCount);

        MergeFinishRecord(vec1, vecBest[0]);

        // Iteratively split centroids: 1 -> 2 -> 4 -> 8.
        int exp = 1;
        for (int w = 0; w < 3; )
        {
            vec2 = TVec{ 0.0, -1.0, 0.0 };
            for (int i = 0; i < exp; ++i)
                for (int y = 0; y <= 2; ++y)
                    vecBest[exp + i][y] = (0.01 * vec2[y]) + vecBest[i][y];
            ++w;
            exp = 1 << w;
            FilterRecords(vecBest, exp, records, recordCount);
        }

        // Quantize the 8 pairs to int16 Q11. Sign convention: Nintendo's
        // decoder uses pred = (c1 * yn1 + c2 * yn2 + 1024) >> 11, while the
        // analysis pipeline carries reflection coefs with the opposite sign,
        // so we negate here on the way out.
        for (int z = 0; z < 8; ++z)
        {
            double d;
            d = -vecBest[z][1] * 2048.0;
            if (d > 0.0)
                coefsOut[z * 2]     = (d >  32767.0) ? int16_t( 32767) : int16_t(std::lround(d));
            else
                coefsOut[z * 2]     = (d < -32768.0) ? int16_t(-32768) : int16_t(std::lround(d));

            d = -vecBest[z][2] * 2048.0;
            if (d > 0.0)
                coefsOut[z * 2 + 1] = (d >  32767.0) ? int16_t( 32767) : int16_t(std::lround(d));
            else
                coefsOut[z * 2 + 1] = (d < -32768.0) ? int16_t(-32768) : int16_t(std::lround(d));
        }

        // Final stability sweep. The DSP-ADPCM codec tolerates marginally-
        // stable predictors (Nintendo's table sits exactly on the AR(2)
        // stability boundary — e.g. (4096, -2048)), but a predictor with
        // poles WELL outside the unit circle predicts so aggressively that
        // the 4-bit residual + clamp16 can't bring reconstruction back to
        // the source — audible as Nyquist-rate ringing / screech because
        // the encoder picks "best of bad" and the chosen scale saturates.
        //
        // Bounds match the prior in-house IsAcceptableCoef thresholds and
        // are intentionally generous — anything Nintendo's reference table
        // ever uses passes. Anything that would produce unbounded prediction
        // growth gets replaced with the corresponding kFallbackCoefs slot,
        // which is a known-safe Nintendo predictor.
        int unstableCount = 0;
        for (int z = 0; z < 8; ++z)
        {
            const double c1 = double(coefsOut[z * 2]);
            const double c2 = double(coefsOut[z * 2 + 1]);
            const bool ok =
                std::isfinite(c1) && std::isfinite(c2) &&
                std::fabs(c2) <= 3072.0 &&     // |a2| <= 1.5
                std::fabs(c1) <= 5120.0 &&     // |a1| <= 2.5
                std::fabs(c1) <= 5325.0 - c2;  // triangle, |a1| <= 2.6 - a2
            if (!ok)
            {
                coefsOut[z * 2]     = kFallbackCoefs[z * 2];
                coefsOut[z * 2 + 1] = kFallbackCoefs[z * 2 + 1];
                unstableCount++;
            }
        }
        if (unstableCount > 0)
        {
            LogDebug("DSPCorrelateCoefs: %d of 8 predictors failed stability check, "
                     "replaced with fixed table.", unstableCount);
        }
        // Always log the final coef table for debugging — it's small.
        LogDebug("DSPCorrelateCoefs: coefs = "
                 "[%d,%d %d,%d %d,%d %d,%d %d,%d %d,%d %d,%d %d,%d]",
                 coefsOut[0],  coefsOut[1],  coefsOut[2],  coefsOut[3],
                 coefsOut[4],  coefsOut[5],  coefsOut[6],  coefsOut[7],
                 coefsOut[8],  coefsOut[9],  coefsOut[10], coefsOut[11],
                 coefsOut[12], coefsOut[13], coefsOut[14], coefsOut[15]);
    }

    // Toggled by DspSetUsePerClipCoefs from the cook side. When false,
    // ComputeClipCoefs short-circuits to kFallbackCoefs — used by the cook's
    // round-trip self-test gate.
    bool sUsePerClipCoefs = true;

    void ComputeClipCoefs(const int16_t* samples, uint32_t numSamples,
                          int16_t outCoefs[16])
    {
        // Tiny clips (< 1 ADPCM block × ~5) don't produce enough analysis
        // windows for stable coefficient estimation; fall back to fixed.
        if (!sUsePerClipCoefs || numSamples < 64)
        {
            std::memcpy(outCoefs, kFallbackCoefs, sizeof(kFallbackCoefs));
            return;
        }
        DSPCorrelateCoefs(samples, int(numSamples), outCoefs);
    }

    // Encode one PCM channel with a specified coefficient table. Same body as
    // the prior DspEncode — just parameterized on the coefs so we can call it
    // twice (once with clip-tuned, once with fallback) and keep whichever
    // produces lower reconstruction error. The dual-encode strategy is the
    // safety net against per-clip predictor analysis producing predictors
    // that look stable on paper but match the source content worse than
    // Nintendo's fixed table.
    void EncodeChannelWithCoefs(
        const int16_t* coefs,
        const int16_t* inSamples,
        uint32_t numSamples,
        uint32_t sampleRate,
        DspChannelHeader& outHeader,
        std::vector<uint8_t>& outAdpcm)
    {
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

            int32_t emitYn1 = 0, emitYn2 = 0;
            EncodeBlockTrial(
                inSamples + sampleStart, numIn,
                coefs, bestPred, bestScale, yn1, yn2,
                &emitYn1, &emitYn2,
                outAdpcm.data() + b * 8);

            if (b == 0)
                firstPs = uint16_t(outAdpcm[0]);

            yn1 = bestYn1;
            yn2 = bestYn2;
        }

        std::memset(&outHeader, 0, sizeof(outHeader));
        outHeader.numSamples       = numSamples;
        outHeader.numAdpcmNibbles  = numBlocks * 16;
        outHeader.sampleRate       = sampleRate;
        outHeader.loopFlag         = 0;
        outHeader.format           = 0;
        outHeader.loopStartNibble  = 2;
        outHeader.loopEndNibble    = (numBlocks * 16) - 1;
        outHeader.currentNibble    = 2;
        std::memcpy(outHeader.coef, coefs, 16 * sizeof(int16_t));
        outHeader.gain    = 0;
        outHeader.ps      = firstPs;
        outHeader.yn1     = 0;
        outHeader.yn2     = 0;
        outHeader.loopPs  = 0;
        outHeader.loopYn1 = 0;
        outHeader.loopYn2 = 0;
        outHeader.pad     = 0;
    }

    // RMS of (orig - decoded(encode(orig))) / 32768.0, fraction of full-scale.
    // Used by the per-clip dual-encode comparison to pick the better of two
    // candidate encodings.
    double EncodedRMS(const DspChannelHeader& header,
                      const std::vector<uint8_t>& adpcm,
                      const int16_t* orig, uint32_t numSamples)
    {
        std::vector<int16_t> recon(numSamples, int16_t(0));
        DspDecode(header, adpcm.data(), uint32_t(adpcm.size()),
                  recon.data(), numSamples);
        double sumSq = 0.0;
        for (uint32_t i = 0; i < numSamples; ++i)
        {
            const double e = double(orig[i]) - double(recon[i]);
            sumSq += e * e;
        }
        return std::sqrt(sumSq / double(numSamples)) / 32768.0;
    }
} // namespace

void DspEncode(
    const int16_t* inSamples,
    uint32_t numSamples,
    uint32_t sampleRate,
    DspChannelHeader& outHeader,
    std::vector<uint8_t>& outAdpcm)
{
    // Per-clip dual-encode safety: encode with clip-tuned coefs AND with
    // Nintendo's fixed table, keep whichever has lower reconstruction RMS.
    //
    // Why: jackoalan's DSPCorrelateCoefs algorithm produces predictors that
    // are LPC-optimal but not necessarily ADPCM-optimal — the 4-bit residual
    // quantization plus per-block scale shift can interact badly with some
    // clip-tuned predictors, producing audible Nyquist-rate ringing or
    // modulation artifacts on real content even though the synthetic gate
    // passes. The fixed Nintendo table is the proven baseline; if our
    // analysis can't beat it for a given clip, we ship the baseline.
    //
    // Cost: roughly 2x channel encode time. The encode is a tiny fraction
    // of total cook time (ffmpeg dominates), so this is negligible.

    int16_t clipCoefs[16];
    ComputeClipCoefs(inSamples, numSamples, clipCoefs);

    // First encode: clip-tuned coefs.
    EncodeChannelWithCoefs(clipCoefs, inSamples, numSamples, sampleRate,
                           outHeader, outAdpcm);

    // If ComputeClipCoefs short-circuited to kFallbackCoefs (silence / tiny
    // clip / per-clip analysis disabled), the second encode would be
    // identical — skip it.
    const bool clipIsFallback =
        std::memcmp(clipCoefs, kFallbackCoefs, sizeof(kFallbackCoefs)) == 0;
    if (clipIsFallback || numSamples == 0)
        return;

    const double clipRms = EncodedRMS(outHeader, outAdpcm, inSamples, numSamples);

    // Second encode: fixed Nintendo coefs, into temporary buffers.
    DspChannelHeader     fbHeader{};
    std::vector<uint8_t> fbAdpcm;
    EncodeChannelWithCoefs(kFallbackCoefs, inSamples, numSamples, sampleRate,
                           fbHeader, fbAdpcm);
    const double fbRms = EncodedRMS(fbHeader, fbAdpcm, inSamples, numSamples);

    if (fbRms < clipRms)
    {
        // Fixed table is better for this clip. Adopt it.
        LogWarning("DspEncode: clip-tuned RMS=%.4f worse than fixed RMS=%.4f; "
                   "shipping fixed coefs for this clip.", clipRms, fbRms);
        outHeader = fbHeader;
        outAdpcm  = std::move(fbAdpcm);
    }
    else
    {
        LogDebug("DspEncode: clip-tuned RMS=%.4f beats fixed RMS=%.4f; "
                 "shipping clip-tuned coefs.", clipRms, fbRms);
    }
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
#if PLATFORM_DOLPHIN
    // PowerPC / Wii / GameCube is big-endian — host byte order IS wire byte
    // order. The cook side never runs on these hosts (cook is editor-only,
    // editor is x86/ARM-LE), but the runtime ALSO calls this helper via
    // DspHeaderFromBE on bytes read from disk; that path needs a no-op here.
    // Without this guard, the round-trip on a BE host swaps once at write
    // (LE-host cook) and once on read (BE-host runtime), corrupting every
    // multi-byte field — most visibly the coef[] table, which is the only
    // field the runtime actually reads from this struct (numSamples / yn1 /
    // yn2 are overridden by the per-frame caller, and sampleRate is read
    // separately via ReadBE32At). With kFallbackCoefs the corruption maps
    // (4096, -2048) → (16, 248) etc. — effectively zero prediction, which
    // sounds like coarse 4-bit PCM (the "thin / distorted" THP baseline).
    // With clip-tuned coefs the corruption produces wildly out-of-range
    // values that blow up reconstruction — audible Nyquist screech.
    return h;
#else
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
#endif
}

DspChannelHeader DspHeaderFromBE(const DspChannelHeader& h)
{
    return DspHeaderToBE(h); // byte-swap (or no-op on BE host) is its own inverse
}

void DspSetUsePerClipCoefs(bool enabled)
{
    sUsePerClipCoefs = enabled;
}

double DspEncodeRoundTripRMS()
{
    constexpr uint32_t kRate    = 22050;
    constexpr uint32_t kSamples = 22050; // 1 second
    constexpr double   kPi      = 3.14159265358979323846;

    // Synthetic dual-tone signal: 1 kHz sine + 100 Hz square. Mixed bandwidth
    // is exactly the failure mode kFallbackCoefs exhibits on real content, so
    // a passing round-trip on this signal proves the per-clip table is doing
    // useful work.
    std::vector<int16_t> orig(kSamples);
    for (uint32_t i = 0; i < kSamples; ++i)
    {
        const double t       = double(i) / double(kRate);
        const double sineVal = 0.4 * std::sin(2.0 * kPi * 1000.0 * t);
        const double sqPhase = std::fmod(2.0 * kPi * 100.0 * t, 2.0 * kPi);
        const double sqVal   = (sqPhase < kPi) ? 0.3 : -0.3;
        const double mix     = sineVal + sqVal;
        const double clamped = std::max(-1.0, std::min(1.0, mix));
        orig[i] = int16_t(std::lround(clamped * 32767.0));
    }

    DspChannelHeader     hdr{};
    std::vector<uint8_t> adpcm;
    DspEncode(orig.data(), kSamples, kRate, hdr, adpcm);

    std::vector<int16_t> recon(kSamples, int16_t(0));
    DspDecode(hdr, adpcm.data(), uint32_t(adpcm.size()),
              recon.data(), kSamples);

    double sumSq = 0.0;
    for (uint32_t i = 0; i < kSamples; ++i)
    {
        const double e = double(orig[i]) - double(recon[i]);
        sumSq += e * e;
    }
    return std::sqrt(sumSq / double(kSamples)) / 32768.0;
}

} // namespace VideoPlayerAddon
