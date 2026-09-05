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
    uint  lightCount;           // entries in the light table
    uint  pad0;
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
layout(buffer_reference, scalar) readonly buffer TriMaterialBuffer { uint values[]; };

/// Per-instance geometry, reached by device address so that adding a prototype
/// does not touch any descriptor set.
struct InstanceGeometry {
    uint64_t positions;
    uint64_t indices;
    uint64_t normals;   // zero if the mesh has no authored normals
    uint64_t uvs;       // zero if the mesh has no texture coordinates
    // Per-triangle material, from GeomSubsets. Zero when every triangle uses
    // the instance's own binding, which is the common case.
    uint64_t triangleMaterials;
    mat3x4 objectToWorld;
    mat3x4 worldToObject;
    uint material;
    uint pad0;
    uint pad1;
    uint pad2;
};

/// The material shading triangle `primitive` of `geometry`.
uint hdclaude_material_of(InstanceGeometry geometry, int primitive)
{
    if (geometry.triangleMaterials != 0ul)
    {
        return TriMaterialBuffer(geometry.triangleMaterials).values[primitive];
    }
    return geometry.material;
}
layout(set = 0, binding = 14, scalar) readonly buffer InstanceTable {
    InstanceGeometry values[];
} instances;

// --- Lights -----------------------------------------------------------------
//
// Analytic, and deliberately *not* in the acceleration structure. Only
// next-event estimation finds a light, so a scattered ray can never hit one and
// there is no double counting to weigh away with MIS. The cost is variance on
// glossy reflections of large area lights; the benefit is that the first
// implementation is correct rather than nearly correct.
//
// Mirrors hdclaude::Light in include/hdclaude/gpu/scene.h; the two must be
// changed together.

#define HDCLAUDE_LIGHT_DISTANT 0u
#define HDCLAUDE_LIGHT_SPHERE  1u
#define HDCLAUDE_LIGHT_RECT    2u
#define HDCLAUDE_LIGHT_DISK    3u

struct Light {
    vec3  position;       // world centre; unused by a distant light
    float radius;         // sphere and disk radius

    vec3  direction;      // the direction the light emits along (USD's -Z)
    float angularRadius;  // distant light half-angle, radians

    vec3  radiance;       // colour * intensity * 2^exposure, area-normalised
    uint  type;

    vec3  uAxis;          // rectangle half-extent along local X, world space
    uint  castsShadows;

    vec3  vAxis;          // rectangle half-extent along local Y, world space
    float area;
};
layout(set = 0, binding = 15, scalar) readonly buffer LightTable {
    Light values[];
} lights;

// --- Textures ---------------------------------------------------------------
//
// One shared array for the whole scene rather than a descriptor per material.
// A generated material refers to its images by index -- the generator emits
// `#define <sampler> hdclaude_textures[i]` -- so adding a texture changes no
// pipeline layout, and the stock mx_image_* implementations are used unchanged
// because `texture(name, uv)` still expands to a sampler expression.
//
// The capacity is fixed because every kernel shares one descriptor set layout
// and that layout is built before any texture is known. It must match
// kTextureCapacity in include/hdclaude/gpu/scene.h.
//
// Every slot is written, unused ones with a placeholder: an undefined
// descriptor is undefined behaviour, and on this driver that is a lost device
// rather than a wrong colour.
//
// The array itself is *not* declared here. The shade kernel is appended to a
// generated material, so a declaration in this file would come hundreds of
// lines after the material body that samples it. The generator emits the
// declaration instead, and only for a material that has textures; no other
// kernel samples one, and a shader need not declare every binding in its
// layout.

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


// --- Light sampling ---------------------------------------------------------

/// One sample of one light, in world space.
struct LightSample {
    vec3  direction;    // from the surface toward the light, normalised
    float distance;     // to the sampled point; huge for a distant light
    vec3  radiance;     // emitted radiance arriving along `direction`
    float pdf;          // solid-angle density, zero if the sample is unusable
    bool  castsShadows;
};

/// An orthonormal basis around `n`.
void hdclaude_light_basis(vec3 n, out vec3 t, out vec3 b)
{
    // Duff et al.'s branchless construction: numerically stable at n.z near -1,
    // where the naive cross-product basis degenerates.
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = vec3(c, sign + n.y * n.y * a, -n.y);
}

/// Sample light `index` as seen from `position`.
///
/// Area lights are sampled uniformly over their surface and the density is
/// converted to solid angle, which is what lets the caller treat every light
/// type through one expression. A sample whose geometry cannot contribute --
/// behind the light, or degenerate -- returns pdf 0 rather than a small number,
/// so the caller discards it instead of dividing by nearly nothing.
LightSample hdclaude_sample_light(uint index, vec3 position, vec2 u)
{
    Light light = lights.values[index];

    LightSample result;
    result.direction = vec3(0.0, 1.0, 0.0);
    result.distance = 0.0;
    result.radiance = vec3(0.0);
    result.pdf = 0.0;
    result.castsShadows = light.castsShadows != 0u;

    if (light.type == HDCLAUDE_LIGHT_DISTANT)
    {
        // A cone of directions around the light's axis. The surface is lit
        // *from* -direction, since `direction` is where the light points.
        vec3 axis = normalize(-light.direction);
        float cosMax = cos(max(light.angularRadius, 1.0e-4));

        vec3 t, b;
        hdclaude_light_basis(axis, t, b);
        float cosTheta = mix(cosMax, 1.0, u.x);
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        float phi = 6.28318530718 * u.y;

        result.direction = normalize(t * (cos(phi) * sinTheta) +
                                     b * (sin(phi) * sinTheta) +
                                     axis * cosTheta);
        result.distance = 1.0e30;
        result.radiance = light.radiance;
        result.pdf = 1.0 / (6.28318530718 * max(1.0e-6, 1.0 - cosMax));
        return result;
    }

    vec3 point;
    vec3 normal;

    if (light.type == HDCLAUDE_LIGHT_SPHERE)
    {
        // Uniform over the whole sphere rather than over the visible cap. The
        // back half is rejected below by the facing test, which costs samples
        // but keeps the density exactly 1/area and matches what the area-to-
        // solid-angle conversion assumes.
        float z = 1.0 - 2.0 * u.x;
        float r = sqrt(max(0.0, 1.0 - z * z));
        float phi = 6.28318530718 * u.y;
        normal = vec3(r * cos(phi), r * sin(phi), z);
        point = light.position + normal * light.radius;
    }
    else if (light.type == HDCLAUDE_LIGHT_RECT)
    {
        vec2 offset = u * 2.0 - 1.0;
        point = light.position + light.uAxis * offset.x + light.vAxis * offset.y;
        normal = normalize(light.direction);
    }
    else   // HDCLAUDE_LIGHT_DISK
    {
        float r = light.radius * sqrt(u.x);
        float phi = 6.28318530718 * u.y;
        normal = normalize(light.direction);
        vec3 t, b;
        hdclaude_light_basis(normal, t, b);
        point = light.position + t * (r * cos(phi)) + b * (r * sin(phi));
    }

    vec3 toLight = point - position;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1.0e-12 || light.area <= 0.0)
    {
        return result;
    }
    float distance = sqrt(distanceSquared);
    vec3 direction = toLight / distance;

    // A rect or disk emits from one face only; a sphere's sampled point must
    // face the receiver.
    float cosLight = dot(normal, -direction);
    if (cosLight <= 1.0e-6)
    {
        return result;
    }

    result.direction = direction;
    result.distance = distance;
    result.radiance = light.radiance;
    // Uniform area density 1/A converted to solid angle: d^2 / (cos * A).
    result.pdf = distanceSquared / (cosLight * light.area);
    return result;
}
