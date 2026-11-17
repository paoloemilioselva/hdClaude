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

    // --- Instancing ----------------------------------------------------------
    //
    // What the last publication's flat placement list was made of. hdClaude
    // flattens instancing, nested or not, into one placement per prototype
    // copy (docs/architecture.md), so the cost of a heavily instanced stage is
    // the length of that list and the bytes it takes on the device, and these
    // are what say how close a stage is to where flattening stops being free.
    // All written by the render thread after a publication, from counts it
    // already had; nothing here is computed when a host asks.
    std::atomic<std::uint64_t> prototypes{0};
    std::atomic<std::uint64_t> splatPlacements{0};
    /// Rprims placed by an instancer, and the deepest chain of instancers
    /// above any of them: 1 for a point instancer or a native instance, 2 for
    /// either inside another, and so on.
    std::atomic<std::uint64_t> instancedRprims{0};
    std::atomic<std::uint64_t> instancingDepth{0};
    /// The rprim with the most placements, and how many. Its path is kept by
    /// the delegate beside these, because a path is not an atomic.
    std::atomic<std::uint64_t> largestPlacementCount{0};
    /// Time the rprims spent computing their placements through their
    /// instancers, summed over threads, as the other sync timers are.
    std::atomic<double> instancingMilliseconds{0.0};

    /// Device-local bytes the scene's structures hold after the publication,
    /// read once from the allocator's breakdown on the render thread.
    std::atomic<std::uint64_t> blasBytes{0};
    std::atomic<std::uint64_t> tlasBytes{0};
    /// The placement array the top-level build reads and shading reads each
    /// hit's transform from: 64 bytes a placement.
    std::atomic<std::uint64_t> tlasInstanceBytes{0};
    /// The largest scratch one top-level build asked for. Released after the
    /// build, so it is not in the resident total, but it is held at the
    /// moment a stage's memory peaks.
    std::atomic<std::uint64_t> tlasScratchBytes{0};
    std::atomic<std::uint64_t> tlasChunks{0};
    std::atomic<double> tlasBuildMilliseconds{0.0};
    /// The per-placement tables shading reads beside the structure: the
    /// instance table, motion history, light and cull links, and pick ids.
    std::atomic<std::uint64_t> placementTableBytes{0};
    /// Vertex data (positions, normals, uvs, curve segments, splats) and
    /// indices of every prototype. The `primvar` and `topology` lines usdview's
    /// GPU-stats HUD prints, under the names Storm gives them.
    std::atomic<std::uint64_t> vertexBytes{0};
    std::atomic<std::uint64_t> indexBytes{0};

    // --- Rays ----------------------------------------------------------------
    /// Camera rays, which are exactly the pixels times the samples and need no
    /// counter to know. The rays those go on to spawn -- one per bounce that
    /// survives, plus a shadow ray per shading event -- are written on the
    /// device and are not counted yet; doing it needs an accumulator in the
    /// counters buffer and one readback after the frame, and is recorded rather
    /// than estimated. An estimate here would be a guess wearing a number's
    /// clothes.
    /// Bottom-level acceleration structures built, and reused because their
    /// geometry matched one already held. See PathTracer::BlasBuilt.
    std::atomic<std::uint64_t> blasBuilt{0};
    std::atomic<std::uint64_t> blasReused{0};
    std::atomic<std::uint64_t> blasRefit{0};
    /// Images the last publication uploaded, the bytes they took, and images
    /// it kept because they were the ones already on the device. See
    /// PathTracer::TexturesUploaded.
    std::atomic<std::uint64_t> texturesUploaded{0};
    std::atomic<std::uint64_t> textureBytesUploaded{0};
    std::atomic<std::uint64_t> texturesReused{0};
    /// Images the last publication could not fit in device memory, and the
    /// bytes they needed. See PathTracer::SetTextureMemoryLimit.
    std::atomic<std::uint64_t> texturesOverBudget{0};
    std::atomic<std::uint64_t> textureBytesOverBudget{0};
    /// Images the last publication block-compressed, and those compression
    /// could not take. See PathTracer::SetTextureCompression.
    std::atomic<std::uint64_t> texturesCompressed{0};
    std::atomic<std::uint64_t> texturesNotCompressed{0};

    /// Frames the denoiser produced, and the time it took over them, guides
    /// cleaned included. See docs/denoising.md.
    std::atomic<std::uint64_t> denoisedFrames{0};
    std::atomic<double> denoiseMilliseconds{0.0};

    std::atomic<std::uint64_t> cameraRays{0};
    /// One per active path per bounce, and one per shadow ray shading asked
    /// for, counted on the device and read back once per call. The ratio of
    /// these to the camera rays is how far paths actually get before they are
    /// absorbed, terminated or leave -- which is a property of the scene, not
    /// of the settings, and cannot be predicted from the bounce limit.
    std::atomic<std::uint64_t> tracedRays{0};
    std::atomic<std::uint64_t> shadowRays{0};
    /// A hash over every hit resolved. Two runs that agree here found the same
    /// geometry for the same rays; see PathTracer::HitHash.
    std::atomic<std::uint64_t> hitHash{0};

    /// The companion hash over the rays themselves; see PathTracer::RayHash.
    /// Two processes can trace the same number of rays without tracing the
    /// same rays, and only the pair of hashes together says which.
    std::atomic<std::uint64_t> rayHash{0};

    // --- Materials ---------------------------------------------------------
    /// Generation and SPIR-V compilation together: they are one cost from the
    /// outside and there is no moment between them worth reporting.
    /// How long the path tracer spent tracing, summed over every progressive
    /// call in the render.
    ///
    /// This is the one that was missing, and it is the largest: every other
    /// timer here covers preparing a scene, and none of them covered rendering
    /// it. A reader adding the stages up and comparing them with the wall time
    /// found a gap the size of the render and no line to put it against.
    std::atomic<double> traceMilliseconds{0.0};

    /// The per-kernel GPU time of one sample, when HDCLAUDE_PROFILE_KERNELS is
    /// set. Zero otherwise. See PathTracer::KernelProfile: traceMs alone cannot
    /// say which kernel a slow scene is slow in.
    std::atomic<double> kernelPrepareMs{0.0};
    std::atomic<double> kernelExtendMs{0.0};
    std::atomic<double> kernelSortMs{0.0};
    std::atomic<double> kernelEnvironmentMs{0.0};
    std::atomic<double> kernelShadeMs{0.0};
    std::atomic<double> kernelShadowMs{0.0};
    std::atomic<double> kernelFilmMs{0.0};

    std::atomic<double> materialMilliseconds{0.0};
    std::atomic<std::uint64_t> materialsCompiled{0};
    /// Materials whose network matched one already compiled, and that were
    /// given its program rather than compiled again.
    std::atomic<std::uint64_t> materialsReused{0};

    // --- Textures ----------------------------------------------------------
    std::atomic<double> textureMilliseconds{0.0};
    std::atomic<std::uint64_t> texturesLoaded{0};
    std::atomic<std::uint64_t> textureBytes{0};

    // --- Startup -------------------------------------------------------------
    //
    // What the delegate costs before a scene reaches it. These are *not* reset
    // between renders: they happen once, when the delegate is constructed, and
    // a second frame that reported them as zero would say the startup was free
    // rather than already paid.
    //
    // They exist because the stages above sum to a few seconds on a scene whose
    // wall time is twenty, and a figure nobody records is a figure nobody
    // improves: hdClaude took 18.5 s to render a stage holding one camera and no
    // geometry, against Storm's 0.9 s, and none of it appeared in any stat.
    /// Creating the Vulkan instance and device, and choosing the adapter.
    std::atomic<double> startupVulkanMs{0.0};
    /// Compiling the fixed kernels -- raygen, extend, shade, shadow, film and
    /// the rest -- and building their pipelines.
    std::atomic<double> startupKernelsMs{0.0};
    /// Loading the MaterialX standard libraries, which the material compiler
    /// needs before it can generate anything.
    std::atomic<double> startupMaterialXMs{0.0};
    /// Compiling the fallback material, which is generated up front so a
    /// failure during Sync has something to fall back to.
    std::atomic<double> startupFallbackMs{0.0};

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
        prototypes.store(0, std::memory_order_relaxed);
        splatPlacements.store(0, std::memory_order_relaxed);
        instancedRprims.store(0, std::memory_order_relaxed);
        instancingDepth.store(0, std::memory_order_relaxed);
        largestPlacementCount.store(0, std::memory_order_relaxed);
        instancingMilliseconds.store(0.0, std::memory_order_relaxed);
        blasBytes.store(0, std::memory_order_relaxed);
        tlasBytes.store(0, std::memory_order_relaxed);
        tlasInstanceBytes.store(0, std::memory_order_relaxed);
        tlasScratchBytes.store(0, std::memory_order_relaxed);
        tlasChunks.store(0, std::memory_order_relaxed);
        tlasBuildMilliseconds.store(0.0, std::memory_order_relaxed);
        placementTableBytes.store(0, std::memory_order_relaxed);
        vertexBytes.store(0, std::memory_order_relaxed);
        indexBytes.store(0, std::memory_order_relaxed);
        blasBuilt.store(0, std::memory_order_relaxed);
        blasReused.store(0, std::memory_order_relaxed);
        blasRefit.store(0, std::memory_order_relaxed);
        texturesUploaded.store(0, std::memory_order_relaxed);
        textureBytesUploaded.store(0, std::memory_order_relaxed);
        texturesReused.store(0, std::memory_order_relaxed);
        texturesOverBudget.store(0, std::memory_order_relaxed);
        textureBytesOverBudget.store(0, std::memory_order_relaxed);
        texturesCompressed.store(0, std::memory_order_relaxed);
        texturesNotCompressed.store(0, std::memory_order_relaxed);
        denoisedFrames.store(0, std::memory_order_relaxed);
        denoiseMilliseconds.store(0.0, std::memory_order_relaxed);
        cameraRays.store(0, std::memory_order_relaxed);
        tracedRays.store(0, std::memory_order_relaxed);
        shadowRays.store(0, std::memory_order_relaxed);
        hitHash.store(0, std::memory_order_relaxed);
        rayHash.store(0, std::memory_order_relaxed);
        traceMilliseconds.store(0.0, std::memory_order_relaxed);
        materialMilliseconds.store(0.0, std::memory_order_relaxed);
        materialsCompiled.store(0, std::memory_order_relaxed);
        materialsReused.store(0, std::memory_order_relaxed);
        textureMilliseconds.store(0.0, std::memory_order_relaxed);
        texturesLoaded.store(0, std::memory_order_relaxed);
        textureBytes.store(0, std::memory_order_relaxed);
    }
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif   // HDCLAUDE_HYDRA_STAGE_STATS_H
