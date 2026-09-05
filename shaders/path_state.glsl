// Shared layout for the wavefront kernels.
//
// Included by raygen, extend, shade, shadow and film, so every kernel agrees
// about the bindings and the path state without any of them restating it.
// Mirrored on the host by include/hdclaude/gpu/path_tracer.h; the two must be
// changed together.
//
// Path state is structure-of-arrays: every kernel touches a different subset of
// the fields, and a coalesced read of one array beats a strided read of a
// struct. See docs/wavefront-integrator.md 1.

// Declared here rather than per kernel: this file declares the acceleration
// structure binding, so every kernel that includes it needs the extension that
// makes `accelerationStructureEXT` a type -- including the ones that never
// trace a ray, such as raygen and film.
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require

// --- Frame constants --------------------------------------------------------

layout(set = 0, binding = 0, scalar) uniform FrameBlock {
    mat4  cameraToWorld;
    vec4  environmentColor;     // rgb, w unused
    vec4  sunDirection;         // xyz normalised, w = angular radius (radians)
    vec4  sunRadiance;          // rgb, w unused
    uvec2 resolution;
    uint  sampleIndex;          // progressive sample being traced
    uint  maxBounces;
    float tanHalfFov;
    float aspect;
    uint  pathCount;
    uint  bounce;               // current bounce, 0 for camera rays
} frame;

// --- Path state -------------------------------------------------------------

layout(set = 0, binding = 1, scalar) buffer PathOrigin   { vec3  values[]; } pathOrigin;
layout(set = 0, binding = 2, scalar) buffer PathDir      { vec3  values[]; } pathDirection;
layout(set = 0, binding = 3, scalar) buffer PathThrough  { vec3  values[]; } pathThroughput;
layout(set = 0, binding = 4, scalar) buffer PathRadiance { vec3  values[]; } pathRadiance;
layout(set = 0, binding = 5, scalar) buffer PathPixel    { uint  values[]; } pathPixel;
layout(set = 0, binding = 6, scalar) buffer PathRng      { uint  values[]; } pathRng;

// Hit record written by `extend` and read by `shade`.
//
//   x  instance custom index, or -1 for a miss
//   y  primitive index
//   z  barycentric u, encoded
//   w  barycentric v, encoded
layout(set = 0, binding = 7, scalar) buffer HitRecords { ivec4 values[]; } hits;

// --- Queues -----------------------------------------------------------------
//
// Counters live on the GPU and are never read back during a frame; dispatch
// sizes come from indirect commands a small kernel fills from them
// (docs/wavefront-integrator.md 2).
layout(set = 0, binding = 8, scalar) buffer Counters {
    uint activeCount;
    uint nextActiveCount;
    uint shadowCount;
    uint pad;
} counters;

layout(set = 0, binding = 9,  scalar) buffer ActiveQueue     { uint values[]; } activeQueue;
layout(set = 0, binding = 10, scalar) buffer NextActiveQueue { uint values[]; } nextActiveQueue;

// A shadow ray and the radiance it delivers if unoccluded. The contribution is
// computed at shading time and carried here, so the shadow kernel does no
// shading of its own -- it only decides whether the contribution survives.
//
// It carries the *path*, not the pixel. An unoccluded contribution is added to
// that path's radiance, and a path emits at most one shadow ray per bounce, so
// no two invocations of the shadow kernel write the same location and no
// atomic is needed. See the note on the film buffer below.
struct ShadowRay {
    vec3 origin;
    vec3 direction;
    vec3 contribution;
    float maxDistance;
    uint path;
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(set = 0, binding = 11, scalar) buffer ShadowRays { ShadowRay values[]; } shadowRays;

// --- Film -------------------------------------------------------------------
//
// RGBA32F, accumulated across progressive samples. Alpha holds the sample count
// so a partially converged frame can be resolved at any time.
//
// Written without atomics, which is safe only because paths map one-to-one onto
// pixels: `pathPixel[i] == i`, so exactly one invocation of the film kernel
// touches each entry. Tracing several paths per pixel would break that, and the
// fix then is a float atomic -- which needs VK_EXT_shader_atomic_float and the
// shaderBufferFloat32AtomicAdd feature, neither of which the renderer requires
// today. The invariant is stated here so the requirement is not discovered by a
// race.
layout(set = 0, binding = 12, scalar) buffer Accumulation { vec4 values[]; } accumulation;

// --- Scene ------------------------------------------------------------------

layout(set = 0, binding = 13) uniform accelerationStructureEXT sceneTlas;

layout(buffer_reference, scalar) readonly buffer PositionBuffer { vec3 values[]; };
layout(buffer_reference, scalar) readonly buffer IndexBuffer    { uint values[]; };
layout(buffer_reference, scalar) readonly buffer NormalBuffer   { vec3 values[]; };
layout(buffer_reference, scalar) readonly buffer UvBuffer       { vec2 values[]; };

/// Per-instance geometry, reached by device address so that adding a prototype
/// does not touch any descriptor set.
struct InstanceGeometry {
    uint64_t positions;
    uint64_t indices;
    uint64_t normals;   // zero if the mesh has no authored normals
    uint64_t uvs;       // zero if the mesh has no texture coordinates
    mat3x4 objectToWorld;
    mat3x4 worldToObject;
    uint material;
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(set = 0, binding = 14, scalar) readonly buffer InstanceTable {
    InstanceGeometry values[];
} instances;

// --- Sampling ---------------------------------------------------------------

// PCG hash, stateless: any dimension is reproducible from the seed without
// carrying sampler state, which is what lets path state survive compaction.
uint hdclaude_pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float hdclaude_random(inout uint state)
{
    return float(hdclaude_pcg(state)) * (1.0 / 4294967296.0);
}

uint hdclaude_seed(uint pixel, uint sampleIndex, uint bounce)
{
    uint h = pixel * 73856093u ^ sampleIndex * 19349663u ^ bounce * 83492791u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    return h + 1u;
}

// --- Geometry helpers -------------------------------------------------------

/// Offset a ray origin off a surface along its geometric normal.
///
/// Scaled by the position's magnitude so the offset stays meaningful at any
/// scene scale; a fixed epsilon either leaks at large coordinates or visibly
/// detaches shadows at small ones.
vec3 hdclaude_offset_ray(vec3 position, vec3 normal)
{
    float scale = max(1.0, max(abs(position.x), max(abs(position.y), abs(position.z))));
    return position + normal * (1.0e-5 * scale);
}
