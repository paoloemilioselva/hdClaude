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
    uint  hasDomeTexture;       // 1 when hdclaude_dome holds an environment map
    uint  materialCount;        // compiled shading pipelines; the sort's key range
    mat4  domeWorldToLight;     // takes a world direction into the dome's frame
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

// The sorted queue: the active paths that hit geometry, grouped by the material
// that shades them. One backing buffer partitioned by a prefix sum over the
// per-material counts, so a scene with more materials costs no more allocations
// (docs/wavefront-integrator.md 2). A path that missed is absent -- the
// environment kernel owns it -- so the groups do not cover the whole queue.
layout(set = 0, binding = 18, scalar) buffer MaterialQueue { uint values[]; } materialQueue;

// Counts, offsets and scatter cursors for the sort, `frame.materialCount`
// entries each, in that order in one buffer.
//
// Three arrays rather than three buffers because the stride is not known until
// a scene is published, and a descriptor set layout is fixed before that. The
// accessors below are the only places that know the packing.
layout(set = 0, binding = 19, scalar) buffer MaterialTable { uint values[]; } materialTable;

uint hdclaude_material_count(uint material)
{
    return materialTable.values[material];
}

uint hdclaude_material_offset(uint material)
{
    return materialTable.values[frame.materialCount + material];
}

// Indirect dispatch commands, GPU-written by prepare_dispatch and read by the
// command processor. The CPU never reads a counter during a frame
// (docs/wavefront-integrator.md 2), so every dispatch whose size depends on one
// takes its workgroup count from here.
//
//   0        the active queue: extend, the sort passes, environment
//   1        the shadow queue
//   2 + m    material m's group of the sorted queue
//
// `w` is padding: VkDispatchIndirectCommand is three uints, and the fourth
// keeps each command 16-byte aligned so a slot's byte offset is slot * 16.
layout(set = 0, binding = 20, scalar) buffer DispatchArgs { uvec4 values[]; } dispatchArgs;

#define HDCLAUDE_DISPATCH_ACTIVE 0u
#define HDCLAUDE_DISPATCH_SHADOW 1u
#define HDCLAUDE_DISPATCH_MATERIAL 2u

/// Workgroups needed to cover `items` at the 64-wide layout every queue kernel
/// declares.
uvec4 hdclaude_dispatch_groups(uint items)
{
    return uvec4((items + 63u) / 64u, 1u, 1u, 0u);
}

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

/// The 3x3 linear part of an instance transform.
///
/// An instance transform is stored row-major 3x4, which is the layout Vulkan's
/// acceleration-structure instance expects, so the host writes it once and uses
/// it for both. GLSL indexes a `mat3x4` by *column*, so `transform[i]` here is
/// the i-th row of what the host wrote, and a matrix built from those as
/// columns is the transpose of the one intended. Transposing gives back a
/// matrix that multiplies a column vector.
///
/// Getting this wrong is invisible in the geometry -- positions come from the
/// acceleration structure, which the driver transforms itself -- and shows only
/// in shading, as normals and tangents that are wrong by a rotation.
mat3 hdclaude_linear(mat3x4 transform)
{
    return transpose(mat3(transform[0].xyz, transform[1].xyz, transform[2].xyz));
}

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
#define HDCLAUDE_LIGHT_CYLINDER 4u

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

    float coneCosAngle;   // cosine of the shaping cone, -1 when unshaped
    float coneSoftness;   // 0 hard edge, 1 falloff across the whole cone
    float focus;          // focus exponent about the axis, 0 for uniform
    float pad0;
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
//
// The dome light's environment map is the exception: it is a single sampler
// rather than an array element, because the kernel that reads it -- the
// environment kernel -- has no generated material in front of it and so no
// array declaration. A placeholder is bound when the scene has no dome map,
// and frame.hasDomeTexture says whether to look.
layout(set = 0, binding = 17) uniform sampler2D hdclaude_dome;

/// Radiance leaving the scene along `direction`.
vec3 hdclaude_environment(vec3 direction)
{
    if (frame.hasDomeTexture == 0u)
    {
        return frame.environmentColor.rgb;
    }

    // Latitude-longitude, in the dome's own frame. USD's dome has +Y up and
    // wraps u about -Z, and the decoded image rows run bottom-up, so v = 1 is
    // straight up.
    vec3 d = normalize((frame.domeWorldToLight * vec4(direction, 0.0)).xyz);
    float u = atan(d.x, -d.z) * (1.0 / 6.28318530718) + 0.5;
    float v = 1.0 - acos(clamp(d.y, -1.0, 1.0)) * (1.0 / 3.14159265359);
    return texture(hdclaude_dome, vec2(u, v)).rgb * frame.environmentColor.rgb;
}

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

/// UsdLuxShapingAPI falloff for a direction leaving the light.
///
/// Applied to the emitted radiance rather than to the density: shaping changes
/// how much light leaves in a direction, not how the sampler chose it. Folding
/// it into the density instead would make a spot light's estimator wrong
/// wherever the cone cuts off.
float hdclaude_light_shaping(Light light, vec3 outgoing)
{
    if (light.coneCosAngle <= -1.0 && light.focus <= 0.0)
    {
        return 1.0;
    }

    float cosTheta = dot(normalize(light.direction), normalize(outgoing));
    if (cosTheta <= 0.0)
    {
        return 0.0;
    }

    float attenuation = 1.0;

    if (light.coneCosAngle > -1.0)
    {
        if (cosTheta < light.coneCosAngle)
        {
            return 0.0;
        }
        // Softness widens an inner cone inward from the edge, and the falloff
        // runs smoothly between the two. Softness 0 leaves a hard edge.
        float inner = mix(1.0, light.coneCosAngle,
                          clamp(light.coneSoftness, 0.0, 1.0));
        if (inner > light.coneCosAngle)
        {
            attenuation *= smoothstep(light.coneCosAngle, inner, cosTheta);
        }
    }

    if (light.focus > 0.0)
    {
        attenuation *= pow(cosTheta, light.focus);
    }
    return attenuation;
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
    else if (light.type == HDCLAUDE_LIGHT_DISK)
    {
        float r = light.radius * sqrt(u.x);
        float phi = 6.28318530718 * u.y;
        normal = normalize(light.direction);
        vec3 t, b;
        hdclaude_light_basis(normal, t, b);
        point = light.position + t * (r * cos(phi)) + b * (r * sin(phi));
    }
    else   // HDCLAUDE_LIGHT_CYLINDER
    {
        // Uniform over the curved surface only; USD's cylinder light has no
        // end caps. `uAxis` carries the half-length along the axis.
        vec3 axis = light.uAxis;
        float halfLength = length(axis);
        if (halfLength <= 1.0e-6)
        {
            return result;
        }
        axis /= halfLength;

        vec3 t, b;
        hdclaude_light_basis(axis, t, b);
        float phi = 6.28318530718 * u.y;
        normal = normalize(t * cos(phi) + b * sin(phi));
        point = light.position + axis * ((u.x * 2.0 - 1.0) * halfLength) +
                normal * light.radius;
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
    result.radiance = light.radiance * hdclaude_light_shaping(light, -direction);
    // Uniform area density 1/A converted to solid angle: d^2 / (cos * A).
    result.pdf = distanceSquared / (cosLight * light.area);
    return result;
}
