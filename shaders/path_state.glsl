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

// Number of correlated hero wavelengths a path carries. Must equal
// hdclaude::kSpectralLanes in include/hdclaude/core/spectrum.h.
//
// Guarded because the closure ABI declares the same constant: a generated
// material includes `lib/mx_closure_type.glsl` and the shade kernel is appended
// to it, so both definitions are in scope there and only there.
#ifndef HDCLAUDE_SPECTRAL_LANES
#define HDCLAUDE_SPECTRAL_LANES 4
#endif

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
    // Which bounce a dispatch is no longer lives here. It is a push constant on
    // the two kernels that vary with it, because a field of a host-written
    // uniform cannot vary within one command buffer -- see
    // shaders/shade.comp.glsl. The slot is kept so the block's scalar layout is
    // unchanged and every other field keeps its offset.
    uint  unusedWasBounce;
    uint  lightCount;           // entries in the light table
    uint  hasDomeTexture;       // 1 when hdclaude_dome holds an environment map
    uint  hasDomeLight;         // 1 when a dome light supplied the environment
    uint  materialCount;        // compiled shading pipelines; the sort's key range
    uint  hasEnvironmentDistribution;   // 1 when the dome map has a CDF built
    uint  environmentWidth;
    uint  environmentHeight;
    uint  environmentConditional;       // index of the conditional CDF block
    uint  environmentDensity;           // index of the density block
    // --- Spectral tables -----------------------------------------------
    uint  spectralSamples;      // entries in the CIE/illuminant grid
    uint  chromaTableOffset;    // where the chromaticity table starts
    uint  chromaTableSize;      // one axis of one face of it
    float spectralNormalisation;  // 1 / integral of D65 * ybar
    float environmentTemperature;      // dome blackbody in kelvin, or zero
    float environmentTemperatureScale; // equates its luminance with D65's
    mat4  domeWorldToLight;     // takes a world direction into the dome's frame
    mat4  domeLightToWorld;     // and back, for a direction sampled in the map
    // World to clip, the host's own view times its own projection. The depth
    // guide is normalised device depth, and which normalisation is the right
    // one is a question about the host's projection rather than about this
    // renderer, so it is carried rather than rebuilt from a field of view.
    mat4  worldToClip;
    // The previous frame's world-to-clip. Equal to worldToClip when there was
    // no previous frame, which reports no motion rather than motion invented
    // from nothing.
    mat4  previousWorldToClip;
    // The frame's sub-pixel offset in pixels, and whether raygen uses it
    // instead of drawing one per sample. See RenderSettings::jitter.
    vec2  jitter;
    uint  useFixedJitter;
    // Whether a light's own geometry is in the scene at all: 1 renders the
    // shapes, 0 removes every one of them however the asset was authored. See
    // RenderSettings::lightGeometry. It takes the slot a pad held, so the
    // block's scalar layout is unchanged.
    uint  lightGeometry;
    // Light linking: words of category membership an instance has in
    // `instanceLinks`, and the dome light's two categories, -1 for none.
    uint  linkWords;
    int   domeLightLink;
    int   domeShadowLink;
    // How a splat cloud is transported: 0 coverage, 1 volume. Two readings of
    // the schema, not two qualities of one (docs/gaussian-splats.md 3 and 7).
    uint  splatTransport;
    // Where the splat placements start in the instance table and how many there
    // are. They are contiguous and follow every mesh placement, so the walk
    // takes a range rather than a list of its own.
    uint  splatInstanceBegin;
    uint  splatInstanceCount;
    uint  padTail;
} frame;

const uint HDCLAUDE_SPLAT_COVERAGE = 0u;
const uint HDCLAUDE_SPLAT_VOLUME = 1u;

// --- Path state -------------------------------------------------------------

layout(set = 0, binding = 1, scalar) buffer PathOrigin   { vec3  values[]; } pathOrigin;
layout(set = 0, binding = 2, scalar) buffer PathDir      { vec3  values[]; } pathDirection;
// Throughput and radiance are per *lane*, not per colour channel. A path
// carries four wavelengths and four scalar quantities along them; RGB appears
// only where an asset supplies one and where the film hands an image back.
layout(set = 0, binding = 3, scalar) buffer PathThrough  { vec4  values[]; } pathThroughput;
layout(set = 0, binding = 4, scalar) buffer PathRadiance { vec4  values[]; } pathRadiance;
layout(set = 0, binding = 5, scalar) buffer PathPixel    { uint  values[]; } pathPixel;
layout(set = 0, binding = 6, scalar) buffer PathRng      { uint  values[]; } pathRng;

// The solid-angle density of the scattering that produced this path's current
// ray, or zero when there was none to speak of -- a camera ray, or a delta
// closure, both of which take the environment in full.
//
// It exists for multiple importance sampling against every emitter a scattered
// ray can reach, which is the environment and -- since they became opaque
// emitters intersected in closed form -- the analytic lights as well. Zero means
// no competing strategy: a camera ray, or a delta closure, which next-event
// estimation skips entirely and which therefore takes the emitter in full.
layout(set = 0, binding = 21, scalar) buffer PathScatterPdf { float values[]; } pathScatterPdf;

// The interior medium a path is currently inside, as an absorption coefficient
// per unit distance, or zero in vacuum.
//
// Absorption is a property of the *volume between* surfaces, not of any surface,
// so it cannot live in a closure's response: the closure that publishes it is
// evaluated where the path enters, and the light it removes is removed over the
// flight that follows. Carrying it on the path is what lets `extend` apply
// Beer-Lambert over the distance it just measured.
// Held as an extinction and a bounded colour, rather than as the absorption and
// scattering coefficients a closure authors, because a coefficient pair does not
// survive being resolved to wavelengths: each half goes through its own fit and
// the ratio between them -- which is the whole colour of a subsurface material
// -- comes back changed. What the colour *means* travels with it in `w`, since
// the two closures that publish an interior describe it differently.
//   values[2*path + 0] = extinction rgb, anisotropy in w
//   values[2*path + 1] = colour rgb, and in w:
//                          0  the path is outside
//                          1  inside, with no interior to transport through
//                          2  inside; the colour is a single-scattering albedo
//                          3  inside; the colour is the reflectance that comes
//                             back out, to be inverted per wavelength
//
// One is not a degenerate case of two. A clear dielectric encloses nothing and
// still has an inside, which is the side a closure reads its relative index
// from; a medium that scatters is a separate claim on top of that.
#define HDCLAUDE_INSIDE 1.0
layout(set = 0, binding = 25, scalar) buffer PathMedium { vec4 values[]; } pathMedium;

// Whether this path has already been collapsed onto its hero wavelength.
//
// A dispersive surface refracts each wavelength into a different direction, and
// a path can only take one of them, so the first such event keeps the hero lane
// and terminates the other three -- with the survivor scaled by the lane count,
// because the film divides the packet by it (`film.comp.glsl`).
//
// The flag exists because that compensation must happen exactly *once*. A ray
// entering a glass slab and leaving it shades the same dispersive material
// twice, and scaling twice would make the second crossing four times too
// bright. Nothing else on the path records that the packet is already a single
// wavelength: three zero lanes are what a terminated path looks like too.
//
//   0  the packet is intact, all four lanes live
//   1  hero only, and already compensated
layout(set = 0, binding = 26, scalar) buffer PathHeroOnly { uint values[]; } pathHeroOnly;

// Normalised device depth of the primary hit, one float per pixel.
//
// A guide rather than an accumulator: every sample writes it and the last one
// wins, which is deterministic because samples run in order. Averaging depth
// across jittered samples would give, at a silhouette, a value lying on neither
// surface -- worse for anything consuming it than either surface alone.
layout(set = 0, binding = 27, scalar) buffer GuideDepth { float values[]; } guideDepth;

// Screen-space motion of the primary hit, in pixels, one vec2 per pixel.
//
// Where the surface seen at this pixel *was* on the previous frame, minus where
// it is now -- the displacement a reconstructor adds to a pixel's coordinate to
// find its history. Zero where nothing was hit, and zero on a frame with no
// previous one.
layout(set = 0, binding = 28, scalar) buffer GuideMotion { vec2 values[]; } guideMotion;

// The primary surface's reconstruction guides, three vec4 per pixel.
//
//   [3i]    world-space shading normal, and linear roughness in w
//   [3i+1]  diffuse albedo
//   [3i+2]  specular albedo
//
// Definitions are NVIDIA's (docs/dlss-integration.md 4) and the values are what
// the MaterialX closures report. Like depth, every sample writes them and the
// last one wins. A miss holds NVIDIA's sky defaults: no normal, no roughness,
// a diffuse albedo of one half and no specular albedo.
layout(set = 0, binding = 29, scalar) buffer GuideSurface { vec4 values[]; } guideSurface;

// The specular hit distance guide and the probe ray it is measured along, two
// vec4 per pixel:
//
//   [2i]    probe origin, and 1 in w where `shade` recorded a probe
//   [2i+1]  probe direction, and the distance it met in w
//
// `guides` clears every pixel to no probe and NVIDIA's sky value (FP16_MAX),
// `shade` records a probe at each primary hit, and `specular_hit` traces them.
layout(set = 0, binding = 30, scalar) buffer GuideSpecularRay { vec4 values[]; } guideSpecularRay;



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

    // How many rays this call has traced, accumulated on the device.
    //
    // Not read during a frame -- that is the stall this whole design exists to
    // avoid -- and read exactly once after it, which is what `MaterialCounts`
    // already does. A path tracer's cost is its rays, and until these existed
    // the only ray count anybody could state was the camera's, which is the one
    // number that needs no counting.
    //
    // They live past byte 16 on purpose: the inter-bounce reset fills bytes 4
    // to 16 and would otherwise clear them every bounce.
    uint tracedRays;
    uint shadowRays;

    /// A hash over every hit this call resolved: which instance, which
    /// triangle, and which path found it.
    ///
    /// Accumulated with `atomicAdd`, so it does not depend on the order paths
    /// take through the queues -- which varies run to run and is not the thing
    /// under test. What it *is* under test is whether two runs resolve the same
    /// geometry for the same rays. If the hash agrees and the image does not,
    /// the difference is in shading; if the hash disagrees, it is traversal or
    /// the structure being traversed.
    ///
    /// A sum can collide. It is summing a hash of some hundreds of millions of
    /// hits and being asked a yes-or-no question, and a collision would have to
    /// be contrived rather than merely unlucky.
    uint hitHash;

    /// The same, over the rays rather than over what they found: a hash of the
    /// origin and direction bits of every ray this call traced.
    ///
    /// The two together separate the last pair of candidates for the
    /// cross-process nondeterminism. Identical ray counts say nothing about
    /// identical ray values, so if this agrees between two processes and the
    /// hit hash does not, the same ray was told it hit different geometry and
    /// the acceleration structure is the cause; if this disagrees, the
    /// divergence is upstream of traversal and the structure is innocent.
    uint rayHash;
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
    vec4 contribution;   // per lane
    float maxDistance;
    uint path;
    /// The light's `shadowLink` category: only geometry in it occludes this
    /// ray. -1 when every piece of geometry does.
    int shadowLink;
    uint pad1;
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
layout(buffer_reference, scalar) readonly buffer SegmentBuffer  { float values[]; };
layout(buffer_reference, scalar) readonly buffer IndexBuffer    { uint values[]; };
layout(buffer_reference, scalar) readonly buffer NormalBuffer   { vec3 values[]; };
layout(buffer_reference, scalar) readonly buffer UvBuffer       { vec2 values[]; };
layout(buffer_reference, scalar) readonly buffer TriMaterialBuffer { uint values[]; };
layout(buffer_reference, scalar) readonly buffer SplatBuffer    { float values[]; };
layout(buffer_reference, scalar) readonly buffer MajorantBuffer { float values[]; };
layout(buffer_reference, scalar) readonly buffer GridOffsetBuffer { uint values[]; };
layout(buffer_reference, scalar) readonly buffer GridIndexBuffer { uint values[]; };
layout(buffer_reference, scalar) readonly buffer HarmonicBuffer { float values[]; };

/// Per-instance geometry, reached by device address so that adding a prototype
/// does not touch any descriptor set.
struct InstanceGeometry {
    uint64_t positions;
    uint64_t indices;
    /// Curve segments, eight floats each: start xyz, start radius, end xyz, end
    /// radius. Non-zero exactly when this instance is a curve set, which is how
    /// every kernel tells the two kinds of geometry apart -- there is no flag,
    /// because the buffer's presence *is* the fact.
    uint64_t segments;
    uint64_t normals;   // zero if the mesh has no normals at all
    uint64_t uvs;       // zero if the mesh has no texture coordinates
    // Per-triangle material, from GeomSubsets. Zero when every triangle uses
    // the instance's own binding, which is the common case.
    uint64_t triangleMaterials;
    /// Particles, thirteen floats each: centre, opacity, then the nine of
    /// inv(S) transpose(R). Non-zero exactly when this instance is a Gaussian
    /// splat cloud, which is how every kernel tells it from a mesh and a curve
    /// set -- the buffer's presence is the fact, as it is for `segments`.
    uint64_t splats;
    /// Spherical-harmonics coefficients, particle-major: three floats a
    /// coefficient, (degree+1)^2 coefficients a particle.
    uint64_t harmonics;
    /// This cloud's majorant grid, a `SplatVolume`, or zero. Per cloud rather
    /// than per instance, so a mesh does not carry a splat cloud's grid.
    uint64_t splatVolume;
    mat3x4 objectToWorld;
    mat3x4 worldToObject;
    // The previous frame's placement. With worldToObject it takes a hit point
    // back to where that surface was, which is what a motion vector measures.
    mat3x4 previousObjectToWorld;
    uint material;
    /// 1 when `uvs` holds one coordinate per triangle *corner* rather than per
    /// vertex, which is how a face-varying primvar arrives. A UV seam cannot
    /// be expressed any other way, and neither can a textured quad whose four
    /// vertices carry six coordinates.
    uint uvsPerCorner;
    /// 1 when `normals` holds one normal per triangle *corner* rather than per
    /// vertex. That is how a face-varying or uniform primvar arrives, and it
    /// is the only way a hard edge can be expressed: the two sides of a crease
    /// need different normals at the same vertex.
    uint normalsPerCorner;
    /// The degree every particle of a splat cloud shares.
    uint harmonicsDegree;
    /// Which spatial basis function the particles instantiate. Matches
    /// `hdclaude::SplatKernel`: 0 ellipsoid, 1 Gaussian surflet, 2 constant
    /// surflet.
    uint splatKernel;
    /// Explicit padding, so neither this struct nor its C++ mirror has any
    /// *implicit* padding: nine addresses, three matrices, six words, 240
    /// bytes. See the mirror in path_tracer.cpp for why that matters.
    uint pad1;
};

/// What a hit record's first component holds when the ray was stopped by a
/// Gaussian splat, with the instance in `.y` and the particle in `.z`.
///
/// A band of its own rather than an instance index, because a splat hit is not
/// shaded: it has no material, and routing it through the per-material sort
/// would hand it the fallback material and shade a particle as though it were a
/// surface. Negative, so the sort and the shading kernel skip it exactly as they
/// skip a miss, and tested *before* the analytic-light band -- a light is encoded
/// as `-2 - index`, which is also negative.
const int HDCLAUDE_HIT_SPLAT = int(0x80000000u);

const uint HDCLAUDE_SPLAT_KERNEL_ELLIPSOID = 0u;
const uint HDCLAUDE_SPLAT_KERNEL_GAUSSIAN_SURFLET = 1u;
const uint HDCLAUDE_SPLAT_KERNEL_CONSTANT_SURFLET = 2u;

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
    /// Blackbody temperature in kelvin, or zero. A temperature is a spectrum,
    /// so it is carried as one rather than resolved to a tint on the host.
    float colorTemperature;
    /// Equates the peak-normalised blackbody's luminous power with the default
    /// illuminant's, so a temperature tints without brightening.
    float temperatureScale;
    /// Whether *this* light's shape is rendered, as the asset asked. Ignored
    /// when `frame.lightGeometry` is 0, which removes every light's geometry
    /// regardless: the global setting can only take geometry away, never add
    /// it back. Takes the slot a pad held.
    uint  visibleGeometry;
    /// The light-linking category this light illuminates, or -1 for every
    /// surface (UsdLux `collection:lightLink`).
    int   lightLink;
    /// The category of geometry that occludes it, or -1 for all geometry
    /// (UsdLux `collection:shadowLink`).
    int   shadowLink;
};
layout(set = 0, binding = 15, scalar) readonly buffer LightTable {
    Light values[];
} lights;

// --- Light linking ----------------------------------------------------------
//
// UsdLux links a light to geometry through two collections on the light:
// `lightLink`, the geometry it illuminates, and `shadowLink`, the geometry that
// casts its shadows. Hydra resolves each non-trivial collection into a category
// and tells every rprim which categories include it; a light names a category
// by index, and -1 is the collection that includes everything.
//
// Membership is one bit a category, `frame.linkWords` words an instance,
// indexed by the instance's custom index.
layout(set = 0, binding = 31, scalar) readonly buffer InstanceLinks {
    uint values[];
} instanceLinks;

// The instance each path last scattered from, or -1 while the path is still a
// camera ray. An emitter a scattered ray reaches lights the surface the ray
// left, so that surface is the one its light link is asked about.
layout(set = 0, binding = 32, scalar) buffer PathLastInstance {
    int values[];
} pathLastInstance;

/// Whether `instance` belongs to link `category`.
///
/// A category of -1 is the collection that includes everything, so every
/// instance belongs to it; so does no instance at all, which is what a camera
/// ray has -- linking decides what a light does to a surface, and a camera ray
/// has not left one.
bool hdclaude_linked(int instance, int category)
{
    if (category < 0 || instance < 0)
    {
        return true;
    }
    uint word = instanceLinks.values[uint(instance) * frame.linkWords +
                                     uint(category) / 32u];
    return ((word >> (uint(category) % 32u)) & 1u) != 0u;
}

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

/// The dome map's sampling distribution: a marginal CDF over rows, a
/// conditional CDF per row, and the density those two sample from, all in one
/// buffer (`hdclaude/gpu/environment_distribution.h`).
layout(set = 0, binding = 22, scalar) readonly buffer EnvironmentDistribution {
    float values[];
} environmentDistribution;

/// The hero wavelengths this path carries, in nanometres, fixed at ray
/// generation and held for its life.
layout(set = 0, binding = 23, scalar) buffer PathWavelengths {
    vec4 values[];
} pathWavelengths;

/// Everything spectral the kernels need, in one buffer.
///
/// Laid out as `spectralSamples` groups of four -- xbar, ybar, zbar, D65 -- at
/// 5 nm from 360 nm, followed at `chromaTableOffset` by the chromaticity table
/// (`hdclaude/core/spectrum.h`).
///
/// Sampled rather than evaluated in closed form on purpose. The colour matching
/// functions and the illuminant both already exist on the host, where they are
/// the definitions the upsampling fit and its round-trip gate are written
/// against; a second closed form here would be a second thing to keep in
/// agreement with them, and a disagreement would show up as every material
/// being a slightly different colour than its own unit test says.
layout(set = 0, binding = 24, scalar) readonly buffer SpectralTables {
    float values[];
} spectralTables;

// --- Spectral -----------------------------------------------------------

const float kHdclaudeLambdaMin = 360.0;
const float kHdclaudeLambdaMax = 830.0;

/// One row of the sampled tables, linearly interpolated.
///
/// Returns (xbar, ybar, zbar, D65). Outside the visible range everything is
/// zero, which is what makes a wavelength the sampler cannot produce contribute
/// nothing rather than an extrapolated tail.
vec4 hdclaude_spectral_row(float lambda)
{
    if (lambda < kHdclaudeLambdaMin || lambda > kHdclaudeLambdaMax)
    {
        return vec4(0.0);
    }
    float position = (lambda - kHdclaudeLambdaMin) / 5.0;
    uint last = frame.spectralSamples - 1u;
    uint index = min(uint(position), last);
    uint next = min(index + 1u, last);
    float t = position - float(index);

    vec4 low = vec4(spectralTables.values[index * 4u + 0u],
                    spectralTables.values[index * 4u + 1u],
                    spectralTables.values[index * 4u + 2u],
                    spectralTables.values[index * 4u + 3u]);
    vec4 high = vec4(spectralTables.values[next * 4u + 0u],
                     spectralTables.values[next * 4u + 1u],
                     spectralTables.values[next * 4u + 2u],
                     spectralTables.values[next * 4u + 3u]);
    return mix(low, high, t);
}

/// Density of the wavelength sampler, matching `VisibleWavelengthPdf`.
float hdclaude_wavelength_pdf(float lambda)
{
    if (lambda < kHdclaudeLambdaMin || lambda > kHdclaudeLambdaMax)
    {
        return 0.0;
    }
    float c = cosh(0.0072 * (lambda - 538.0));
    return 0.0039398042 / (c * c);
}

/// A correlated hero packet from one uniform sample, matching
/// `SampleHeroWavelengths`.
vec4 hdclaude_sample_hero(float u)
{
    float hero = 538.0 - (1.0 / 0.0072) *
                             atanh(0.8569106254 - 1.8275019724 * clamp(u, 0.0, 1.0));
    const float range = kHdclaudeLambdaMax - kHdclaudeLambdaMin;
    const float stride = range * 0.25;

    vec4 lambda;
    lambda.x = hero;
    lambda.y = hero + stride;
    lambda.z = hero + 2.0 * stride;
    lambda.w = hero + 3.0 * stride;
    // Wrapped, which is what keeps the rotation a bijection of the visible
    // range onto itself -- and therefore what makes every lane's density the
    // hero's own.
    lambda.y -= lambda.y > kHdclaudeLambdaMax ? range : 0.0;
    lambda.z -= lambda.z > kHdclaudeLambdaMax ? range : 0.0;
    lambda.w -= lambda.w > kHdclaudeLambdaMax ? range : 0.0;
    return lambda;
}

/// The bounded sigmoid the upsampling model is built on.
vec4 hdclaude_reflectance_sigmoid(vec4 x)
{
    return 0.5 * (1.0 + x * inversesqrt(1.0 + x * x));
}

/// The sigmoid coefficients for a chromaticity, read from the table.
///
/// `rgb` is expected to have its largest component at one. Bilinear in the two
/// free ratios, exact in the face, which is the same lookup `LookUpChroma`
/// performs on the host so the two describe one function.
vec3 hdclaude_chroma_coefficients(vec3 chroma, uint face)
{
    vec2 ratio;
    if (face == 0u)      { ratio = vec2(chroma.y, chroma.z); }
    else if (face == 1u) { ratio = vec2(chroma.x, chroma.z); }
    else                 { ratio = vec2(chroma.x, chroma.y); }

    uint size = frame.chromaTableSize;
    float last = float(size - 1u);
    vec2 position = clamp(ratio, vec2(0.0), vec2(1.0)) * last;
    uvec2 low = uvec2(min(uint(position.x), size - 1u),
                      min(uint(position.y), size - 1u));
    uvec2 high = uvec2(min(low.x + 1u, size - 1u), min(low.y + 1u, size - 1u));
    vec2 t = position - vec2(low);

    uint base = frame.chromaTableOffset + face * size * size * 3u;
    vec3 result;
    for (uint component = 0u; component < 3u; ++component)
    {
        float c00 = spectralTables.values[base + (low.y * size + low.x) * 3u + component];
        float c10 = spectralTables.values[base + (low.y * size + high.x) * 3u + component];
        float c01 = spectralTables.values[base + (high.y * size + low.x) * 3u + component];
        float c11 = spectralTables.values[base + (high.y * size + high.x) * 3u + component];
        result[component] = mix(mix(c00, c10, t.x), mix(c01, c11, t.x), t.y);
    }
    return result;
}

/// Upsample an authored RGB to the four lanes.
///
/// The magnitude is divided out and reapplied, so this is bounded by that
/// magnitude and never negative -- a reflectance cannot create energy, and an
/// emitter of any brightness is expressible. The same decomposition the host
/// fitter uses (`hdclaude/core/spectrum.h`).
vec4 hdclaude_upsample(vec3 rgb, vec4 lambda)
{
    vec3 clamped = max(rgb, vec3(0.0));
    float scale = max(clamped.r, max(clamped.g, clamped.b));
    if (!(scale > 0.0))
    {
        return vec4(0.0);
    }
    uint face = clamped.r >= clamped.g
                    ? (clamped.r >= clamped.b ? 0u : 2u)
                    : (clamped.g >= clamped.b ? 1u : 2u);

    vec3 c = hdclaude_chroma_coefficients(clamped / scale, face);
    vec4 t = (lambda - kHdclaudeLambdaMin) / (kHdclaudeLambdaMax - kHdclaudeLambdaMin);
    return scale * hdclaude_reflectance_sigmoid((c.x * t + c.y) * t + c.z);
}

/// Van de Hulst's inversion: the single-scattering albedo a walk must be given
/// so that a semi-infinite half-space of it reflects `reflectance`.
///
/// Quoted from OpenPBR, which states it in closed form:
///
///     a = (1 - s^2) / (1 - g s^2)
///     s = 4.09712 + 4.20863 C - sqrt(9.59217 + 41.6808 C + 17.7126 C^2)
///
/// Applied here, per wavelength, and not on the host or in the closure. The
/// relation is steeply nonlinear -- a reflectance of 0.6 needs collisions that
/// survive 95 per cent of the time -- so inverting an RGB triple and upsampling
/// the result gives a walk whose returned spectrum projects to some colour
/// other than the authored one. Upsampling the reflectance and inverting each
/// lane makes the walk's reflectance at every wavelength exactly the authored
/// spectrum, whose projection is exactly the authored colour.
///
/// Mirrors hdclaude::SubsurfaceSingleScatteringAlbedo, which is checked against
/// the forward relation it inverts.
vec4 hdclaude_subsurface_albedo(vec4 reflectance, float g)
{
    vec4 c = clamp(reflectance, vec4(0.0), vec4(1.0));
    float anisotropy = clamp(g, -0.99, 0.99);

    // The fit overshoots by about a thousandth at each end, so `s` is clamped:
    // at C = 1 it reaches -0.00087, which would return an albedo above one and
    // make a lossless medium gain light at every collision.
    vec4 s = clamp(4.09712 + 4.20863 * c -
                       sqrt(9.59217 + 41.6808 * c + 17.7126 * c * c),
                   vec4(0.0), vec4(1.0));

    vec4 sSquared = s * s;
    return clamp((vec4(1.0) - sSquared) /
                     max(vec4(1.0) - anisotropy * sSquared, vec4(1.0e-6)),
                 vec4(0.0), vec4(1.0));
}

/// Planck's law, normalised to a peak of one.
///
/// Matching `NormalizedBlackbody` on the host, with the peak found by Wien's
/// displacement law rather than by a search. Written with the constants in
/// nanometre-kelvin so the exponent stays in a range float handles: at 1667 K
/// and 360 nm it is about 24, and at 25000 K and 830 nm about 0.7.
float hdclaude_blackbody(float lambda, float kelvin)
{
    const float c2 = 1.4387769e7;   // hc/k, in nm K
    float peak = 2.8977721e6 / kelvin;
    float x = c2 / (lambda * kelvin);
    float xPeak = c2 / (peak * kelvin);
    float l = peak / lambda;
    // The ratio of two Planck evaluations, with the fifth power written as a
    // ratio so neither term overflows on its own.
    return l * l * l * l * l * (exp(xPeak) - 1.0) / (exp(x) - 1.0);
}

/// The illuminant an emitter's authored RGB is referred to.
///
/// D65 by default, because that is what an RGB colour means in an sRGB
/// pipeline. A light with a colour temperature is referred to its *blackbody*
/// instead -- it emits Planck's law, tinted by whatever colour was authored --
/// and the scale keeps its luminous power the same, so the control tints
/// without brightening.
///
/// Replacing the illuminant rather than multiplying by it is the point. A light
/// at 2700 K does not emit daylight through an amber filter; it emits a 2700 K
/// spectrum, and the difference shows on any surface whose reflectance varies
/// across the spectrum, which is every real one.
vec4 hdclaude_emitter_illuminant(vec4 lambda, float kelvin, float scale)
{
    if (kelvin > 0.0)
    {
        return scale * vec4(hdclaude_blackbody(lambda.x, kelvin),
                            hdclaude_blackbody(lambda.y, kelvin),
                            hdclaude_blackbody(lambda.z, kelvin),
                            hdclaude_blackbody(lambda.w, kelvin));
    }
    return vec4(hdclaude_spectral_row(lambda.x).w,
                hdclaude_spectral_row(lambda.y).w,
                hdclaude_spectral_row(lambda.z).w,
                hdclaude_spectral_row(lambda.w).w);
}

/// Beer-Lambert transmittance through `distance` of a medium, on the four lanes.
///
/// The transmittance is upsampled rather than the coefficient. `exp(-sigma * d)`
/// is bounded in (0, 1] whatever the coefficient is, which is exactly the range
/// the reflectance fit is built for and guaranteed on; an absorption coefficient
/// is unbounded and has no such fit. It also keeps the one rule this renderer
/// has about colour -- RGB becomes spectral at the closure boundary and nowhere
/// else.
vec4 hdclaude_transmittance(vec3 absorption, float distance, vec4 lambda)
{
    vec3 transmittance = exp(-max(absorption, vec3(0.0)) * max(distance, 0.0));
    return hdclaude_upsample(transmittance, lambda);
}

/// Upsample an authored *emission* RGB to the four lanes.
///
/// The upsampled chromaticity times the illuminant the colour is referred to.
/// That is what makes a white surface under a white light come back white,
/// since the film divides by the default illuminant's luminous integral.
vec4 hdclaude_upsample_emission(vec3 rgb, vec4 lambda, float kelvin, float scale)
{
    return hdclaude_upsample(rgb, lambda) *
           hdclaude_emitter_illuminant(lambda, kelvin, scale);
}

/// The same, for an emitter with no colour temperature of its own.
vec4 hdclaude_upsample_emission(vec3 rgb, vec4 lambda)
{
    return hdclaude_upsample_emission(rgb, lambda, 0.0, 1.0);
}

/// How many emitters next-event estimation chooses between.
///
/// The analytic lights plus the environment, which is sampled as one more
/// emitter rather than left to be found by a scattered ray. A path in an
/// enclosed set -- which is what every studio-lit interior is -- otherwise sees
/// the sky only through a chain of bounces that survives to a miss, and the
/// image is dark and noisy for want of a shadow ray it never cast.
/// Whether the stand-in sun lights this frame.
///
/// Only when the stage lit itself in no way at all. A dome light is not in the
/// light table -- it supplies the environment rather than an entry -- so
/// counting the table alone says a dome-lit stage has no lights, and the sun
/// was being added on top of an environment the asset had authored. The Open
/// Chess Set is exactly that stage, and its stand-in sun was a second key light
/// nobody asked for.
bool hdclaude_has_stand_in_sun()
{
    return frame.lightCount == 0u && frame.hasDomeLight == 0u;
}

uint hdclaude_emitter_count()
{
    // The analytic lights, the environment, and -- only when the stage lights
    // itself in no way at all -- the stand-in sun.
    return frame.lightCount + (hdclaude_has_stand_in_sun() ? 2u : 1u);
}

/// Where `direction` lands in the dome map's parameterisation.
///
/// The inverse of the lookup in `hdclaude_environment` below, and the two are
/// only ever right together: a sampler that walks the map in one
/// parameterisation and a density evaluated in another agree nowhere.
vec2 hdclaude_dome_uv(vec3 direction, out float sinTheta)
{
    vec3 d = normalize((frame.domeWorldToLight * vec4(direction, 0.0)).xyz);
    float theta = acos(clamp(d.y, -1.0, 1.0));
    sinTheta = sin(theta);
    float u = fract((atan(d.z, d.x) + 1.57079632679) * (1.0 / 6.28318530718));
    float v = 1.0 - theta * (1.0 / 3.14159265359);
    return vec2(u, v);
}

/// The first bin whose CDF interval contains `u`.
///
/// `base` is the index of the CDF's leading zero and there are `count + 1`
/// entries, so the answer is in [0, count).
uint hdclaude_cdf_search(uint base, uint count, float u)
{
    uint low = 0u;
    uint high = count;
    while (low < high)
    {
        uint mid = (low + high) >> 1u;
        if (environmentDistribution.values[base + mid + 1u] <= u)
        {
            low = mid + 1u;
        }
        else
        {
            high = mid;
        }
    }
    return min(low, count - 1u);
}

/// Density in (u, v) measure of the bin containing `uv`.
float hdclaude_environment_density(vec2 uv)
{
    uint x = min(uint(uv.x * float(frame.environmentWidth)),
                 frame.environmentWidth - 1u);
    uint y = min(uint(uv.y * float(frame.environmentHeight)),
                 frame.environmentHeight - 1u);
    return environmentDistribution.values[frame.environmentDensity +
                                          y * frame.environmentWidth + x];
}

/// Density of choosing `direction` by environment sampling, in solid angle,
/// including the chance of having chosen the environment among the emitters.
///
/// Deliberately a function of the direction alone: the kernel that needs this
/// for a *scattered* ray has a direction and a miss and no surface, so a
/// density it could not recompute there would not be usable as an MIS weight
/// at all. A textured dome is sampled by luminance, an untextured one -- which
/// is uniform, and for which no distribution exists -- over the sphere.
///
/// The change of measure is the map's own Jacobian: solid angle is
/// `sin(theta) dtheta dphi`, and (u, v) spans `dtheta = pi dv`,
/// `dphi = 2 pi du`, so `domega = 2 pi^2 sin(theta) du dv`.
float hdclaude_environment_pdf(vec3 direction)
{
    float selection = 1.0 / float(hdclaude_emitter_count());
    if (frame.hasEnvironmentDistribution == 0u)
    {
        return (1.0 / (4.0 * 3.14159265359)) * selection;
    }

    float sinTheta;
    vec2 uv = hdclaude_dome_uv(direction, sinTheta);
    if (sinTheta <= 1.0e-6)
    {
        // Straight up or straight down: the parameterisation is singular there
        // and no finite density describes it. Reporting zero costs the MIS
        // weight nothing -- it becomes one for the strategy that can reach it.
        return 0.0;
    }
    return hdclaude_environment_density(uv) /
           (2.0 * 3.14159265359 * 3.14159265359 * sinTheta) * selection;
}

/// A direction sampled from the dome map's luminance, with its density.
///
/// `pdf` is the solid-angle density *without* the emitter-selection factor,
/// matching what `hdclaude_sample_light` reports, so the shade kernel applies
/// the selection probability once for every emitter alike.
struct EnvironmentSample {
    vec3  direction;
    float pdf;
};

EnvironmentSample hdclaude_sample_environment(vec2 xi)
{
    EnvironmentSample result;

    if (frame.hasEnvironmentDistribution == 0u)
    {
        // No map, or a map with no light in it: uniform over the sphere, which
        // is exactly right for a constant environment.
        float z = 1.0 - 2.0 * xi.x;
        float r = sqrt(max(0.0, 1.0 - z * z));
        float phi = 6.28318530718 * xi.y;
        result.direction = vec3(r * cos(phi), r * sin(phi), z);
        result.pdf = 1.0 / (4.0 * 3.14159265359);
        return result;
    }

    // The row, then the column within it. The leftover of each search is
    // reused as the position *inside* the bin, so a bin is sampled uniformly
    // rather than at its edge and no second random number is needed.
    uint y = hdclaude_cdf_search(0u, frame.environmentHeight, xi.x);
    float yLow = environmentDistribution.values[y];
    float yHigh = environmentDistribution.values[y + 1u];
    float dy = yHigh > yLow ? (xi.x - yLow) / (yHigh - yLow) : 0.5;

    uint conditional =
        frame.environmentConditional + y * (frame.environmentWidth + 1u);
    uint x = hdclaude_cdf_search(conditional, frame.environmentWidth, xi.y);
    float xLow = environmentDistribution.values[conditional + x];
    float xHigh = environmentDistribution.values[conditional + x + 1u];
    float dx = xHigh > xLow ? (xi.y - xLow) / (xHigh - xLow) : 0.5;

    vec2 uv = vec2((float(x) + dx) / float(frame.environmentWidth),
                   (float(y) + dy) / float(frame.environmentHeight));

    // The inverse of hdclaude_dome_uv, in the dome's frame, then back to world.
    float theta = (1.0 - uv.y) * 3.14159265359;
    float sinTheta = sin(theta);
    float phi = 6.28318530718 * uv.x - 1.57079632679;
    vec3 inLight = vec3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
    result.direction =
        normalize((frame.domeLightToWorld * vec4(inLight, 0.0)).xyz);

    result.pdf = sinTheta > 1.0e-6
                     ? hdclaude_environment_density(uv) /
                           (2.0 * 3.14159265359 * 3.14159265359 * sinTheta)
                     : 0.0;
    return result;
}

/// Radiance leaving the scene along `direction`.
vec3 hdclaude_environment(vec3 direction)
{
    if (frame.hasDomeTexture == 0u)
    {
        return frame.environmentColor.rgb;
    }

    // Latitude-longitude, in the dome's own frame, in the orientation USD
    // defines -- `u = (atan2(z, x) + pi/2) / 2pi`, which is what
    // hdSt/shaders/domeLight.glslfx samples with and therefore what an authored
    // HDRI is framed against. hdClaude wrapped u about -Z instead, which is the
    // same map rotated by half a turn: every dome-lit scene showed the wall
    // behind the camera instead of the one in front of it.
    //
    // The decoded image rows run bottom-up, so v = 1 is straight up.
    vec3 d = normalize((frame.domeWorldToLight * vec4(direction, 0.0)).xyz);
    float u = (atan(d.z, d.x) + 1.57079632679) * (1.0 / 6.28318530718);
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

// --- Gaussian splats --------------------------------------------------------
//
// The device half of include/hdclaude/core/gaussian_splats.h. Every formula here
// has a host twin the unit tests assert against -- the peak response along a
// ray, the support radius, the spherical-harmonics basis -- and the two are
// written the same way on purpose, because a renderer whose gate is a closed
// form and whose kernel is a second derivation of it gates nothing.
//
// A hit is never composited and never sorted. A particle stops a ray with
// probability equal to its opacity times the kernel's falloff, the nearest one
// that does is what the ray sees, and the expectation of that is exactly the
// front-to-back alpha compositing the schema defines. See
// docs/gaussian-splats.md 3.

/// How far a kernel's support reaches, in units of its own sigma.
///
/// Three for either Gaussian, which is the specification's own figure -- "the
/// 3-sigma point is 3.0 and 99.7% of the splat support is within a spherical
/// region of radius 3" -- and one for the constant surflet, whose support it
/// bounds exactly.
float hdclaude_splat_support(uint kernel)
{
    return kernel == HDCLAUDE_SPLAT_KERNEL_CONSTANT_SURFLET ? 1.0 : 3.0;
}

bool hdclaude_splat_flat(uint kernel)
{
    return kernel != HDCLAUDE_SPLAT_KERNEL_ELLIPSOID;
}

/// One particle: its centre, its opacity, and the map into the kernel's own
/// space where the falloff is exp(-0.5 |x|^2).
struct Splat {
    vec3 center;
    float opacity;
    mat3 inverseTransform;
};

Splat hdclaude_splat(uint64_t address, int particle)
{
    SplatBuffer particles = SplatBuffer(address);
    uint base = uint(particle) * 13u;
    Splat splat;
    splat.center = vec3(particles.values[base + 0u], particles.values[base + 1u],
                        particles.values[base + 2u]);
    splat.opacity = particles.values[base + 3u];
    // Row-major on the host; GLSL's mat3 constructor takes columns, so the rows
    // are handed over as columns of the transpose and read back transposed. The
    // alternative is three dot products written out, which is the same work
    // said less clearly.
    splat.inverseTransform = transpose(mat3(
        vec3(particles.values[base + 4u], particles.values[base + 5u],
             particles.values[base + 6u]),
        vec3(particles.values[base + 7u], particles.values[base + 8u],
             particles.values[base + 9u]),
        vec3(particles.values[base + 10u], particles.values[base + 11u],
             particles.values[base + 12u])));
    return splat;
}

/// Where an object-space ray comes closest to a particle in the particle's own
/// metric, and how strongly the kernel responds there.
///
/// The specification says where the kernel is evaluated for a *projection* and
/// says nothing about a ray, so hdClaude evaluates it at its maximum response
/// along the ray: exact for every ray rather than for one projection, and
/// closed-form (docs/gaussian-splats.md 2.2). Returns false when the ray misses
/// the support.
bool hdclaude_splat_peak(Splat splat, uint kernel, vec3 origin, vec3 direction,
                         float tMin, float tMax, out float t, out float response)
{
    vec3 b = splat.inverseTransform * (origin - splat.center);
    vec3 a = splat.inverseTransform * direction;
    float radius = hdclaude_splat_support(kernel);

    if (hdclaude_splat_flat(kernel))
    {
        // A flat kernel has no thickness, so there is one candidate distance
        // rather than a peak: where the ray crosses the plane the disk lies in.
        if (a.z == 0.0)
        {
            return false;
        }
        float hit = -b.z / a.z;
        if (hit < tMin || hit > tMax)
        {
            return false;
        }
        vec2 planar = b.xy + hit * a.xy;
        float distance = length(planar);
        if (distance > radius)
        {
            return false;
        }
        t = hit;
        response = kernel == HDCLAUDE_SPLAT_KERNEL_CONSTANT_SURFLET
                       ? 1.0
                       : exp(-0.5 * distance * distance);
        return true;
    }

    float aa = dot(a, a);
    float ab = dot(a, b);
    float bb = dot(b, b);
    float peak = aa > 0.0 ? -ab / aa : tMin;
    peak = clamp(peak, tMin, tMax);

    float squared = max(0.0, bb + peak * (2.0 * ab + peak * aa));
    if (squared > radius * radius)
    {
        return false;
    }
    t = peak;
    response = exp(-0.5 * squared);
    return true;
}

/// The radiance a particle emits along `direction`.
///
/// The normalised real spherical harmonics, l-major and m-ascending, evaluated
/// by the same Legendre recurrence the host uses -- rather than the four bands
/// written out longhand, because two derivations of one basis is two things to
/// keep in agreement and the recurrence handles any degree an asset carries.
///
/// `direction` points from the particle *toward the viewer*. Nothing in the
/// schema says which way round it should be; this is hdClaude's choice, it is
/// recorded in docs/gaussian-splats.md 2.3, and the test stage asserts it.
vec3 hdclaude_splat_radiance(uint64_t address, uint degree, int particle,
                             vec3 direction)
{
    HarmonicBuffer coefficients = HarmonicBuffer(address);
    uint count = (degree + 1u) * (degree + 1u);
    uint base = uint(particle) * count * 3u;

    float length2 = dot(direction, direction);
    if (length2 <= 0.0)
    {
        // No direction at all. The DC term is the only one that does not depend
        // on one, and returning it is the honest answer rather than black.
        float y00 = 0.28209479177387814;
        return y00 * vec3(coefficients.values[base + 0u], coefficients.values[base + 1u],
                          coefficients.values[base + 2u]);
    }
    vec3 d = direction * inversesqrt(length2);

    float sinTheta = sqrt(max(0.0, 1.0 - d.z * d.z));
    // At a pole the azimuth is undefined and every term that would use it is
    // multiplied by a power of sin(theta), so any value does.
    float cosPhi = sinTheta > 0.0 ? d.x / sinTheta : 1.0;
    float sinPhi = sinTheta > 0.0 ? d.y / sinTheta : 0.0;

    vec3 radiance = vec3(0.0);
    float cosMPhi = 1.0;
    float sinMPhi = 0.0;

    for (uint m = 0u; m <= degree; ++m)
    {
        if (m > 0u)
        {
            float nextCos = cosMPhi * cosPhi - sinMPhi * sinPhi;
            float nextSin = sinMPhi * cosPhi + cosMPhi * sinPhi;
            cosMPhi = nextCos;
            sinMPhi = nextSin;
        }

        // P(m,m) = (-1)^m (2m-1)!! sin(theta)^m, Condon-Shortley phase included,
        // which is what makes this agree with the published closed forms.
        float pmm = 1.0;
        for (uint i = 1u; i <= m; ++i)
        {
            pmm *= -(2.0 * float(i) - 1.0) * sinTheta;
        }

        float previous = 0.0;
        float beforePrevious = 0.0;
        for (uint l = m; l <= degree; ++l)
        {
            float legendre;
            if (l == m)
            {
                legendre = pmm;
            }
            else if (l == m + 1u)
            {
                legendre = d.z * (2.0 * float(m) + 1.0) * previous;
            }
            else
            {
                legendre = (d.z * (2.0 * float(l) - 1.0) * previous -
                            (float(l) + float(m) - 1.0) * beforePrevious) /
                           (float(l) - float(m));
            }
            beforePrevious = previous;
            previous = legendre;

            // sqrt((2l+1)/(4 pi) * (l-m)!/(l+m)!), as a running quotient rather
            // than two factorials, which is what keeps it finite at any degree.
            float normalisation = (2.0 * float(l) + 1.0) / 12.566370614359173;
            for (uint k = l - m + 1u; k <= l + m; ++k)
            {
                normalisation /= float(k);
            }
            normalisation = sqrt(normalisation);

            uint band = l * l + l;
            if (m == 0u)
            {
                uint at = base + band * 3u;
                radiance += normalisation * legendre *
                            vec3(coefficients.values[at + 0u], coefficients.values[at + 1u],
                                 coefficients.values[at + 2u]);
            }
            else
            {
                float scale = 1.4142135623730951 * normalisation * legendre;
                uint positive = base + (band + m) * 3u;
                uint negative = base + (band - m) * 3u;
                radiance += scale * cosMPhi *
                            vec3(coefficients.values[positive + 0u],
                                 coefficients.values[positive + 1u],
                                 coefficients.values[positive + 2u]);
                radiance += scale * sinMPhi *
                            vec3(coefficients.values[negative + 0u],
                                 coefficients.values[negative + 1u],
                                 coefficients.values[negative + 2u]);
            }
        }
    }
    return radiance;
}

/// The coin a particle's coverage is decided by.
///
/// Deliberately *not* drawn from the path's own generator. A ray query reports
/// candidate boxes in whatever order traversal reaches them, so consuming the
/// path's stream inside the candidate loop would make the number of draws -- and
/// therefore every random number the rest of the path uses -- depend on
/// traversal order. That is not a bias, but it is irreproducible, and it would
/// make every existing hash in the test suite depend on a driver's choice of
/// tree walk.
///
/// Hashed from the path, the sample and the particle instead, so the decision is
/// independent per particle, identical whichever order the boxes arrive in, and
/// the same on two runs of the same frame.
float hdclaude_splat_coin(uint path, uint sampleIndex, int instance,
                          int particle)
{
    uint seed = path * 2654435761u;
    seed ^= sampleIndex * 2246822519u;
    seed ^= uint(instance) * 3266489917u;
    seed ^= uint(particle) * 668265263u;
    // Two rounds, because one leaves the low bits of `particle` visible in the
    // result and neighbouring particles are exactly what must not correlate.
    seed = hdclaude_pcg(seed);
    return float(hdclaude_pcg(seed) >> 8) * (1.0 / 16777216.0);
}

// --- The volumetric reading -------------------------------------------------
//
// Coverage above stops a ray at one particle. This is the other reading, in
// which the cloud is a density a path travels *through*. The device half of the
// same functions in include/hdclaude/core/gaussian_splats.h, and the same
// numbers: the extinction that makes an isotropic particle's authored alpha come
// out exactly, and a majorant that bounds the summed density everywhere in a
// cell. See docs/gaussian-splats.md 7.

/// A splat cloud's majorant grid. Mirrors SplatVolume in path_tracer.cpp.
struct SplatVolume {
    vec3 origin;
    vec3 cellSize;
    ivec3 resolution;
    /// The largest density anywhere in the cloud, which delta tracking samples
    /// against.
    float majorantBound;
    uint64_t majorant;
    uint64_t offsets;
    uint64_t indices;
};

layout(buffer_reference, scalar) readonly buffer SplatVolumeBuffer {
    SplatVolume value;
};

/// The cell an object-space point falls in, or -1 outside the grid.
int hdclaude_splat_cell(SplatVolume volume, vec3 point)
{
    vec3 offset = (point - volume.origin) / volume.cellSize;
    ivec3 cell = ivec3(floor(offset));
    if (any(lessThan(cell, ivec3(0))) ||
        any(greaterThanEqual(cell, volume.resolution)))
    {
        return -1;
    }
    return (cell.z * volume.resolution.y + cell.y) * volume.resolution.x + cell.x;
}

/// The bound on the summed extinction anywhere in the cell holding `point`.
///
/// Zero outside the grid, because no particle's support reaches there. Delta
/// tracking is unbiased only while this really bounds the density, which is what
/// the host asserts at a quarter of a million sampled points -- the failure is
/// silent otherwise, and the image simply loses the collisions it should have
/// rejected.
float hdclaude_splat_majorant(SplatVolume volume, vec3 point)
{
    int cell = hdclaude_splat_cell(volume, point);
    if (cell < 0 || volume.majorant == 0ul)
    {
        return 0.0;
    }
    return MajorantBuffer(volume.majorant).values[cell];
}

/// The extinction a particle contributes at its own centre, per unit length.
///
/// The choice recorded in docs/gaussian-splats.md 7.2: exact for an isotropic
/// particle from every direction, matched on the geometric mean of the three
/// scales otherwise. The scales are the lengths of the forward transform's
/// columns, and the forward transform's determinant is the reciprocal of the
/// inverse's -- so the product of the three scales, which is all this needs,
/// comes straight off the determinant with no inversion at all.
float hdclaude_splat_extinction(Splat splat, float maximumOpticalDepth)
{
    float scaled = determinant(splat.inverseTransform);
    if (!(abs(scaled) > 0.0))
    {
        return 0.0;
    }
    // (sx sy sz) = 1 / det(inv(S) transpose(R)), and the mean is its cube root.
    float mean = pow(1.0 / abs(scaled), 1.0 / 3.0);
    if (!(mean > 0.0))
    {
        return 0.0;
    }
    float opacity = clamp(splat.opacity, 0.0, 1.0);
    float depth = opacity >= 1.0 ? maximumOpticalDepth : -log(1.0 - opacity);
    depth = min(depth, maximumOpticalDepth);
    // A ray through the centre of an isotropic particle integrates
    // exp(-0.5 (t/s)^2) over the whole line, which is s sqrt(2 pi).
    return depth / (2.5066282746310002 * mean);
}

/// The summed extinction at an object-space point.
///
/// Reads only the particles the grid says reach this cell, which the host
/// asserts equals the sum over the whole cloud -- a grid that dropped particles
/// at cell boundaries would show as faint seams on a lattice, invisible in a
/// noisy volume.
///
/// The ellipsoid kernel only. A surflet is a disk with no thickness, so it
/// encloses no volume and has no density to speak of -- and the 3DGS prim
/// applies the ellipsoid kernel, which is the only one that reaches here.
float hdclaude_splat_density(InstanceGeometry geometry, SplatVolume volume,
                             vec3 point, float maximumOpticalDepth)
{
    int cell = hdclaude_splat_cell(volume, point);
    if (cell < 0 || volume.offsets == 0ul || volume.indices == 0ul)
    {
        return 0.0;
    }
    GridOffsetBuffer offsets = GridOffsetBuffer(volume.offsets);
    GridIndexBuffer members = GridIndexBuffer(volume.indices);

    float radius = hdclaude_splat_support(geometry.splatKernel);
    float total = 0.0;
    for (uint at = offsets.values[cell]; at < offsets.values[cell + 1]; ++at)
    {
        uint index = members.values[at];
        Splat splat = hdclaude_splat(geometry.splats, int(index));
        vec3 local = splat.inverseTransform * (point - splat.center);
        float squared = dot(local, local);
        if (squared > radius * radius)
        {
            continue;
        }
        total += hdclaude_splat_extinction(splat, maximumOpticalDepth) *
                 exp(-0.5 * squared);
    }
    return total;
}


/// Where a ray enters and leaves a cloud's grid, in the ray's own parameter.
///
/// The slab test, in the cloud's object space. Returns false when the ray misses
/// the grid entirely, which for most rays in most frames is the answer and is
/// why this is the first thing the walk asks.
bool hdclaude_splat_grid_span(SplatVolume volume, vec3 origin, vec3 direction,
                              float tMin, float tMax, out float enter,
                              out float leave)
{
    vec3 lo = volume.origin;
    vec3 hi = volume.origin + vec3(volume.resolution) * volume.cellSize;
    float near = tMin;
    float far = tMax;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-20)
        {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
            {
                return false;
            }
            continue;
        }
        float inverse = 1.0 / direction[axis];
        float first = (lo[axis] - origin[axis]) * inverse;
        float second = (hi[axis] - origin[axis]) * inverse;
        near = max(near, min(first, second));
        far = min(far, max(first, second));
    }
    if (far < near)
    {
        return false;
    }
    enter = near;
    leave = far;
    return true;
}

/// The radiance a collision at `point` emits toward `outgoing`.
///
/// Weighted by each particle's share of the density there, which is what makes
/// it the radiance of the medium rather than of whichever particle happened to
/// be looked up first. A collision happens *because of* the summed density, so
/// the emission it carries is that sum's weighted mean.
vec3 hdclaude_splat_volume_emission(InstanceGeometry geometry,
                                    SplatVolume volume, vec3 point,
                                    vec3 outgoing, float maximumOpticalDepth)
{
    int cell = hdclaude_splat_cell(volume, point);
    if (cell < 0 || volume.offsets == 0ul || volume.indices == 0ul)
    {
        return vec3(0.0);
    }
    GridOffsetBuffer offsets = GridOffsetBuffer(volume.offsets);
    GridIndexBuffer members = GridIndexBuffer(volume.indices);

    float radius = hdclaude_splat_support(geometry.splatKernel);
    vec3 weighted = vec3(0.0);
    float total = 0.0;
    for (uint at = offsets.values[cell]; at < offsets.values[cell + 1]; ++at)
    {
        uint index = members.values[at];
        Splat splat = hdclaude_splat(geometry.splats, int(index));
        vec3 local = splat.inverseTransform * (point - splat.center);
        float squared = dot(local, local);
        if (squared > radius * radius)
        {
            continue;
        }
        float density = hdclaude_splat_extinction(splat, maximumOpticalDepth) *
                        exp(-0.5 * squared);
        if (density <= 0.0)
        {
            continue;
        }
        total += density;
        weighted += density * hdclaude_splat_radiance(geometry.harmonics,
                                                      geometry.harmonicsDegree,
                                                      int(index), outgoing);
    }
    return total > 0.0 ? weighted / total : vec3(0.0);
}

/// Delta tracking through one splat cloud, between `tMin` and `tMax`.
///
/// Samples a flight from the cloud's own majorant and keeps the collision in
/// proportion to the density actually there, which is unbiased for any bound
/// that really bounds -- the host asserts that one does at a quarter of a
/// million points, because the failure is silent and costs exactly the
/// collisions it should have rejected.
///
/// Emission-only, and that is the schema rather than a simplification: a
/// particle carries radiance and no reflectance, so every real collision
/// absorbs and emits and none of them scatters. A bound MaterialX
/// `volumeshader` is what would add scattering, and it is a phase of its own
/// (docs/gaussian-splats.md 7.1).
///
/// The whole walk is done in the cloud's object space with an *unnormalised*
/// direction, so the ray parameter means the same thing in both spaces and no
/// distance has to be converted back. What that costs is one factor: a density
/// per unit object length becomes a density per unit `t` by multiplying by the
/// object-space direction's length.
bool hdclaude_splat_volume_collision(InstanceGeometry geometry,
                                     SplatVolume volume, vec3 worldOrigin,
                                     vec3 worldDirection, float tMin,
                                     float tMax, inout uint rng,
                                     out float hitT, out vec3 emission)
{
    if (volume.majorant == 0ul)
    {
        return false;
    }
    // A row vector times the 3x4, which is how every other kernel takes a point
    // into object space -- and the same form with w = 0 for a direction, which
    // drops the translation.
    vec3 origin = vec4(worldOrigin, 1.0) * geometry.worldToObject;
    vec3 direction = vec4(worldDirection, 0.0) * geometry.worldToObject;

    float scale = length(direction);
    if (!(scale > 0.0))
    {
        return false;
    }

    float enter;
    float leave;
    if (!hdclaude_splat_grid_span(volume, origin, direction, tMin, tMax, enter,
                                  leave))
    {
        return false;
    }

    MajorantBuffer bounds = MajorantBuffer(volume.majorant);

    // Amanatides and Woo, through the grid the bounds are held in. Stepping
    // cell by cell rather than sampling against one bound for the whole cloud
    // is not an optimisation, it is what makes the walk *work*: a real capture
    // has a global majorant thousands of times its typical density, and a walk
    // that samples against it spends every trial it has crossing the first
    // millimetre.
    ivec3 resolution = volume.resolution;
    vec3 atEnter = (origin + direction * enter - volume.origin) / volume.cellSize;
    ivec3 cell = clamp(ivec3(floor(atEnter)), ivec3(0), resolution - 1);

    ivec3 stepping = ivec3(0);
    vec3 tDelta = vec3(1.0e30);
    vec3 tNext = vec3(1.0e30);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-20)
        {
            continue;
        }
        stepping[axis] = direction[axis] > 0.0 ? 1 : -1;
        tDelta[axis] = abs(volume.cellSize[axis] / direction[axis]);
        float boundary = volume.origin[axis] +
                         float(cell[axis] + (stepping[axis] > 0 ? 1 : 0)) *
                             volume.cellSize[axis];
        tNext[axis] = (boundary - origin[axis]) / direction[axis];
    }

    // The walk cannot visit more cells than the grid has along a diagonal, so
    // this bound is the grid's own size rather than a number chosen to feel
    // safe. Running out of it means the ray left the grid, which the tests
    // below already return for.
    int cells = resolution.x + resolution.y + resolution.z + 3;
    float t = enter;
    for (int visited = 0; visited < cells; ++visited)
    {
        float exit = min(min(tNext.x, min(tNext.y, tNext.z)), leave);

        int index = (cell.z * resolution.y + cell.y) * resolution.x + cell.x;
        float majorant = bounds.values[index] * scale;
        if (majorant > 0.0)
        {
            // Within one cell, against that cell's own bound.
            //
            // The cap does *not* force a collision, and an earlier version that
            // did was wrong twice over. A cap is reached when the cell's
            // majorant is far above the density actually in it, not when the
            // cell is opaque -- so calling it a collision invented occlusion,
            // and the point it invented one at had no density, so the emission
            // there was black. It rendered a capture as black cubes one grid
            // cell across. With the majorant bounded per principal axis the cap
            // is not reached by any cloud measured here; if one ever reaches it,
            // it under-occludes that cell rather than fabricating a surface.
            const int kTrialsPerCell = 256;
            float u = t;
            for (int trial = 0; trial < kTrialsPerCell; ++trial)
            {
                u -= log(max(1.0e-8, 1.0 - hdclaude_random(rng))) / majorant;
                if (u >= exit)
                {
                    break;
                }
                vec3 point = origin + direction * u;
                float density =
                    hdclaude_splat_density(geometry, volume, point, 20.0) * scale;
                if (hdclaude_random(rng) * majorant < density)
                {
                    hitT = u;
                    emission = hdclaude_splat_volume_emission(
                        geometry, volume, point, normalize(-direction), 20.0);
                    return true;
                }
            }
        }

        if (exit >= leave)
        {
            return false;
        }
        t = exit;

        // Advance whichever boundary came first.
        if (tNext.x <= tNext.y && tNext.x <= tNext.z)
        {
            cell.x += stepping.x;
            tNext.x += tDelta.x;
        }
        else if (tNext.y <= tNext.z)
        {
            cell.y += stepping.y;
            tNext.y += tDelta.y;
        }
        else
        {
            cell.z += stepping.z;
            tNext.z += tDelta.z;
        }
        if (any(lessThan(cell, ivec3(0))) ||
            any(greaterThanEqual(cell, resolution)))
        {
            return false;
        }
    }
    return false;
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
    /// The emitter's blackbody temperature, or zero for the default
    /// illuminant. Carried on the sample because the shading point upsamples
    /// the radiance and has to know which illuminant it is referred to.
    float colorTemperature;
    float temperatureScale;
    vec3  direction;    // from the surface toward the light, normalised
    float distance;     // to the sampled point; huge for a distant light
    vec3  radiance;     // emitted radiance arriving along `direction`
    float pdf;          // solid-angle density, zero if the sample is unusable
    bool  castsShadows;
};

/// The balance heuristic for two strategies.
///
/// Power-one rather than power-two: the extra sharpening buys little here and
/// the balance heuristic is the one whose optimality is proven.
float hdclaude_mis_weight(float thisPdf, float otherPdf)
{
    const float total = thisPdf + otherPdf;
    return total > 0.0 ? thisPdf / total : 0.0;
}

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

/// Henyey-Greenstein phase function sampling, about `forward`.
///
/// `g` is the asymmetry: 0 is isotropic, positive is forward-scattering. The
/// inversion is the standard one, and the g = 0 branch is written separately
/// because the general form divides by g.
vec3 hdclaude_sample_phase(vec3 forward, float g, vec2 u)
{
    float cosTheta;
    if (abs(g) < 1.0e-3)
    {
        cosTheta = 1.0 - 2.0 * u.x;
    }
    else
    {
        // Henyey-Greenstein, inverted from the form the literature states it
        // in. That form gives the cosine against `wo`, the direction pointing
        // *back* along the ray, because a phase function is conventionally
        // written between two directions that both point away from the vertex.
        // This function is handed the direction of propagation and returns
        // another one, so its cosine is against `forward` and the sign flips.
        //
        // Taking the published formula with a basis built around the direction
        // of travel is a full reversal of the medium: `g` of 1, which OpenPBR
        // defines as fully forward scattering, scattered every ray exactly
        // backwards. The playground's bottle authors exactly that value.
        //
        // Negating is the whole correction, and it is a correction rather than
        // a convention: the Henyey-Greenstein density is symmetric under
        // `(g, cos) -> (-g, -cos)`, so a negated sample of the `wo` form is
        // exactly a sample of the propagation form with the same `g`.
        float term = (1.0 - g * g) / (1.0 + g - 2.0 * g * u.x);
        cosTheta = (1.0 + g * g - term * term) / (2.0 * g);
    }
    cosTheta = clamp(cosTheta, -1.0, 1.0);

    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 6.28318530718 * u.y;
    vec3 t, b;
    hdclaude_light_basis(forward, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) +
                     forward * cosTheta);
}

/// Per-lane extinction from an RGB coefficient, exactly.
///
/// `exp(-sigma * d)` is `(exp(-sigma))^d`, so upsampling the *unit*
/// transmittance -- which is bounded in (0, 1] and so is what the reflectance
/// fit is built for -- and taking its logarithm recovers the coefficient on the
/// hero wavelengths without ever asking the fit about an unbounded quantity.
vec4 hdclaude_lane_extinction(vec3 sigma, vec4 lambda)
{
    vec3 rgb = max(sigma, vec3(0.0));
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (!(peak > 0.0))
    {
        return vec4(0.0);
    }

    // Over the medium's *own* mean free path, not over one scene unit.
    //
    // The transmittance is what gets upsampled, because `exp(-sigma d)` is
    // bounded in (0, 1] whatever the coefficient is and that is the range the
    // reflectance fit is built for. But the distance it is evaluated at decides
    // whether the fit is being asked a well-conditioned question. At a fixed
    // unit distance a coefficient of 30 arrives as a transmittance of 1e-13,
    // which the clamp below turns into 18.4 -- so every medium denser than
    // about 18 per unit used to map to the same one, and a subsurface material,
    // whose mean free paths are millimetres, is nothing but such media.
    //
    // Referring it to `1 / peak` puts the densest channel at `exp(-1)` and every
    // other between that and one, which is the best-conditioned band the fit
    // has, and it does so at any scene scale: the returned coefficients are
    // unchanged when the whole medium is made denser and the scene smaller in
    // the same proportion, which a fixed unit distance could not manage.
    float reference = 1.0 / peak;
    vec4 transmittance = hdclaude_upsample(exp(-rgb * reference), lambda);
    return -log(max(transmittance, vec4(1.0e-8))) * peak;
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
    result.colorTemperature = light.colorTemperature;
    result.temperatureScale = light.temperatureScale;

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

// --- Lights a scattered ray can hit -----------------------------------------
//
// The analytic lights are intersected in closed form rather than built into the
// acceleration structure. They *are* closed forms -- a rect is a bounded plane,
// a sphere is a sphere -- so tessellating them into triangles would only add a
// discretisation of a shape already known exactly, plus a build, plus the
// question of how finely to tessellate a light nobody is looking at.
//
// The cost is a loop over the lights per ray, which is right for the handful a
// scene authors and wrong for a thousand. When a scene arrives with a thousand
// they belong in the structure; the density and emission below do not change
// when that happens, only where the intersection comes from.

/// Distance along `direction` at which the ray meets `light`, or -1.
///
/// `normal` is the light's outward normal at the hit. A rect and a disk emit
/// from one face, and a ray arriving at the back of one is not a hit at all --
/// which is the same test `hdclaude_sample_light` applies when it rejects a
/// sampled point facing away from the receiver. The two agree by construction,
/// and they have to: a direction one strategy can produce and the other cannot
/// account for is exactly the asymmetry that makes an MIS weight wrong.
float hdclaude_intersect_light(Light light, vec3 origin, vec3 direction,
                               out vec3 normal)
{
    normal = vec3(0.0, 1.0, 0.0);
    const float kEpsilon = 1.0e-4;

    if (light.type == HDCLAUDE_LIGHT_RECT ||
        light.type == HDCLAUDE_LIGHT_DISK)
    {
        vec3 planeNormal = normalize(light.direction);
        float denominator = dot(direction, planeNormal);
        if (abs(denominator) < 1.0e-9)
        {
            return -1.0;
        }
        float t = dot(light.position - origin, planeNormal) / denominator;
        if (t <= kEpsilon)
        {
            return -1.0;
        }
        vec3 offset = origin + direction * t - light.position;

        if (light.type == HDCLAUDE_LIGHT_RECT)
        {
            // `uAxis` and `vAxis` are half-extent vectors, which is what
            // `hdclaude_sample_light` assumes when it offsets by [-1, 1].
            float uu = dot(light.uAxis, light.uAxis);
            float vv = dot(light.vAxis, light.vAxis);
            if (uu <= 1.0e-12 || vv <= 1.0e-12)
            {
                return -1.0;
            }
            if (abs(dot(offset, light.uAxis) / uu) > 1.0 ||
                abs(dot(offset, light.vAxis) / vv) > 1.0)
            {
                return -1.0;
            }
        }
        else if (dot(offset, offset) > light.radius * light.radius)
        {
            return -1.0;
        }

        normal = planeNormal;
        return dot(normal, -direction) > 1.0e-6 ? t : -1.0;
    }

    if (light.type == HDCLAUDE_LIGHT_SPHERE)
    {
        vec3 toCentre = origin - light.position;
        float half_b = dot(toCentre, direction);
        float c = dot(toCentre, toCentre) - light.radius * light.radius;
        float discriminant = half_b * half_b - c;
        if (discriminant < 0.0)
        {
            return -1.0;
        }
        float root = sqrt(discriminant);
        float t = -half_b - root;
        if (t <= kEpsilon)
        {
            t = -half_b + root;
        }
        if (t <= kEpsilon)
        {
            return -1.0;
        }
        normal = normalize(origin + direction * t - light.position);
        return t;
    }

    if (light.type == HDCLAUDE_LIGHT_CYLINDER)
    {
        // The curved surface only; USD's cylinder light has no end caps, and
        // `hdclaude_sample_light` samples none either.
        vec3 axis = light.uAxis;
        float halfLength = length(axis);
        if (halfLength <= 1.0e-6)
        {
            return -1.0;
        }
        axis /= halfLength;

        vec3 toAxis = origin - light.position;
        vec3 dPerp = direction - axis * dot(direction, axis);
        vec3 oPerp = toAxis - axis * dot(toAxis, axis);
        float a = dot(dPerp, dPerp);
        if (a <= 1.0e-12)
        {
            return -1.0;
        }
        float half_b = dot(dPerp, oPerp);
        float c = dot(oPerp, oPerp) - light.radius * light.radius;
        float discriminant = half_b * half_b - a * c;
        if (discriminant < 0.0)
        {
            return -1.0;
        }
        float root = sqrt(discriminant);
        for (int which = 0; which < 2; ++which)
        {
            float t = (which == 0) ? (-half_b - root) / a
                                   : (-half_b + root) / a;
            if (t <= kEpsilon)
            {
                continue;
            }
            vec3 local = origin + direction * t - light.position;
            if (abs(dot(local, axis)) > halfLength)
            {
                continue;
            }
            normal = normalize(local - axis * dot(local, axis));
            return t;
        }
        return -1.0;
    }

    // A distant light is at infinity: no ray reaches it at a finite distance,
    // so it is added by the kernel that owns a ray which hit nothing.
    return -1.0;
}

/// The nearest light along a ray closer than `tMax`, or -1.
/// Whether a ray can hit this light's shape.
///
/// Both halves must agree: the render setting removes every light's geometry
/// when it is off, and the light's own flag is what the asset asked for. There
/// is deliberately no way for a light to force its shape back into a frame that
/// asked for none -- a light fixture is usually a modelled asset, and the
/// stand-in rectangle is not wanted beside it.
///
/// A light whose geometry is invisible still *lights*: it is sampled by
/// next-event estimation exactly as before. What changes is that no ray can
/// find it, so it has no second sampling strategy and next-event estimation
/// takes the whole contribution rather than a balance-heuristic share of it
/// (shade.comp.glsl). That is the same treatment the stand-in sun has always
/// had, for the same reason.
bool hdclaude_light_geometry_visible(Light light)
{
    return frame.lightGeometry != 0u && light.visibleGeometry != 0u;
}

int hdclaude_nearest_light(vec3 origin, vec3 direction, float tMax,
                           out float tHit, out vec3 normalHit)
{
    int nearest = -1;
    tHit = tMax;
    normalHit = vec3(0.0, 1.0, 0.0);

    for (uint i = 0u; i < frame.lightCount; ++i)
    {
        if (!hdclaude_light_geometry_visible(lights.values[i]))
        {
            continue;
        }
        vec3 normal;
        float t = hdclaude_intersect_light(lights.values[i], origin, direction,
                                           normal);
        if (t > 0.0 && t < tHit)
        {
            tHit = t;
            normalHit = normal;
            nearest = int(i);
        }
    }
    return nearest;
}

/// Any unit vector perpendicular to `n`, for a frame that has no other
/// constraint on where it points.
vec3 hdclaude_any_perpendicular(vec3 n)
{
    vec3 other = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    return normalize(cross(n, other));
}

/// Ray against one curve segment: a cone with a sphere at each end.
///
/// The shape a swept tube approximates, intersected exactly instead. A hair
/// segment is a truncated cone because its two ends have different widths, and
/// the spheres are what make consecutive segments join without a gap or a
/// crease -- a bare cone would leave both wherever the curve bends.
///
/// Returns the nearest positive distance, or -1 when the ray misses. `pa`, `ra`
/// and `pb`, `rb` are the two ends; the ray is in the same space as they are,
/// which for this renderer is the instance's object space.
///
/// The algebra is the standard one: the cone's surface is where the distance
/// from the axis equals the linearly interpolated radius, which after squaring
/// is a quadratic in t. `d2` is the squared axis length reduced by the radius
/// difference, and it is what makes the slanted surface rather than a cylinder.
/// The caps are two spheres, tried only when the quadratic's root falls outside
/// the truncated part -- which is exactly where the surface *is* a cap.
float hdclaude_intersect_segment(vec3 origin, vec3 direction, vec3 pa, float ra,
                                 vec3 pb, float rb)
{
    vec3 ba = pb - pa;
    vec3 oa = origin - pa;
    vec3 ob = origin - pb;
    float rr = ra - rb;
    float m0 = dot(ba, ba);
    float m1 = dot(ba, oa);
    float m2 = dot(ba, direction);
    float m3 = dot(direction, oa);
    float m5 = dot(oa, oa);
    float m6 = dot(ob, direction);
    float m7 = dot(ob, ob);

    float d2 = m0 - rr * rr;

    float k2 = d2 - m2 * m2;
    float k1 = d2 * m3 - m1 * m2 + m2 * rr * ra;
    float k0 = d2 * m5 - m1 * m1 + m1 * rr * ra * 2.0 - m0 * ra * ra;

    float h = k1 * k1 - k0 * k2;
    if (h >= 0.0 && abs(k2) > 1.0e-20)
    {
        float t = (-sqrt(h) - k1) / k2;
        float y = m1 - ra * rr + t * m2;
        // Between the two ends: the hit is on the slanted surface itself.
        if (y > 0.0 && y < d2 && t > 0.0)
        {
            return t;
        }
    }

    // Otherwise it is on one of the end spheres, if it is anywhere.
    float h1 = m3 * m3 - m5 + ra * ra;
    float h2 = m6 * m6 - m7 + rb * rb;
    float best = -1.0;
    if (h1 > 0.0)
    {
        float t = -m3 - sqrt(h1);
        if (t > 0.0)
        {
            best = t;
        }
    }
    if (h2 > 0.0)
    {
        float t = -m6 - sqrt(h2);
        if (t > 0.0 && (best < 0.0 || t < best))
        {
            best = t;
        }
    }
    return best;
}

/// The outward normal at a point on a segment, and how far along it that point
/// is.
///
/// Recomputed from the position rather than carried out of the traversal
/// kernel, because the position is already there and a second channel through
/// the hit record is a second thing to keep in step. `along` comes back in
/// [0, 1] and is the v of the curve's texture coordinate.
vec3 hdclaude_segment_normal(vec3 point, vec3 pa, float ra, vec3 pb, float rb,
                             out float along)
{
    vec3 ba = pb - pa;
    vec3 oa = point - pa;
    float rr = ra - rb;
    float m0 = dot(ba, ba);
    float d2 = m0 - rr * rr;
    float y = dot(ba, oa) - ra * rr;

    if (y > 0.0 && y < d2 && d2 > 0.0)
    {
        along = clamp(y / d2, 0.0, 1.0);
        return normalize(d2 * oa - ba * y);
    }
    // A cap: the normal is the sphere's, and the point is at one end or the
    // other.
    if (dot(oa, oa) * (rb * rb) < dot(point - pb, point - pb) * (ra * ra))
    {
        along = 0.0;
        return normalize(oa);
    }
    along = 1.0;
    return normalize(point - pb);
}

/// The solid-angle density with which next-event estimation would have chosen
/// `direction` toward this light, for the MIS weight on a ray that hit it.
///
/// The same expression `hdclaude_sample_light` returns -- uniform over the
/// light's area, converted by d^2 / (cos * A) -- evaluated at the point the ray
/// actually reached rather than at a sampled one. Writing it twice is the risk
/// here; the furnace test exists because the two must agree exactly.
float hdclaude_light_hit_pdf(Light light, float distance, vec3 normal,
                             vec3 direction)
{
    float cosLight = dot(normal, -direction);
    if (cosLight <= 1.0e-6 || light.area <= 0.0)
    {
        return 0.0;
    }
    return (distance * distance) / (cosLight * light.area);
}
