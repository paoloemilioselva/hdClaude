// The renderer's scene snapshot.
//
// Plain data with no Vulkan and no OpenUSD in it. Hydra adapters publish one of
// these; the GPU backend consumes it. That boundary is what lets scene
// synchronisation be tested without a GPU and traversal be tested without a USD
// runtime (docs/architecture.md 3).
//
// Geometry is kept in *object space*, with instancing expressed as transforms.
// A prototype's acceleration structure is then independent of where it is
// placed, which is what makes it reusable across scene publications and across
// the many copies a PointInstancer produces.

#ifndef HDCLAUDE_GPU_SCENE_H
#define HDCLAUDE_GPU_SCENE_H

#include <cstdint>
#include <string>
#include <vector>

namespace hdclaude {

/// Row-major 3x4 object-to-world transform, matching the layout Vulkan's
/// instance structure expects, so no conversion happens at upload.
struct Transform3x4 {
    float m[12] = {1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f};
};

/// How a mesh participates in traversal.
///
/// An opaque mesh lets the driver take its fast path and skip any-hit
/// evaluation entirely. This is part of a prototype's identity because changing
/// it changes the acceleration structure's build flags, so a mesh that becomes
/// cut-out cannot reuse an opaque structure.
enum class OpacityClass : std::uint8_t {
    Opaque,
    Cutout,
};

/// One object-space mesh prototype.
struct MeshPrototype {
    /// Interleaved xyz, three floats per vertex.
    std::vector<float> positions;
    /// Triangle indices, three per triangle.
    std::vector<std::uint32_t> indices;
    /// Interleaved xyz shading normals. May be empty.
    ///
    /// Three per *vertex* normally, and three per triangle *corner* when
    /// `normalsPerCorner` is set. Face-varying normals are how a hard edge is
    /// authored -- the two sides of a crease need different normals at the
    /// same point -- so an adapter that only accepts a vertex-length array
    /// throws away exactly the meshes whose shading was authored most
    /// deliberately.
    std::vector<float> normals;
    /// True when `normals` holds one normal per triangle corner rather than
    /// per vertex, which is how a face-varying or uniform primvar arrives.
    bool normalsPerCorner = false;
    /// Interleaved uv. May be empty.
    ///
    /// Two per *vertex* normally, and two per triangle *corner* when
    /// `uvsPerCorner` is set. Face-varying is not a variant hdClaude can
    /// ignore: a UV seam is authored that way, and a mesh as small as a
    /// textured quad -- the StandardShaderBall's ground is one -- carries six
    /// coordinates on four vertices and cannot be expressed any other way.
    std::vector<float> uvs;
    /// True when `uvs` holds one coordinate per triangle corner rather than
    /// per vertex, which is how a face-varying primvar arrives.
    bool uvsPerCorner = false;
    /// Per-triangle material index, one per triangle. May be empty, in which
    /// case every triangle uses the instance's material.
    std::vector<std::uint32_t> triangleMaterials;

    OpacityClass opacity = OpacityClass::Opaque;
    std::string debugName;

    std::size_t VertexCount() const { return positions.size() / 3; }
    std::size_t TriangleCount() const { return indices.size() / 3; }

    /// Identity for acceleration-structure reuse.
    ///
    /// Covers the vertex and index data and the opacity class -- everything a
    /// built structure depends on. Two prototypes with the same fingerprint
    /// share a BLAS; a prototype whose fingerprint is unchanged across a scene
    /// publication keeps the one it has.
    ///
    /// A fingerprint collision costs a wrong reuse, so this is a 64-bit hash of
    /// the actual bytes rather than of a summary.
    std::uint64_t Fingerprint() const;
};

/// One placement of a prototype.
struct MeshInstance {
    std::uint32_t prototype = 0;
    Transform3x4 transform;
    /// Material for triangles whose prototype does not name one.
    std::uint32_t material = 0;
    /// Visible to camera rays. Invisible instances still cast shadows unless
    /// excluded separately, matching UsdGeom's purpose semantics.
    bool visible = true;
};

/// What kind of emitter a Light is. Mirrors `path_state.glsl`.
enum class LightType : std::uint32_t {
    /// Infinitely distant, an angular radius wide. UsdLuxDistantLight.
    Distant = 0,
    /// A sphere of `radius` at `position`. UsdLuxSphereLight.
    Sphere = 1,
    /// A rectangle spanned by `uAxis` and `vAxis`, emitting along `direction`.
    /// UsdLuxRectLight.
    Rect = 2,
    /// A disk of `radius` in the plane normal to `direction`. UsdLuxDiskLight.
    Disk = 3,
    /// A capsule-less cylinder of `radius` and `length` about `uAxis`.
    /// UsdLuxCylinderLight.
    Cylinder = 4,
};

/// One analytic light.
///
/// Analytic rather than geometric: hdClaude's lights are not in the
/// acceleration structure, so only next-event estimation finds them and there
/// is no double counting to weigh away. That costs variance on glossy
/// reflections of large area lights, which is the price of not needing MIS in
/// the first implementation, and is recorded in docs/roadmap.md.
///
/// Field order and padding mirror the GLSL struct exactly. `scalar` layout
/// packs a vec3 followed by a float into 16 bytes, which is why every vec3 here
/// is followed by a scalar rather than by another vector.
struct Light {
    /// World-space centre. Unused by Distant.
    float position[3] = {0.0f, 0.0f, 0.0f};
    /// Sphere and disk radius, in world units.
    float radius = 0.0f;

    /// The direction the light emits *along*: USD's -Z of the light's
    /// transform. A surface is lit from `-direction`.
    float direction[3] = {0.0f, 0.0f, -1.0f};
    /// Distant light angular radius, in radians.
    float angularRadius = 0.0f;

    /// Emitted radiance: colour * intensity * 2^exposure, already divided by
    /// area when the light asked to be normalised.
    float radiance[3] = {1.0f, 1.0f, 1.0f};
    std::uint32_t type = static_cast<std::uint32_t>(LightType::Distant);

    /// Half-extent along the rectangle's local X, in world space.
    float uAxis[3] = {1.0f, 0.0f, 0.0f};
    /// Whether this light is occluded by geometry. A light with shadows
    /// disabled still needs a shadow ray slot, so this is a flag rather than a
    /// separate list.
    std::uint32_t castsShadows = 1;

    /// Half-extent along the rectangle's local Y, in world space. For a
    /// cylinder this is unused; `uAxis` carries the half-length along its axis.
    float vAxis[3] = {0.0f, 1.0f, 0.0f};
    /// Surface area, for the area-to-solid-angle conversion. Zero for Distant.
    float area = 0.0f;

    /// Cosine of the cone's outer angle from UsdLuxShapingAPI. -1 means the
    /// light is unshaped and emits over the whole hemisphere.
    float coneCosAngle = -1.0f;
    /// Softness of the cone edge, 0 for a hard edge and 1 for a full falloff
    /// across the cone.
    float coneSoftness = 0.0f;
    /// Focus exponent, sharpening emission about the axis. 0 is uniform.
    float focus = 0.0f;
    float pad0 = 0.0f;
};

/// Slots in the shared texture array.
///
/// Fixed rather than sized to the scene, because every kernel shares one
/// descriptor set layout and that layout is built before any texture is known.
/// Must match kHdClaudeTextureCapacity in shaders/path_state.glsl; the
/// MaterialX generator reads this constant so only those two can disagree.
inline constexpr std::uint32_t kTextureCapacity = 128;

/// How a decoded texture's texels are laid out.
///
/// Quantising a wider *integer* source to eight bits loses precision, which is
/// a fair trade for a mask or a roughness map. Quantising a *float* source
/// clamps it, which is not a trade at all: an HDRI's windows and lamps are the
/// only part of it that lights anything, and they live entirely above 1.0. A
/// dome light whose map has been clamped is a dome light with no highlights,
/// no sun, and a flat grey sky.
enum class TexelFormat : std::uint8_t {
    /// Eight bits a channel, linear.
    Rgba8Unorm,
    /// Eight bits a channel, sRGB-encoded; the sampler decodes.
    Rgba8Srgb,
    /// Half a channel. What a float or half source is kept in.
    Rgba16Sfloat,
};

/// Bytes one texel of `format` occupies.
inline constexpr std::uint32_t TexelSize(TexelFormat format)
{
    return format == TexelFormat::Rgba16Sfloat ? 8u : 4u;
}

/// One decoded texture, ready to upload.
///
/// Plain bytes with no image library in the interface: decoding belongs to the
/// Hydra layer, which already has OpenUSD's image plugins, and the GPU layer
/// stays free of both OpenUSD and any codec.
struct TextureImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Tightly packed RGBA in `format`, `width * height * TexelSize(format)`
    /// bytes, **bottom row first**: row 0 is v = 0, which is where USD and
    /// MaterialX put the origin of a texture and what the sampler reads as
    /// v = 0.
    std::vector<std::uint8_t> texels;
    /// How to read `texels`, and -- for the two eight-bit forms -- whether the
    /// sampler decodes sRGB.
    ///
    /// The sRGB choice is a property of what the image *means*, not of how it
    /// is stored: a normal, roughness or metalness map is data, and is
    /// routinely shipped in the same 8-bit JPEG a colour map would be. Decoding
    /// one of those as sRGB bends every value it holds -- a flat normal map
    /// stops being flat, so the whole surface tilts and its detail is
    /// exaggerated.
    TexelFormat format = TexelFormat::Rgba8Unorm;
    std::string debugName;

    bool Valid() const
    {
        return width > 0 && height > 0 &&
               texels.size() == static_cast<std::size_t>(width) * height *
                                    TexelSize(format);
    }
};

/// An immutable published scene.
///
/// Published as a whole so the renderer never observes a half-updated scene.
/// The revision is what the frame description carries, so a single comparison
/// decides whether acceleration structures need rebuilding
/// (docs/architecture.md 4).
struct Scene {
    std::vector<MeshPrototype> prototypes;
    std::vector<MeshInstance> instances;
    std::vector<Light> lights;

    /// Textures, indexed by the array index a generated material refers to.
    std::vector<TextureImage> textures;

    /// Radiance returned by a ray that leaves the scene. A dome light sets
    /// this; without one it is the stand-in sky.
    ///
    /// Bright enough to light an interior, because that is the stand-in's whole
    /// purpose: a stage with no `UsdLux` prim should render as a lit room with
    /// a lighting gap, not as a silhouette that could equally be a shading bug.
    /// At the earlier tenth of this it failed that test on the first real
    /// lightless asset -- Intel Sponza rendered as a black rectangle with a few
    /// fireflies in it. A dome light replaces it entirely, so raising it moves
    /// no scene that authors its own lighting.
    float environmentColor[3] = {0.30f, 0.38f, 0.52f};
    /// True once a dome light has supplied the environment, so the render pass
    /// knows not to apply its stand-in on top.
    bool hasDomeLight = false;

    /// Latitude-longitude environment map, if the dome light has one. Index
    /// into `textures`, or -1.
    int domeTexture = -1;
    /// World-to-light rotation for the dome, column-major, so a direction can
    /// be taken into the map's own frame. Identity when the light is unrotated.
    float domeWorldToLight[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// And back, for a direction the environment sampler chose *in* the map.
    /// Carried rather than transposed on the GPU: a dome's transform is a
    /// rotation in every scene that means anything, but "in every scene that
    /// means anything" is not a thing to build a shader on.
    float domeLightToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    std::uint64_t revision = 0;

    std::size_t TotalTriangles() const;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_SCENE_H
