#include "hdclaude/core/texture_compression.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hdclaude {
namespace {

/// BC6H's and BC7's 4-bit interpolation weights, in 64ths (both formats share
/// them).
constexpr int kWeights4[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                               34, 38, 43, 47, 51, 55, 60, 64};

/// Writes fields least significant bit first, as every BC format packs them.
struct BitWriter {
    std::uint8_t* out;
    unsigned position = 0;

    void Put(std::uint32_t value, unsigned bits)
    {
        for (unsigned i = 0; i < bits; ++i, ++position) {
            if ((value >> i) & 1u) {
                out[position >> 3] |= static_cast<std::uint8_t>(1u << (position & 7u));
            }
        }
    }
};

/// The direction along which `count` points of `dims` components vary most,
/// and their mean. Power iteration on the covariance, started from the
/// bounding box's diagonal, which converges in a handful of steps for the
/// near-linear colour sets a 4x4 block usually holds.
void PrincipalAxis(const float* points, int count, int dims, float mean[4],
                   float axis[4])
{
    for (int c = 0; c < dims; ++c) {
        mean[c] = 0.0f;
        for (int i = 0; i < count; ++i) {
            mean[c] += points[i * dims + c];
        }
        mean[c] /= static_cast<float>(count);
    }
    float covariance[4][4] = {};
    float low[4], high[4];
    for (int c = 0; c < dims; ++c) {
        low[c] = high[c] = points[c];
    }
    for (int i = 0; i < count; ++i) {
        for (int a = 0; a < dims; ++a) {
            const float da = points[i * dims + a] - mean[a];
            low[a] = std::min(low[a], points[i * dims + a]);
            high[a] = std::max(high[a], points[i * dims + a]);
            for (int b = 0; b < dims; ++b) {
                covariance[a][b] += da * (points[i * dims + b] - mean[b]);
            }
        }
    }
    float vector[4];
    for (int c = 0; c < dims; ++c) {
        vector[c] = high[c] - low[c];
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        float next[4] = {};
        for (int a = 0; a < dims; ++a) {
            for (int b = 0; b < dims; ++b) {
                next[a] += covariance[a][b] * vector[b];
            }
        }
        float length = 0.0f;
        for (int c = 0; c < dims; ++c) {
            length += next[c] * next[c];
        }
        if (!(length > 1e-20f)) {
            break;
        }
        length = std::sqrt(length);
        for (int c = 0; c < dims; ++c) {
            vector[c] = next[c] / length;
        }
    }
    float length = 0.0f;
    for (int c = 0; c < dims; ++c) {
        length += vector[c] * vector[c];
    }
    length = std::sqrt(length);
    for (int c = 0; c < dims; ++c) {
        axis[c] = length > 0.0f ? vector[c] / length : (c == 0 ? 1.0f : 0.0f);
    }
}

/// Endpoints spanning the points' extent along the principal axis.
void AxisEndpoints(const float* points, int count, int dims, float e0[4],
                   float e1[4])
{
    float mean[4], axis[4];
    PrincipalAxis(points, count, dims, mean, axis);
    float lowest = 0.0f, highest = 0.0f;
    for (int i = 0; i < count; ++i) {
        float t = 0.0f;
        for (int c = 0; c < dims; ++c) {
            t += (points[i * dims + c] - mean[c]) * axis[c];
        }
        lowest = i == 0 ? t : std::min(lowest, t);
        highest = i == 0 ? t : std::max(highest, t);
    }
    for (int c = 0; c < dims; ++c) {
        e0[c] = mean[c] + axis[c] * lowest;
        e1[c] = mean[c] + axis[c] * highest;
    }
}

/// The endpoints that, for the weights the indices chose, minimise the squared
/// error: the normal equations of sum |(1 - w) e0 + w e1 - x|^2. Leaves the
/// endpoints alone when every texel chose the same weight.
void RefitEndpoints(const float* points, const float* weights, int count, int dims,
                    float e0[4], float e1[4])
{
    double a = 0.0, b = 0.0, c = 0.0;
    double d0[4] = {}, d1[4] = {};
    for (int i = 0; i < count; ++i) {
        const double w = weights[i];
        a += (1.0 - w) * (1.0 - w);
        b += (1.0 - w) * w;
        c += w * w;
        for (int k = 0; k < dims; ++k) {
            d0[k] += (1.0 - w) * points[i * dims + k];
            d1[k] += w * points[i * dims + k];
        }
    }
    const double determinant = a * c - b * b;
    if (std::abs(determinant) < 1e-9) {
        return;
    }
    for (int k = 0; k < dims; ++k) {
        e0[k] = static_cast<float>((c * d0[k] - b * d1[k]) / determinant);
        e1[k] = static_cast<float>((a * d1[k] - b * d0[k]) / determinant);
    }
}

// --- BC4 -----------------------------------------------------------------------

/// The eight values a BC4 block with these endpoints decodes to.
void Bc4Palette(int r0, int r1, float palette[8])
{
    palette[0] = static_cast<float>(r0);
    palette[1] = static_cast<float>(r1);
    if (r0 > r1) {
        for (int k = 1; k <= 6; ++k) {
            palette[k + 1] = static_cast<float>((7 - k) * r0 + k * r1) / 7.0f;
        }
    } else {
        for (int k = 1; k <= 4; ++k) {
            palette[k + 1] = static_cast<float>((5 - k) * r0 + k * r1) / 5.0f;
        }
        palette[6] = 0.0f;
        palette[7] = 255.0f;
    }
}

float Bc4Indices(const std::uint8_t values[16], int r0, int r1, int indices[16])
{
    float palette[8];
    Bc4Palette(r0, r1, palette);
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        for (int k = 0; k < 8; ++k) {
            const float d = static_cast<float>(values[i]) - palette[k];
            if (d * d < best) {
                best = d * d;
                indices[i] = k;
            }
        }
        error += best;
    }
    return error;
}

}  // namespace

void EncodeBc4Block(const std::uint8_t values[16], std::uint8_t block[8])
{
    int low = 255, high = 0;
    int innerLow = 255, innerHigh = 0;
    for (int i = 0; i < 16; ++i) {
        low = std::min<int>(low, values[i]);
        high = std::max<int>(high, values[i]);
        if (values[i] != 0 && values[i] != 255) {
            innerLow = std::min<int>(innerLow, values[i]);
            innerHigh = std::max<int>(innerHigh, values[i]);
        }
    }

    int bestR0 = high, bestR1 = low;
    int bestIndices[16] = {};
    float bestError = 1e30f;
    const auto consider = [&](int r0, int r1) {
        int indices[16];
        const float error = Bc4Indices(values, r0, r1, indices);
        if (error < bestError) {
            bestError = error;
            bestR0 = r0;
            bestR1 = r1;
            std::memcpy(bestIndices, indices, sizeof(indices));
        }
    };
    // Eight interpolated levels across the block's range, the endpoints
    // nudged inward a little, since the extremes are rarely where the error is.
    for (int inset0 = 0; inset0 <= 2; ++inset0) {
        for (int inset1 = 0; inset1 <= 2; ++inset1) {
            const int r0 = high - inset0;
            const int r1 = low + inset1;
            if (r0 > r1) {
                consider(r0, r1);
            }
        }
    }
    if (high == low) {
        consider(high, high);
    }
    // Six levels and exact 0 and 255, for a block that holds the extremes and
    // something between them -- the edge of a mask.
    if (innerLow <= innerHigh && (low == 0 || high == 255)) {
        consider(innerLow, innerHigh);
    } else if (innerLow > innerHigh) {
        consider(0, 0);   // only 0s and 255s
    }

    std::memset(block, 0, 8);
    BitWriter bits{block};
    bits.Put(static_cast<std::uint32_t>(bestR0), 8);
    bits.Put(static_cast<std::uint32_t>(bestR1), 8);
    for (int i = 0; i < 16; ++i) {
        bits.Put(static_cast<std::uint32_t>(bestIndices[i]), 3);
    }
}

void EncodeBc5Block(const std::uint8_t values[32], std::uint8_t block[16])
{
    std::uint8_t red[16], green[16];
    for (int i = 0; i < 16; ++i) {
        red[i] = values[i * 2];
        green[i] = values[i * 2 + 1];
    }
    EncodeBc4Block(red, block);
    EncodeBc4Block(green, block + 8);
}

// --- BC7, mode 6 ---------------------------------------------------------------

namespace {

/// An endpoint as mode 6 stores it: seven bits a channel and one shared p-bit,
/// and the byte values that decode to.
struct Bc7Endpoint {
    int code[4];
    int pbit;
    int value[4];
};

/// The nearest storable endpoint. `forcedPbit` fixes the p-bit (an opaque
/// block must keep alpha 255, which only p = 1 reaches), or -1 to choose it.
Bc7Endpoint QuantiseBc7(const float e[4], int forcedPbit)
{
    Bc7Endpoint best{};
    float bestError = 1e30f;
    for (int p = 0; p <= 1; ++p) {
        if (forcedPbit >= 0 && p != forcedPbit) {
            continue;
        }
        Bc7Endpoint candidate{};
        candidate.pbit = p;
        float error = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const int code = std::clamp(
                static_cast<int>(std::lround((e[c] - static_cast<float>(p)) * 0.5f)), 0,
                127);
            candidate.code[c] = code;
            candidate.value[c] = (code << 1) | p;
            const float d = static_cast<float>(candidate.value[c]) - e[c];
            error += d * d;
        }
        if (error < bestError) {
            bestError = error;
            best = candidate;
        }
    }
    return best;
}

float Bc7Indices(const float* points, const Bc7Endpoint& e0, const Bc7Endpoint& e1,
                 int indices[16])
{
    float palette[16][4];
    for (int k = 0; k < 16; ++k) {
        for (int c = 0; c < 4; ++c) {
            palette[k][c] = static_cast<float>(
                ((64 - kWeights4[k]) * e0.value[c] + kWeights4[k] * e1.value[c] + 32) >> 6);
        }
    }
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        for (int k = 0; k < 16; ++k) {
            float d = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const float delta = points[i * 4 + c] - palette[k][c];
                d += delta * delta;
            }
            if (d < best) {
                best = d;
                indices[i] = k;
            }
        }
        error += best;
    }
    return error;
}

}  // namespace

namespace {

/// BC7's 2-bit interpolation weights, in 64ths.
constexpr int kWeights2[4] = {0, 21, 43, 64};

/// A mode 5 colour endpoint: seven bits a channel, widened to eight by
/// repeating the top bit.
int ExpandBc7Seven(int code) { return (code << 1) | (code >> 6); }

float Bc7Mode5ColourIndices(const float* rgb, const int c0[3], const int c1[3],
                            int indices[16])
{
    float palette[4][3];
    for (int k = 0; k < 4; ++k) {
        for (int c = 0; c < 3; ++c) {
            palette[k][c] = static_cast<float>(
                ((64 - kWeights2[k]) * ExpandBc7Seven(c0[c]) +
                 kWeights2[k] * ExpandBc7Seven(c1[c]) + 32) >> 6);
        }
    }
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        for (int k = 0; k < 4; ++k) {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float delta = rgb[i * 3 + c] - palette[k][c];
                d += delta * delta;
            }
            if (d < best) {
                best = d;
                indices[i] = k;
            }
        }
        error += best;
    }
    return error;
}

float Bc7Mode5AlphaIndices(const float* alpha, int a0, int a1, int indices[16])
{
    float palette[4];
    for (int k = 0; k < 4; ++k) {
        palette[k] =
            static_cast<float>(((64 - kWeights2[k]) * a0 + kWeights2[k] * a1 + 32) >> 6);
    }
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        for (int k = 0; k < 4; ++k) {
            const float d = (alpha[i] - palette[k]) * (alpha[i] - palette[k]);
            if (d < best) {
                best = d;
                indices[i] = k;
            }
        }
        error += best;
    }
    return error;
}

/// Mode 5: colour and alpha as two separate lines, the colour with seven-bit
/// endpoints and 2-bit indices, the alpha with eight-bit endpoints and its own
/// 2-bit indices. What a block whose alpha does not follow its colour -- an
/// opacity mask in a colour map's alpha -- needs, and mode 6, one line through
/// all four channels, cannot give. Returns the squared error.
float EncodeBc7Mode5(const std::uint8_t rgba[64], std::uint8_t block[16])
{
    float rgb[48], alpha[16];
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            rgb[i * 3 + c] = static_cast<float>(rgba[i * 4 + c]);
        }
        alpha[i] = static_cast<float>(rgba[i * 4 + 3]);
    }

    // Colour.
    float e0[4], e1[4];
    AxisEndpoints(rgb, 16, 3, e0, e1);
    const auto quantise = [](const float e[4], int code[3]) {
        for (int c = 0; c < 3; ++c) {
            code[c] = std::clamp(
                static_cast<int>(std::lround(std::clamp(e[c], 0.0f, 255.0f) * 127.0f /
                                             255.0f)),
                0, 127);
        }
    };
    int c0[3], c1[3];
    quantise(e0, c0);
    quantise(e1, c1);
    int colourIndices[16];
    float colourError = Bc7Mode5ColourIndices(rgb, c0, c1, colourIndices);
    {
        float weights[16];
        for (int i = 0; i < 16; ++i) {
            weights[i] = static_cast<float>(kWeights2[colourIndices[i]]) / 64.0f;
        }
        float r0[4] = {e0[0], e0[1], e0[2], 0.0f};
        float r1[4] = {e1[0], e1[1], e1[2], 0.0f};
        RefitEndpoints(rgb, weights, 16, 3, r0, r1);
        int t0[3], t1[3];
        quantise(r0, t0);
        quantise(r1, t1);
        int trial[16];
        const float trialError = Bc7Mode5ColourIndices(rgb, t0, t1, trial);
        if (trialError < colourError) {
            colourError = trialError;
            std::memcpy(c0, t0, sizeof(t0));
            std::memcpy(c1, t1, sizeof(t1));
            std::memcpy(colourIndices, trial, sizeof(trial));
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        bool improved = false;
        for (int which = 0; which < 2; ++which) {
            for (int c = 0; c < 3; ++c) {
                for (const int step : {-1, 1}) {
                    int t0[3] = {c0[0], c0[1], c0[2]};
                    int t1[3] = {c1[0], c1[1], c1[2]};
                    int* t = which == 0 ? t0 : t1;
                    if (t[c] + step < 0 || t[c] + step > 127) {
                        continue;
                    }
                    t[c] += step;
                    int trial[16];
                    const float trialError = Bc7Mode5ColourIndices(rgb, t0, t1, trial);
                    if (trialError < colourError) {
                        colourError = trialError;
                        std::memcpy(c0, t0, sizeof(t0));
                        std::memcpy(c1, t1, sizeof(t1));
                        std::memcpy(colourIndices, trial, sizeof(trial));
                        improved = true;
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    // Alpha: the block's range, then each end nudged.
    int a0 = 255, a1 = 0;
    for (int i = 0; i < 16; ++i) {
        a0 = std::min(a0, static_cast<int>(rgba[i * 4 + 3]));
        a1 = std::max(a1, static_cast<int>(rgba[i * 4 + 3]));
    }
    int alphaIndices[16];
    float alphaError = Bc7Mode5AlphaIndices(alpha, a0, a1, alphaIndices);
    for (int pass = 0; pass < 4; ++pass) {
        bool improved = false;
        for (int which = 0; which < 2; ++which) {
            for (const int step : {-1, 1}) {
                int t0 = a0, t1 = a1;
                int& t = which == 0 ? t0 : t1;
                if (t + step < 0 || t + step > 255) {
                    continue;
                }
                t += step;
                int trial[16];
                const float trialError = Bc7Mode5AlphaIndices(alpha, t0, t1, trial);
                if (trialError < alphaError) {
                    alphaError = trialError;
                    a0 = t0;
                    a1 = t1;
                    std::memcpy(alphaIndices, trial, sizeof(trial));
                    improved = true;
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    // Each first index is stored with its top bit implied zero.
    if (colourIndices[0] >= 2) {
        for (int c = 0; c < 3; ++c) {
            std::swap(c0[c], c1[c]);
        }
        for (int& index : colourIndices) {
            index = 3 - index;
        }
    }
    if (alphaIndices[0] >= 2) {
        std::swap(a0, a1);
        for (int& index : alphaIndices) {
            index = 3 - index;
        }
    }

    std::memset(block, 0, 16);
    BitWriter bits{block};
    bits.Put(1u << 5, 6);   // mode 5
    bits.Put(0, 2);         // no channel rotation
    for (int c = 0; c < 3; ++c) {
        bits.Put(static_cast<std::uint32_t>(c0[c]), 7);
        bits.Put(static_cast<std::uint32_t>(c1[c]), 7);
    }
    bits.Put(static_cast<std::uint32_t>(a0), 8);
    bits.Put(static_cast<std::uint32_t>(a1), 8);
    bits.Put(static_cast<std::uint32_t>(colourIndices[0]), 1);
    for (int i = 1; i < 16; ++i) {
        bits.Put(static_cast<std::uint32_t>(colourIndices[i]), 2);
    }
    bits.Put(static_cast<std::uint32_t>(alphaIndices[0]), 1);
    for (int i = 1; i < 16; ++i) {
        bits.Put(static_cast<std::uint32_t>(alphaIndices[i]), 2);
    }
    return colourError + alphaError;
}

/// Mode 6: one line through all four channels, seven-bit endpoints and a
/// p-bit each, 4-bit indices. Returns the squared error.
float EncodeBc7Mode6(const std::uint8_t rgba[64], std::uint8_t block[16])
{
    float points[64];
    bool opaque = true;
    bool transparent = true;
    for (int i = 0; i < 64; ++i) {
        points[i] = static_cast<float>(rgba[i]);
    }
    for (int i = 0; i < 16; ++i) {
        opaque = opaque && rgba[i * 4 + 3] == 255;
        transparent = transparent && rgba[i * 4 + 3] == 0;
    }
    // A block every texel of which is opaque, or transparent, stays exactly
    // so: an alpha of 254 is a surface that lets a little light through.
    const int forcedPbit = opaque ? 1 : (transparent ? 0 : -1);

    float e0[4], e1[4];
    AxisEndpoints(points, 16, 4, e0, e1);
    for (int c = 0; c < 4; ++c) {
        e0[c] = std::clamp(e0[c], 0.0f, 255.0f);
        e1[c] = std::clamp(e1[c], 0.0f, 255.0f);
    }
    Bc7Endpoint q0 = QuantiseBc7(e0, forcedPbit);
    Bc7Endpoint q1 = QuantiseBc7(e1, forcedPbit);
    int indices[16];
    float error = Bc7Indices(points, q0, q1, indices);

    for (int iteration = 0; iteration < 2; ++iteration) {
        float weights[16];
        for (int i = 0; i < 16; ++i) {
            weights[i] = static_cast<float>(kWeights4[indices[i]]) / 64.0f;
        }
        float r0[4] = {e0[0], e0[1], e0[2], e0[3]};
        float r1[4] = {e1[0], e1[1], e1[2], e1[3]};
        RefitEndpoints(points, weights, 16, 4, r0, r1);
        for (int c = 0; c < 4; ++c) {
            r0[c] = std::clamp(r0[c], 0.0f, 255.0f);
            r1[c] = std::clamp(r1[c], 0.0f, 255.0f);
        }
        const Bc7Endpoint t0 = QuantiseBc7(r0, forcedPbit);
        const Bc7Endpoint t1 = QuantiseBc7(r1, forcedPbit);
        int trial[16];
        const float trialError = Bc7Indices(points, t0, t1, trial);
        if (!(trialError < error)) {
            break;
        }
        error = trialError;
        q0 = t0;
        q1 = t1;
        std::memcpy(indices, trial, sizeof(trial));
        std::memcpy(e0, r0, sizeof(r0));
        std::memcpy(e1, r1, sizeof(r1));
    }

    // Then each stored code nudged by one, kept when the block improves: the
    // refit is optimal for continuous endpoints, and rounding each channel to
    // seven bits independently is not optimal for the block.
    for (int pass = 0; pass < 2; ++pass) {
        bool improved = false;
        for (int which = 0; which < 2; ++which) {
            for (int c = 0; c < 4; ++c) {
                for (const int step : {-1, 1}) {
                    Bc7Endpoint t0 = q0, t1 = q1;
                    Bc7Endpoint& t = which == 0 ? t0 : t1;
                    const int code = t.code[c] + step;
                    if (code < 0 || code > 127) {
                        continue;
                    }
                    t.code[c] = code;
                    t.value[c] = (code << 1) | t.pbit;
                    int trial[16];
                    const float trialError = Bc7Indices(points, t0, t1, trial);
                    if (trialError < error) {
                        error = trialError;
                        q0 = t0;
                        q1 = t1;
                        std::memcpy(indices, trial, sizeof(trial));
                        improved = true;
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    // The first texel's index is stored with its top bit implied zero.
    if (indices[0] >= 8) {
        std::swap(q0, q1);
        for (int& index : indices) {
            index = 15 - index;
        }
    }

    std::memset(block, 0, 16);
    BitWriter bits{block};
    bits.Put(1u << 6, 7);   // mode 6
    for (int c = 0; c < 4; ++c) {
        bits.Put(static_cast<std::uint32_t>(q0.code[c]), 7);
        bits.Put(static_cast<std::uint32_t>(q1.code[c]), 7);
    }
    bits.Put(static_cast<std::uint32_t>(q0.pbit), 1);
    bits.Put(static_cast<std::uint32_t>(q1.pbit), 1);
    bits.Put(static_cast<std::uint32_t>(indices[0]), 3);
    for (int i = 1; i < 16; ++i) {
        bits.Put(static_cast<std::uint32_t>(indices[i]), 4);
    }
    return error;
}

}  // namespace

void EncodeBc7Block(const std::uint8_t rgba[64], std::uint8_t block[16])
{
    // Both modes, and whichever decodes nearer: mode 6 for a block whose
    // channels move together, which is most of them, mode 5 for one whose
    // alpha goes its own way.
    std::uint8_t six[16], five[16];
    const float sixError = EncodeBc7Mode6(rgba, six);
    const float fiveError = EncodeBc7Mode5(rgba, five);
    std::memcpy(block, fiveError < sixError ? five : six, 16);
}

// --- BC6H, mode 11, unsigned and signed ---------------------------------------

namespace {

/// Mode 11's endpoint, stored in ten bits, as the decoder widens it to the
/// sixteen it interpolates in (unquantize). Signed codes are ten-bit two's
/// complement, and keep one bit fewer of magnitude.
int Bc6hUnquantise(int code, bool isSigned)
{
    if (!isSigned) {
        if (code == 0) {
            return 0;
        }
        if (code == 1023) {
            return 0xFFFF;
        }
        return ((code << 16) + 0x8000) >> 10;
    }
    const bool negative = code < 0;
    const int magnitude = negative ? -code : code;
    int widened = 0;
    if (magnitude == 0) {
        widened = 0;
    } else if (magnitude >= 511) {
        widened = 0x7FFF;
    } else {
        widened = ((magnitude << 15) + 0x4000) >> 9;
    }
    return negative ? -widened : widened;
}

/// What a texel decodes to, as a signed integer: the half bit pattern for an
/// unsigned block, and the pattern's magnitude carrying the value's sign for a
/// signed one -- the line along which the format interpolates.
int Bc6hDecode(int unquantised0, int unquantised1, int index, bool isSigned)
{
    const int interpolated =
        (unquantised0 * (64 - kWeights4[index]) + unquantised1 * kWeights4[index] + 32) >>
        6;
    if (!isSigned) {
        return (interpolated * 31) >> 6;
    }
    return interpolated < 0 ? -(((-interpolated) * 31) >> 5) : (interpolated * 31) >> 5;
}

float Bc6hIndices(const float* targets, const int code0[3], const int code1[3],
                  bool isSigned, int indices[16])
{
    int u0[3], u1[3];
    for (int c = 0; c < 3; ++c) {
        u0[c] = Bc6hUnquantise(code0[c], isSigned);
        u1[c] = Bc6hUnquantise(code1[c], isSigned);
    }
    float palette[16][3];
    for (int k = 0; k < 16; ++k) {
        for (int c = 0; c < 3; ++c) {
            palette[k][c] = static_cast<float>(Bc6hDecode(u0[c], u1[c], k, isSigned));
        }
    }
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        for (int k = 0; k < 16; ++k) {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float delta = targets[i * 3 + c] - palette[k][c];
                d += delta * delta;
            }
            if (d < best) {
                best = d;
                indices[i] = k;
            }
        }
        error += best;
    }
    return error;
}

/// An endpoint in the decoder's interpolation space as the nearest ten-bit
/// code: unquantize is close to 64 code + 32 in both forms.
void QuantiseBc6h(const float e[3], bool isSigned, int code[3])
{
    for (int c = 0; c < 3; ++c) {
        if (!isSigned) {
            code[c] = std::clamp(static_cast<int>(std::lround((e[c] - 32.0f) / 64.0f)), 0,
                                 1023);
        } else {
            const float magnitude = std::abs(e[c]);
            const int stored = std::clamp(
                static_cast<int>(std::lround((magnitude - 32.0f) / 64.0f)), 0, 511);
            code[c] = e[c] < 0.0f ? -stored : stored;
        }
    }
}

}  // namespace

void EncodeBc6hBlock(const std::uint16_t rgb[48], std::uint8_t block[16],
                     bool isSigned)
{
    // Fitted in the decoder's interpolation space, where its arithmetic is
    // linear; the error is measured on the half bit patterns it decodes to,
    // which is close to a relative error in the value -- what an HDR texel's
    // precision is about. For a signed block the pattern's magnitude carries
    // the value's sign, which is the line the signed format interpolates on.
    float targets[48];
    float points[48];
    const float scale = isSigned ? 32.0f / 31.0f : 64.0f / 31.0f;
    for (int i = 0; i < 48; ++i) {
        const int magnitude = std::min<int>(rgb[i] & 0x7FFF, 0x7BFF);
        const bool negative = isSigned && (rgb[i] & 0x8000) != 0;
        targets[i] = static_cast<float>(negative ? -magnitude : magnitude);
        points[i] = targets[i] * scale;
    }

    float e0[4], e1[4];
    AxisEndpoints(points, 16, 3, e0, e1);
    int code0[3], code1[3];
    QuantiseBc6h(e0, isSigned, code0);
    QuantiseBc6h(e1, isSigned, code1);
    int indices[16];
    float error = Bc6hIndices(targets, code0, code1, isSigned, indices);

    for (int iteration = 0; iteration < 2; ++iteration) {
        float weights[16];
        for (int i = 0; i < 16; ++i) {
            weights[i] = static_cast<float>(kWeights4[indices[i]]) / 64.0f;
        }
        float r0[4] = {e0[0], e0[1], e0[2], 0.0f};
        float r1[4] = {e1[0], e1[1], e1[2], 0.0f};
        RefitEndpoints(points, weights, 16, 3, r0, r1);
        int t0[3], t1[3];
        QuantiseBc6h(r0, isSigned, t0);
        QuantiseBc6h(r1, isSigned, t1);
        int trial[16];
        const float trialError = Bc6hIndices(targets, t0, t1, isSigned, trial);
        if (!(trialError < error)) {
            break;
        }
        error = trialError;
        std::memcpy(code0, t0, sizeof(t0));
        std::memcpy(code1, t1, sizeof(t1));
        std::memcpy(indices, trial, sizeof(trial));
        std::memcpy(e0, r0, sizeof(r0));
        std::memcpy(e1, r1, sizeof(r1));
    }

    const int lowest = isSigned ? -511 : 0;
    const int highest = isSigned ? 511 : 1023;
    for (int pass = 0; pass < 2; ++pass) {
        bool improved = false;
        for (int which = 0; which < 2; ++which) {
            for (int c = 0; c < 3; ++c) {
                for (const int step : {-1, 1}) {
                    int t0[3] = {code0[0], code0[1], code0[2]};
                    int t1[3] = {code1[0], code1[1], code1[2]};
                    int* t = which == 0 ? t0 : t1;
                    if (t[c] + step < lowest || t[c] + step > highest) {
                        continue;
                    }
                    t[c] += step;
                    int trial[16];
                    const float trialError = Bc6hIndices(targets, t0, t1, isSigned, trial);
                    if (trialError < error) {
                        error = trialError;
                        std::memcpy(code0, t0, sizeof(t0));
                        std::memcpy(code1, t1, sizeof(t1));
                        std::memcpy(indices, trial, sizeof(trial));
                        improved = true;
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }

    if (indices[0] >= 8) {
        for (int c = 0; c < 3; ++c) {
            std::swap(code0[c], code1[c]);
        }
        for (int& index : indices) {
            index = 15 - index;
        }
    }

    std::memset(block, 0, 16);
    BitWriter bits{block};
    bits.Put(0x03, 5);   // mode 11: ten-bit endpoints, one region, no transform
    for (int c = 0; c < 3; ++c) {
        bits.Put(static_cast<std::uint32_t>(code0[c]) & 0x3FFu, 10);
    }
    for (int c = 0; c < 3; ++c) {
        bits.Put(static_cast<std::uint32_t>(code1[c]) & 0x3FFu, 10);
    }
    bits.Put(static_cast<std::uint32_t>(indices[0]), 3);
    for (int i = 1; i < 16; ++i) {
        bits.Put(static_cast<std::uint32_t>(indices[i]), 4);
    }
}

std::vector<std::uint8_t> EncodeBlocks(BlockEncoding encoding, const void* texels,
                                       std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t blocksWide = (width + 3) / 4;
    const std::uint32_t blocksHigh = (height + 3) / 4;
    const std::uint32_t blockBytes = BlockBytes(encoding);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(blocksWide) * blocksHigh *
                                  blockBytes);
    const auto* bytes = static_cast<const std::uint8_t*>(texels);
    const auto* halves = static_cast<const std::uint16_t*>(texels);
    const bool halfFloat =
        encoding == BlockEncoding::Bc6h || encoding == BlockEncoding::Bc6hSigned;
    const int channels = encoding == BlockEncoding::Bc4   ? 1
                         : encoding == BlockEncoding::Bc5 ? 2
                         : halfFloat                      ? 3
                                                          : 4;

    for (std::uint32_t by = 0; by < blocksHigh; ++by) {
        for (std::uint32_t bx = 0; bx < blocksWide; ++bx) {
            std::uint8_t gathered8[64];
            std::uint16_t gathered16[48];
            for (std::uint32_t row = 0; row < 4; ++row) {
                const std::uint32_t y = std::min(by * 4 + row, height - 1);
                for (std::uint32_t column = 0; column < 4; ++column) {
                    const std::uint32_t x = std::min(bx * 4 + column, width - 1);
                    const std::size_t texel = static_cast<std::size_t>(y) * width + x;
                    const int i = static_cast<int>(row * 4 + column);
                    for (int c = 0; c < channels; ++c) {
                        if (halfFloat) {
                            gathered16[i * 3 + c] = halves[texel * 3 + c];
                        } else {
                            gathered8[i * channels + c] = bytes[texel * channels + c];
                        }
                    }
                }
            }
            std::uint8_t* block =
                out.data() + (static_cast<std::size_t>(by) * blocksWide + bx) * blockBytes;
            switch (encoding) {
                case BlockEncoding::Bc4:  EncodeBc4Block(gathered8, block); break;
                case BlockEncoding::Bc5:  EncodeBc5Block(gathered8, block); break;
                case BlockEncoding::Bc6h: EncodeBc6hBlock(gathered16, block); break;
                case BlockEncoding::Bc6hSigned:
                    EncodeBc6hBlock(gathered16, block, true);
                    break;
                case BlockEncoding::Bc7:  EncodeBc7Block(gathered8, block); break;
            }
        }
    }
    return out;
}

}  // namespace hdclaude
