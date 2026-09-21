#include "hdclaude/core/quad_tessellation.h"

#include <algorithm>
#include <cmath>

namespace hdclaude {

namespace {

/// The corners of the domain, in the order a quad's vertices are given.
constexpr float kCorner[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f},
                                 {1.0f, 1.0f}, {0.0f, 1.0f}};

/// A point `t` of the way along side `side`, with t in [0, 1].
void OnSide(int side, float t, float* u, float* v)
{
    const float* from = kCorner[side];
    const float* to = kCorner[(side + 1) % 4];
    *u = from[0] + (to[0] - from[0]) * t;
    *v = from[1] + (to[1] - from[1]) * t;
}

}  // namespace

int EdgeTessellationRate(float lengthInPixels, float targetPixels)
{
    const float target = std::max(targetPixels, 0.01f);
    if (!(lengthInPixels > target)) {
        return 1;
    }
    // The next power of two at or above the ratio, which is the same rounding
    // a refinement level gets: one more level is one more halving.
    const float exponent = std::ceil(std::log2(lengthInPixels / target) - 1.0e-4f);
    const int rate = 1 << std::clamp(static_cast<int>(exponent), 0, 20);
    return std::clamp(rate, 1, kMaxEdgeRate);
}

int EdgeRateForLevel(int level)
{
    const int clamped = std::clamp(level, 0, 20);
    return std::clamp(1 << clamped, 1, kMaxEdgeRate);
}

QuadTessellation TessellateQuad(const int edgeRates[4])
{
    QuadTessellation result;

    int rate[4];
    int inner = 1;
    for (int side = 0; side < 4; ++side) {
        rate[side] = std::clamp(edgeRates[side], 1, kMaxEdgeRate);
        inner = std::max(inner, rate[side]);
    }

    const auto addSample = [&result](float u, float v) {
        const auto index = static_cast<std::uint32_t>(result.SampleCount());
        result.uv.push_back(u);
        result.uv.push_back(v);
        return index;
    };
    const auto addTriangle = [&result](std::uint32_t a, std::uint32_t b,
                                       std::uint32_t c) {
        if (a == b || b == c || a == c) {
            // Two samples of a stitch can coincide where a side's rate and the
            // interior's agree at a corner. A triangle of no area is not a
            // triangle, and one in an acceleration structure is a primitive
            // that can never be hit but is always tested.
            return;
        }
        result.indices.push_back(a);
        result.indices.push_back(b);
        result.indices.push_back(c);
    };

    // --- The boundary ---------------------------------------------------------
    //
    // Walked from (0, 0) round, each side contributing its samples but not its
    // last -- that one is the next side's first, and a boundary that repeated
    // it would be two rings rather than one.
    std::vector<std::uint32_t> boundary;
    std::vector<int> sideStart(5, 0);
    for (int side = 0; side < 4; ++side) {
        sideStart[side] = static_cast<int>(boundary.size());
        for (int step = 0; step < rate[side]; ++step) {
            float u = 0.0f;
            float v = 0.0f;
            OnSide(side, static_cast<float>(step) /
                             static_cast<float>(rate[side]), &u, &v);
            boundary.push_back(addSample(u, v));
        }
    }
    sideStart[4] = static_cast<int>(boundary.size());
    result.boundarySamples = result.SampleCount();

    // Every side one segment: the domain is its four corners and nothing else.
    if (inner < 2) {
        addTriangle(boundary[0], boundary[1], boundary[2]);
        addTriangle(boundary[0], boundary[2], boundary[3]);
        return result;
    }

    // --- The interior grid ----------------------------------------------------
    //
    // The points strictly inside, at the largest of the four rates: (i, j) over
    // 1..inner-1 on each axis. Its own border is what the sides are stitched
    // to, and everything within that border is a regular grid of quads.
    const int span = inner - 1;
    std::vector<std::uint32_t> grid(static_cast<std::size_t>(span) * span);
    for (int j = 0; j < span; ++j) {
        for (int i = 0; i < span; ++i) {
            const float u = static_cast<float>(i + 1) / static_cast<float>(inner);
            const float v = static_cast<float>(j + 1) / static_cast<float>(inner);
            grid[static_cast<std::size_t>(j) * span + i] = addSample(u, v);
        }
    }
    const auto at = [&grid, span](int i, int j) {
        return grid[static_cast<std::size_t>(j) * span + i];
    };

    for (int j = 0; j + 1 < span; ++j) {
        for (int i = 0; i + 1 < span; ++i) {
            addTriangle(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            addTriangle(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    }

    // --- Stitching ------------------------------------------------------------
    //
    // Each side against the run of the interior border that faces it, from one
    // interior corner to the next. Both runs are walked together, advancing
    // whichever has the nearer next sample, which is what makes the result
    // depend on the rates alone and not on which of the two is finer.
    //
    // The corner wedges need no special case: side e's walk starts on the same
    // pair that side e-1's ends on, so the wedge between them is covered by the
    // first triangle of one and the last of the other.
    const auto innerRun = [&](int side) {
        std::vector<std::uint32_t> run;
        run.reserve(static_cast<std::size_t>(span));
        for (int k = 0; k < span; ++k) {
            switch (side) {
                case 0: run.push_back(at(k, 0)); break;
                case 1: run.push_back(at(span - 1, k)); break;
                case 2: run.push_back(at(span - 1 - k, span - 1)); break;
                default: run.push_back(at(0, span - 1 - k)); break;
            }
        }
        return run;
    };

    for (int side = 0; side < 4; ++side) {
        // The side's own samples, this time including its last, which is the
        // next side's first.
        std::vector<std::uint32_t> outer;
        outer.reserve(static_cast<std::size_t>(rate[side]) + 1);
        for (int step = 0; step <= rate[side]; ++step) {
            const int index = sideStart[side] + step;
            outer.push_back(boundary[static_cast<std::size_t>(index) %
                                     boundary.size()]);
        }

        const std::vector<std::uint32_t> in = innerRun(side);
        const int outerSteps = rate[side];
        const int innerSteps = span - 1;

        std::size_t o = 0;
        std::size_t i = 0;
        while (o < outer.size() - 1 || i + 1 < in.size()) {
            // Where each walk has reached, as a fraction of its own run, so
            // the two are compared on the same scale.
            const float outerAt =
                static_cast<float>(o) / static_cast<float>(outerSteps);
            const float innerAt =
                innerSteps > 0
                    ? static_cast<float>(i) / static_cast<float>(innerSteps)
                    : 1.0f;
            const bool advanceOuter =
                i + 1 >= in.size() ||
                (o + 1 < outer.size() && outerAt <= innerAt);
            if (advanceOuter) {
                addTriangle(outer[o], outer[o + 1], in[i]);
                ++o;
            } else {
                addTriangle(outer[o], in[i + 1], in[i]);
                ++i;
            }
        }
    }

    return result;
}

}  // namespace hdclaude
