# The wavefront integrator

Status: design of record. Last revised 2026-09-05.

This document specifies the GPU execution model: path state layout, queues,
kernels, and how a frame is scheduled. Rationale for choosing wavefront over a
megakernel is in [architecture.md](architecture.md) §2.

## 1. Path state

Path state is **structure-of-arrays** in device-local storage buffers, indexed
by path slot. SoA rather than AoS because every kernel touches a different
subset of fields, and a coalesced read of one field beats a strided read of a
struct.

```text
origin        vec3    ray origin, world space
direction     vec3    ray direction, world space
throughput    vec4    spectral throughput on the four hero wavelengths
radiance      vec4    accumulated radiance, same four wavelengths
wavelengths   vec4    the hero wavelengths, nm, fixed for the path's life
pixel         uint    linear pixel index
rngState      uint    sampler state (see 5)
flags         uint    depth, delta-bounce, inside-medium, terminated
lastPdf       float   solid-angle pdf of the previous scattering, for MIS
mediumId      uint    interior medium, or invalid
```

Slot count is `renderWidth * renderHeight * pathsPerPixel`, allocated once per
resolution. At 1920x1080 with one path per pixel this is ~150 MB, which is the
memory price of a wavefront design and is paid deliberately.

## 2. Queues

Every queue is a device-local index buffer plus a device-local counter. **The
CPU never reads a counter during a frame.** Dispatch sizes come from
`vkCmdDispatchIndirect` reading a GPU-written indirect command buffer, which a
small `prepare_dispatch` kernel fills from the counters.

| Queue | Contents |
| --- | --- |
| `active` | path slots that still need traversal |
| `hit[m]` | path slots whose hit uses material `m` — one per distinct material |
| `escaped` | path slots that missed geometry; consumed by the environment kernel |
| `shadow` | shadow-ray records produced by next-event estimation |

`hit[m]` queues share one backing buffer partitioned by a prefix sum over the
per-material counts, so the number of distinct materials does not multiply
allocations.

## 3. Kernels

All compute. One frame is:

```text
raygen
repeat maxBounces times:
    prepare_dispatch                 # counters -> indirect args
    extend                           # closest hit for `active`
    sort                             # count, prefix-sum, scatter into hit[m]
    environment                      # consume `escaped`
    prepare_dispatch
    for each material m present:
        shade_m                      # MaterialX program; emits shadow + next active
    prepare_dispatch
    shadow                           # any-hit; unoccluded contributions to radiance
film
```

### `raygen`
Generates camera rays with subpixel jitter (Halton, or the DLSS-required
sequence in temporal modes), samples the hero wavelength packet, and initialises
path state. Writes all slots to `active`.

### `extend`
One `rayQueryEXT` closest-hit traversal per active path. Writes a hit record:
instance, primitive, barycentrics, and material id. Where
`VK_NV_ray_tracing_invocation_reorder` is available, issues a reorder hint keyed
on the material id before writing, so the *next* kernel's memory access pattern
is already coherent. Where it is not, the sort in the next kernel recovers the
same coherence at slightly higher cost.

### `sort`
A three-pass counting sort keyed on material id: count into per-material
counters, exclusive prefix sum over the material table, scatter path indices
into `hit[m]`. Material count is bounded by the scene's distinct compiled
programs, typically tens, so the prefix sum is a single workgroup.

### `shade_m`
The heart of the design. One dispatch per material present in this bounce, each
bound to **that material's compiled MaterialX program** (see
[materialx-codegen.md](materialx-codegen.md)). Per invocation:

1. Reconstruct the `SurfaceHit`: interpolate position, shading normal,
   tangent frame, UVs, and any `geomprop` the material's generated code
   declares.
2. Evaluate the MaterialX program to fill closures.
3. Next-event estimation: choose a light, evaluate the closure with
   `CLOSURE_TYPE_REFLECTION`/`TRANSMISSION`, compute the MIS weight against
   `CLOSURE_TYPE_PT_PDF`, and push a shadow record.
4. Scatter: `CLOSURE_TYPE_PT_SAMPLE` produces a direction, spectral weight, and
   pdf. Update throughput, apply Russian roulette after a minimum depth, and
   push the slot back to `active` if it survives.
5. Emission: `CLOSURE_TYPE_EMISSION` with the MIS weight against the light
   sampling pdf for the same surface.

Because the dispatch is material-homogeneous, its register footprint is that
material's alone.

### `shadow`
Any-hit traversal with early termination for the shadow queue. Unoccluded
records add their pre-computed contribution to the path's radiance. Batching
all shadow rays for a bounce into one dispatch is what makes NEE cheap.

### `film`
Resolves spectral radiance to CIE XYZ, accumulates into the reference buffer or
writes the interactive frame plus guide images, and applies the sensor
conversion described in [spectral-rendering.md](spectral-rendering.md) §5.

## 4. Compaction

After each bounce, `active` is rebuilt from surviving paths only. A frame that
starts with 2M paths and retains 30% after three bounces dispatches 600K threads
at bounce four, not 2M with 70% of lanes idle. Compaction is the second reason
wavefront beats a megakernel; sorting is the first.

## 5. Sampling

A hash-based stateless sampler (PCG32 seeded from pixel, sample index, bounce,
and dimension) rather than a stored sequence. Stateless means a kernel can
reproduce any dimension without carrying it in path state, and it survives
compaction and reordering — which a stored Sobol sequence position does not,
without extra state.

Determinism requirement: for a fixed scene, camera, resolution, and sample
count, the reference image is bit-identical across runs. This is testable and
is a phase 6 gate.

## 6. Frame slots

Two frame slots, each owning **its own** path state, queues, counters, indirect
buffers, uniforms, descriptor sets, timestamp ranges, and interactive image
storage. Nothing is shared between slots except the scene, the acceleration
structures, and the reference accumulation buffer — none of which a slot writes
during an interactive frame.

This is Rule 1 from [lessons](lessons-from-hdcodex.md) applied at the level
that matters: it is what makes frame N+1's trace genuinely overlap frame N's
readback. A barrier's first synchronisation scope includes every command already
submitted to the same queue, so **any** shared written resource serialises the
slots regardless of how many command buffers and fences exist. hdCodex added the
command buffers and fences and kept the shared buffer; the overlap it claimed
was structurally impossible (finding N3).

## 7. Performance work, in the order it will be measured

Recorded so the order is a decision rather than an accident.

1. Per-material dispatch and compaction — the architecture itself.
2. SER in `extend` on supporting hardware.
3. Shadow-ray batching depth (one queue per bounce vs. per frame).
4. Ray differentials and texture LOD. Untextured-LOD path tracing thrashes the
   texture cache on minified surfaces; this is usually a larger win than it
   sounds.
5. Light BVH for many-light scenes, replacing uniform light selection.
6. Sort granularity — full counting sort vs. partial separation of the most
   expensive materials.

Each is measured on the gallery before and after, with the numbers recorded in
[gallery.md](../gallery.md). No optimisation lands without a measurement.
