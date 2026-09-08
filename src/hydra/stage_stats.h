// What a render cost, stage by stage.
//
// A wall-clock time for a whole frame says a scene is slow and nothing about
// where. These say where: how long the scene took to ingest, how long
// refinement took and how much geometry it produced, how many textures were
// decoded and how many bytes they hold, how many materials were generated and
// compiled, and what the device is holding at the peak.
//
// Every counter is atomic because Hydra syncs prims in parallel: refinement and
// material compilation both happen on worker threads, and a plain `+=` from
// several of them is a race that would make the numbers quietly wrong -- which
// for a diagnostic is worse than having none.
//
// Times are accumulated across threads, so a stage's figure is the *total work*
// done in it rather than the wall time it occupied. Those differ by however
// much parallelism Hydra found, and the distinction matters when reading them:
// a refinement total larger than the frame's wall time is threads working at
// once, not an error.

#ifndef HDCLAUDE_HYDRA_STAGE_STATS_H
#define HDCLAUDE_HYDRA_STAGE_STATS_H

#include "pxr/pxr.h"

#include <atomic>
#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

/// Add to an atomic double, which C++ has no fetch_add for before C++20's
/// specialisation is guaranteed usable on every compiler this builds with.
inline void HdClaudeAddMilliseconds(std::atomic<double>& total, double value)
{
    double current = total.load(std::memory_order_relaxed);
    while (!total.compare_exchange_weak(current, current + value,
                                        std::memory_order_relaxed)) {
    }
}

struct HdClaudeStageStats {
    // --- Subdivision ------------------------------------------------------
    /// Total time spent refining, summed over the meshes and the threads.
    std::atomic<double> subdivideMilliseconds{0.0};
    std::atomic<std::uint64_t> meshesRefined{0};
    /// Control-cage points in, refined points out. The ratio is what a
    /// refinement level actually costs on this asset, which is the number
    /// nobody can predict from the level alone.
    std::atomic<std::uint64_t> subdivideInputPoints{0};
    std::atomic<std::uint64_t> subdivideOutputPoints{0};

    // --- Ingestion ---------------------------------------------------------
    /// Taking the scene out of the store and into a snapshot the GPU layer can
    /// read: the traversal, the copies, and nothing on the device.
    std::atomic<double> ingestMilliseconds{0.0};
    /// Publishing that snapshot: uploads, texture residency, and the
    /// acceleration structure build, which is most of it on a heavy scene.
    std::atomic<double> publishMilliseconds{0.0};
    std::atomic<std::uint64_t> instances{0};
    std::atomic<std::uint64_t> triangles{0};

    // --- Rays ----------------------------------------------------------------
    /// Camera rays, which are exactly the pixels times the samples and need no
    /// counter to know. The rays those go on to spawn -- one per bounce that
    /// survives, plus a shadow ray per shading event -- are written on the
    /// device and are not counted yet; doing it needs an accumulator in the
    /// counters buffer and one readback after the frame, and is recorded rather
    /// than estimated. An estimate here would be a guess wearing a number's
    /// clothes.
    std::atomic<std::uint64_t> cameraRays{0};

    // --- Materials ---------------------------------------------------------
    /// Generation and SPIR-V compilation together: they are one cost from the
    /// outside and there is no moment between them worth reporting.
    std::atomic<double> materialMilliseconds{0.0};
    std::atomic<std::uint64_t> materialsCompiled{0};

    // --- Textures ----------------------------------------------------------
    std::atomic<double> textureMilliseconds{0.0};
    std::atomic<std::uint64_t> texturesLoaded{0};
    std::atomic<std::uint64_t> textureBytes{0};

    /// Reset between renders. A stat that accumulated across two frames would
    /// describe neither.
    void Reset()
    {
        subdivideMilliseconds.store(0.0, std::memory_order_relaxed);
        meshesRefined.store(0, std::memory_order_relaxed);
        subdivideInputPoints.store(0, std::memory_order_relaxed);
        subdivideOutputPoints.store(0, std::memory_order_relaxed);
        ingestMilliseconds.store(0.0, std::memory_order_relaxed);
        publishMilliseconds.store(0.0, std::memory_order_relaxed);
        instances.store(0, std::memory_order_relaxed);
        triangles.store(0, std::memory_order_relaxed);
        cameraRays.store(0, std::memory_order_relaxed);
        materialMilliseconds.store(0.0, std::memory_order_relaxed);
        materialsCompiled.store(0, std::memory_order_relaxed);
        textureMilliseconds.store(0.0, std::memory_order_relaxed);
        texturesLoaded.store(0, std::memory_order_relaxed);
        textureBytes.store(0, std::memory_order_relaxed);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif   // HDCLAUDE_HYDRA_STAGE_STATS_H
