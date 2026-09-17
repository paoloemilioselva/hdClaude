#include "hdclaude/gpu/path_tracer.h"

#include "hdclaude/gpu/environment_distribution.h"
#include "hdclaude/core/environment.h"
#include "hdclaude/core/spectrum.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace hdclaude {
namespace {

/// Descriptor bindings, matching shaders/path_state.glsl. Declared once and
/// shared by every kernel, so the layouts cannot disagree and a set written for
/// one pipeline is valid for another.
std::vector<BindingDescription> KernelBindings()
{
    auto storage = [](std::uint32_t binding, const char* name) {
        BindingDescription description;
        description.binding = binding;
        description.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        description.debugName = name;
        return description;
    };

    std::vector<BindingDescription> bindings;
    BindingDescription uniform;
    uniform.binding = 0;
    uniform.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform.debugName = "frame";
    bindings.push_back(uniform);

    bindings.push_back(storage(1, "pathOrigin"));
    bindings.push_back(storage(2, "pathDirection"));
    bindings.push_back(storage(3, "pathThroughput"));
    bindings.push_back(storage(4, "pathRadiance"));
    bindings.push_back(storage(5, "pathPixel"));
    bindings.push_back(storage(6, "pathRng"));
    bindings.push_back(storage(7, "hits"));
    bindings.push_back(storage(8, "counters"));
    bindings.push_back(storage(9, "activeQueue"));
    bindings.push_back(storage(10, "nextActiveQueue"));
    bindings.push_back(storage(11, "shadowRays"));
    bindings.push_back(storage(12, "accumulation"));

    BindingDescription tlas;
    tlas.binding = 13;
    tlas.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    tlas.debugName = "sceneTlas";
    bindings.push_back(tlas);

    bindings.push_back(storage(14, "instances"));
    bindings.push_back(storage(15, "lights"));

    // The shared texture array. Every kernel carries the binding because every
    // kernel shares one descriptor set layout, even though only `shade` reads
    // it; a layout that varied per kernel would mean a descriptor set per
    // kernel rather than one written identically for all of them.
    BindingDescription textures;
    textures.binding = 16;
    textures.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textures.count = kTextureCapacity;
    textures.debugName = "hdclaude_textures";
    bindings.push_back(textures);

    // The dome light's environment map, as its own sampler. The environment
    // kernel has no generated material in front of it, so it has no texture
    // array to index into.
    BindingDescription dome;
    dome.binding = 17;
    dome.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dome.debugName = "hdclaude_dome";
    bindings.push_back(dome);

    // The material sort and the indirect commands it sizes.
    bindings.push_back(storage(18, "materialQueue"));
    bindings.push_back(storage(19, "materialTable"));
    bindings.push_back(storage(20, "dispatchArgs"));
    bindings.push_back(storage(21, "pathScatterPdf"));
    bindings.push_back(storage(25, "pathMedium"));
    bindings.push_back(storage(26, "pathHeroOnly"));
    bindings.push_back(storage(27, "guideDepth"));
    bindings.push_back(storage(28, "guideMotion"));
    bindings.push_back(storage(29, "guideSurface"));
    bindings.push_back(storage(30, "guideSpecularRay"));
    bindings.push_back(storage(31, "instanceLinks"));
    bindings.push_back(storage(32, "pathLastInstance"));
    bindings.push_back(storage(22, "environmentDistribution"));
    bindings.push_back(storage(23, "pathWavelengths"));
    bindings.push_back(storage(24, "spectralTables"));

    return bindings;
}

/// Mirrors the ShadeParams push constant in shade.comp.glsl.
///
/// The shading dispatch is already per material, so a material property that
/// cannot travel inside the generated program travels here at no cost: there is
/// exactly one dispatch per material and the value is constant across it.
struct ShadePush {
    std::uint32_t materialId = 0;
    float dispersionAbbe = 0.0f;
    /// Which bounce this dispatch is. See shade.comp.glsl: it is a push
    /// constant rather than a frame-uniform field so that a whole sample can be
    /// recorded into one command buffer.
    std::uint32_t bounce = 0;
    /// One when the material is a thin-walled sheet. See shade.comp.glsl.
    std::uint32_t thinWalled = 0;
};

/// Mirrors EnvironmentParams in environment.comp.glsl.
struct EnvironmentPush {
    std::uint32_t bounce = 0;
};

/// Mirrors the FrameBlock uniform in path_state.glsl, scalar layout.
struct FrameBlock {
    float cameraToWorld[16];
    float environmentColor[4];
    float sunDirection[4];
    float sunRadiance[4];
    std::uint32_t resolution[2];
    std::uint32_t sampleIndex;
    std::uint32_t maxBounces;
    float tanHalfFov;
    float aspect;
    std::uint32_t pathCount;
    /// Was the bounce index. It is a push constant now, on the two kernels
    /// that vary with it; the slot stays so every field after it keeps its
    /// offset. See shaders/path_state.glsl.
    std::uint32_t unusedWasBounce;
    std::uint32_t lightCount;
    std::uint32_t hasDomeTexture;
    std::uint32_t hasDomeLight;
    std::uint32_t materialCount;
    std::uint32_t hasEnvironmentDistribution;
    std::uint32_t environmentWidth;
    std::uint32_t environmentHeight;
    std::uint32_t environmentConditional;
    std::uint32_t environmentDensity;
    std::uint32_t spectralSamples;
    std::uint32_t chromaTableOffset;
    std::uint32_t chromaTableSize;
    float spectralNormalisation;
    float environmentTemperature;
    float environmentTemperatureScale;
    float domeWorldToLight[16];
    float domeLightToWorld[16];
    /// World to clip, for the depth guide. See RenderCamera::worldToClip.
    float worldToClip[16];
    /// The previous frame's world-to-clip, for motion vectors. Equal to
    /// `worldToClip` on a frame with no previous one, which reports no motion
    /// rather than motion from nowhere.
    float previousWorldToClip[16];
    /// The frame's sub-pixel offset, and whether raygen should use it rather
    /// than drawing one per sample.
    float jitter[2];
    std::uint32_t useFixedJitter;
    /// Whether light geometry is in the scene at all. Takes the slot a pad
    /// held, so the block's layout is unchanged.
    std::uint32_t lightGeometry;
    /// Words of light-linking membership each instance has in `instanceLinks`.
    std::uint32_t linkWords;
    /// The dome light's link categories, or -1.
    std::int32_t domeLightLink;
    std::int32_t domeShadowLink;
};

/// The radical inverse of `index` in `base`, one coordinate of a Halton
/// sequence.
///
/// Halton rather than a random draw because a reconstruction backend must be
/// told where the sample landed, and rather than a regular grid because a grid
/// of N offsets repeats with period N, and any period becomes a standing
/// pattern in the reconstructed image. Bases two and three are the pair every
/// temporal renderer uses: the first two primes, so the coordinates share no
/// common period.
inline float RadicalInverse(std::uint32_t index, std::uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

/// Mirrors the indirect command slots in path_state.glsl. Each slot is a
/// VkDispatchIndirectCommand followed by a word of padding, so a slot's byte
/// offset is its index times this stride.
constexpr VkDeviceSize kDispatchArgStride = 16;
constexpr std::uint32_t kDispatchSlotActive = 0;
constexpr std::uint32_t kDispatchSlotShadow = 1;
constexpr std::uint32_t kDispatchSlotFirstMaterial = 2;

/// Stages of the prepare_dispatch kernel, matching its push constant.
constexpr std::uint32_t kPrepareActive = 0;
constexpr std::uint32_t kPrepareMaterials = 1;
constexpr std::uint32_t kPrepareShadow = 2;

/// Passes of the material sort, matching its push constant.
constexpr std::uint32_t kSortCount = 0;
constexpr std::uint32_t kSortScatter = 1;

/// Mirrors InstanceGeometry in path_state.glsl.
struct InstanceGeometry {
    std::uint64_t positions;
    std::uint64_t indices;
    /// Curve segments, eight floats each. Non-zero exactly when this instance
    /// is a curve set.
    std::uint64_t segments;
    std::uint64_t normals;
    std::uint64_t uvs;
    std::uint64_t triangleMaterials;
    float objectToWorld[12];
    float worldToObject[12];
    /// The previous frame's placement, for motion vectors. Composed with
    /// `worldToObject` it takes a hit back to where it was.
    float previousObjectToWorld[12];
    std::uint32_t material;
    /// 1 when `uvs` holds one coordinate per triangle corner.
    std::uint32_t uvsPerCorner;
    /// 1 when `normals` holds one normal per triangle corner.
    std::uint32_t normalsPerCorner;
    std::uint32_t pad;
};

VulkanBuffer MakeStorage(VulkanAllocator& allocator, VkDeviceSize size,
                         const char* name)
{
    BufferDescription description;
    description.size = size;
    description.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    description.domain = BufferDomain::DeviceLocal;
    description.debugName = name;
    return VulkanBuffer(allocator, description);
}

/// Column-major 4x4 product, `out = a * b`.
void MultiplyMatrix4(const float a[16], const float b[16], float out[16])
{
    float result[16];
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += double(a[k * 4 + row]) * double(b[column * 4 + k]);
            }
            result[column * 4 + row] = float(sum);
        }
    }
    std::memcpy(out, result, sizeof(result));
}

/// General 4x4 inverse by cofactors, in double. A camera's placement is not
/// assumed rigid: a host may hand one with scale in it, and the inverse has to
/// be the inverse of what was handed.
void InvertMatrix4(const float m[16], float out[16])
{
    double a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    double inv[16];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    const double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    const double scale = det != 0.0 ? 1.0 / det : 0.0;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] * scale);
}

/// Invert a 3x4 rigid-plus-scale transform.
///
/// Written out rather than pulled from a matrix library because the GPU side
/// needs the inverse *transpose* for normals, and a wrong inverse under
/// non-uniform scale produces normals that are subtly off in a way that reads
/// as a shading bug rather than a transform bug.
void InvertTransform3x4(const float m[12], float out[12])
{
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[4], e = m[5], f = m[6];
    const float g = m[8], h = m[9], i = m[10];

    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float inv = det != 0.0f ? 1.0f / det : 0.0f;

    out[0] = (e * i - f * h) * inv;
    out[1] = (c * h - b * i) * inv;
    out[2] = (b * f - c * e) * inv;
    out[4] = (f * g - d * i) * inv;
    out[5] = (a * i - c * g) * inv;
    out[6] = (c * d - a * f) * inv;
    out[8] = (d * h - e * g) * inv;
    out[9] = (b * g - a * h) * inv;
    out[10] = (a * e - b * d) * inv;

    const float tx = m[3], ty = m[7], tz = m[11];
    out[3] = -(out[0] * tx + out[1] * ty + out[2] * tz);
    out[7] = -(out[4] * tx + out[5] * ty + out[6] * tz);
    out[11] = -(out[8] * tx + out[9] * ty + out[10] * tz);
}

/// Half-precision to float, for the one image the renderer reads back that is
/// not already float: a reconstruction backend's output.
///
/// Written out rather than taken from a library because there is no portable
/// one -- `_Float16` is a compiler extension and `std::float16_t` is C++23 --
/// and because the whole of it is a bit rearrangement. Subnormals are rounded
/// to zero, which is the only inexactness here and is below what any display
/// transform can carry; infinities and NaNs survive, which matters: a
/// non-finite pixel out of a backend is a finding, not something to swallow.
float HalfToFloat(std::uint16_t half)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1Fu;
    const std::uint32_t mantissa = half & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        bits = sign;
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void Barrier(VkCommandBuffer command)
{
    // A blunt whole-pipeline barrier between kernels. The wavefront stages are
    // strictly sequential -- each reads what the previous wrote -- so there is
    // nothing to overlap within a bounce, and a finer barrier would buy nothing
    // while adding a way to be wrong.
    //
    // It covers three stages rather than compute alone, because the frame uses
    // all three on the same buffers:
    //
    //   transfer  the queue promotion and the counter resets between bounces
    //             are `vkCmdCopyBuffer` and `vkCmdFillBuffer`. A barrier that
    //             named only compute did not order the fill against the copy
    //             that read the same counters, which synchronisation validation
    //             reports as a write-after-read hazard.
    //   indirect  dispatch sizes are read by the command processor at
    //             VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, not by a shader, so a
    //             compute-only destination scope does not make a freshly
    //             written indirect command visible to the dispatch that reads
    //             it.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_TRANSFER_WRITE_BIT |
                            VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_TRANSFER_WRITE_BIT |
                            VK_ACCESS_2_TRANSFER_READ_BIT |
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
}

}  // namespace

// ---------------------------------------------------------------------------

std::string ResolveKernelIncludes(const std::filesystem::path& directory,
                                  const std::string& source)
{
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t directive = line.find("#include \"");
        if (directive == std::string::npos) {
            output << line << '\n';
            continue;
        }
        const std::size_t begin = directive + 10;
        const std::size_t end = line.find('"', begin);
        if (end == std::string::npos) {
            output << line << '\n';
            continue;
        }
        const std::string name = line.substr(begin, end - begin);
        std::ifstream included(directory / name);
        if (!included) {
            // Left in place so glslang reports the unresolved include with the
            // file named, rather than failing later on a missing declaration.
            output << line << '\n';
            continue;
        }
        std::ostringstream contents;
        contents << included.rdbuf();
        output << ResolveKernelIncludes(directory, contents.str());
    }
    return output.str();
}

std::string LoadKernel(const std::filesystem::path& directory,
                       const std::string& name)
{
    std::ifstream file(directory / name);
    std::ostringstream contents;
    contents << file.rdbuf();
    return ResolveKernelIncludes(directory, contents.str());
}

// ---------------------------------------------------------------------------

PathTracer::PathTracer(const VulkanContext& context, VulkanAllocator& allocator,
                       std::filesystem::path shaderDirectory)
    : _context(context),
      _allocator(allocator),
      _shaderDirectory(std::move(shaderDirectory))
{
    context.RequireLive("PathTracer");

    _shadeKernelSource = LoadKernel(_shaderDirectory, "shade.comp.glsl");
    if (_shadeKernelSource.empty()) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "shade.comp.glsl not found under " +
                              _shaderDirectory.string());
    }

    const std::vector<BindingDescription> bindings = KernelBindings();

    auto build = [&](const char* name, std::uint32_t pushBytes) {
        const std::string source = LoadKernel(_shaderDirectory, name);
        GlslCompileOptions options;
        options.moduleName = name;
        const GlslCompileResult compiled = _compiler.Compile(source, options);
        if (!compiled.ok) {
            throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                              std::string("kernel ") + name + " failed:\n" +
                                  compiled.log);
        }
        return ComputePipeline(context, compiled.spirv, bindings, pushBytes, name);
    };

    _raygen = build("raygen.comp.glsl", 0);
    _extend = build("extend.comp.glsl", 0);
    _prepareDispatch = build("prepare_dispatch.comp.glsl", sizeof(std::uint32_t));
    _materialSort = build("material_sort.comp.glsl", sizeof(std::uint32_t));
    _environment = build("environment.comp.glsl", sizeof(EnvironmentPush));
    _shadow = build("shadow.comp.glsl", 0);
    _film = build("film.comp.glsl", 0);
    _guides = build("guides.comp.glsl", 0);
    _specularHit = build("specular_hit.comp.glsl", 0);

    _accelerator = std::make_unique<SceneAccelerator>(context, allocator);

    // --- Texture sampling ----------------------------------------------------
    // One sampler for every texture. Per-texture wrap and filter modes are a
    // MaterialX image-node parameter, and honouring them needs either a
    // sampler per combination or the address handling moved into the shader;
    // that choice is recorded in docs/roadmap.md rather than guessed at. Repeat
    // addressing and linear filtering are what an authored texture almost
    // always wants, and are what the stock mx_image_* defaults assume.
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    context.Check(vkCreateSampler(context.Device(), &samplerInfo, nullptr, &_sampler),
                  "vkCreateSampler(textures)");

    // A one-pixel placeholder for every unused slot. Opaque magenta rather than
    // black or white: a material that samples a texture hdClaude failed to load
    // should look obviously wrong rather than plausibly dark.
    TextureImage placeholder;
    placeholder.width = 1;
    placeholder.height = 1;
    placeholder.texels = {255, 0, 255, 255};
    placeholder.debugName = "texture.placeholder";
    _placeholderTexture = UploadTexture(placeholder);

    BuildSpectralTables();

    UploadTextures({});
}

PathTracer::~PathTracer()
{
    // The sampler outlives every descriptor that referenced it only because
    // this runs after the caller has waited on all work; the context's
    // device-lost latch is honoured by skipping the wait, never the destroy.
    if (_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(_context.Device(), _sampler, nullptr);
        _sampler = VK_NULL_HANDLE;
    }
    if (_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(_context.Device(), _timestampPool, nullptr);
        _timestampPool = VK_NULL_HANDLE;
    }
}

void PathTracer::BuildSpectralTables()
{
    // The colour matching functions and the illuminant, sampled at 5 nm across
    // the visible range, followed by the chromaticity table.
    //
    // Sampled rather than given to the shader in closed form: both already
    // exist on the host, where they are the definitions the upsampling fit and
    // its round-trip gate are written against. A second closed form in GLSL
    // would be a second thing to keep in agreement with them, and the
    // disagreement would show as every material being a slightly different
    // colour than its own unit test says it is.
    constexpr float kStep = 5.0f;
    std::vector<float> data;
    _spectralSamples = 0;
    for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += kStep) {
        const Vec3 bar = CieXyzBar(lambda);
        data.push_back(bar.x);
        data.push_back(bar.y);
        data.push_back(bar.z);
        data.push_back(IlluminantD65(lambda));
        ++_spectralSamples;
    }

    // The film divides by the illuminant's luminous integral, which is what
    // makes a white surface under a white light resolve to white rather than to
    // the illuminant's absolute power. Computed from the same samples the
    // shader interpolates, so the two cannot disagree about the grid.
    double luminous = 0.0;
    for (std::uint32_t i = 0; i < _spectralSamples; ++i) {
        luminous += static_cast<double>(data[i * 4 + 1]) *
                    static_cast<double>(data[i * 4 + 3]);
    }
    luminous *= static_cast<double>(kStep);
    _spectralNormalisation =
        luminous > 0.0 ? static_cast<float>(1.0 / luminous) : 1.0f;

    _chromaTableOffset = static_cast<std::uint32_t>(data.size());
    const ChromaTable table = BuildChromaTable();
    _chromaTableSize = static_cast<std::uint32_t>(table.size);
    data.insert(data.end(), table.coefficients.begin(), table.coefficients.end());

    const VkDeviceSize size = data.size() * sizeof(float);
    BufferDescription staging;
    staging.size = size;
    staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging.domain = BufferDomain::HostUpload;
    staging.debugName = "spectralTables.staging";
    VulkanBuffer upload(_allocator, staging);
    upload.Write(data.data(), size);

    _spectralTables = MakeStorage(_allocator, size, "spectralTables");
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(command, upload.Handle(), _spectralTables.Handle(), 1,
                        &region);
    });
}

VulkanImage PathTracer::UploadTexture(const TextureImage& texture)
{
    ImageDescription description;
    description.width = texture.width;
    description.height = texture.height;
    // sRGB decode in hardware rather than in the shader. MaterialX generates
    // no linearisation of its own -- it assumes the sampler returns linear --
    // so doing it here is what makes an authored colour map mean what it says.
    //
    // R16G16B16A16_SFLOAT is a mandatory sampled format with linear filtering,
    // so an HDR source needs no capability query to keep its range.
    switch (texture.format) {
        case TexelFormat::Rgba8Srgb:
            description.format = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case TexelFormat::Rgba16Sfloat:
            description.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            break;
        case TexelFormat::Rgba8Unorm:
            description.format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
    }
    description.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    description.debugName = texture.debugName;

    VulkanImage image(_allocator, description);

    const VkDeviceSize size =
        static_cast<VkDeviceSize>(texture.texels.size());
    BufferDescription staging;
    staging.size = size;
    staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging.domain = BufferDomain::HostUpload;
    staging.debugName = texture.debugName + ".staging";
    VulkanBuffer upload(_allocator, staging);
    upload.Write(texture.texels.data(), size);

    _context.SubmitImmediate([&](VkCommandBuffer command) {
        image.RecordBarrier(command, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {texture.width, texture.height, 1};
        vkCmdCopyBufferToImage(command, upload.Handle(), image.Handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        image.RecordBarrier(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });

    return image;
}

void PathTracer::UploadTextures(const std::vector<TextureImage>& textures)
{
    _texturePool.clear();
    _texturePool.reserve(textures.size());
    for (const TextureImage& texture : textures) {
        if (!texture.Valid()) {
            // A texture that failed to decode still occupies its pool slot, so
            // every material's slot table keeps pointing at the right
            // neighbours. The slot resolves to the placeholder.
            _texturePool.push_back(VulkanImage());
            continue;
        }
        _texturePool.push_back(UploadTexture(texture));
    }
}

std::vector<VkDescriptorImageInfo> PathTracer::TextureBindingsFor(
    int material) const
{
    // Every element is written, the unused tail included. A declared
    // descriptor that is never written is undefined the moment a shader
    // indexes it, and an out-of-range index in generated code should give a
    // wrong pixel rather than a lost device.
    std::vector<VkDescriptorImageInfo> bindings(kTextureCapacity);

    const std::vector<std::uint32_t>* slots = nullptr;
    if (material >= 0 &&
        static_cast<std::size_t>(material) < _materialTextureSlots.size()) {
        slots = &_materialTextureSlots[static_cast<std::size_t>(material)];
    }

    for (std::uint32_t i = 0; i < kTextureCapacity; ++i) {
        VkImageView view = _placeholderTexture.View();
        if (slots != nullptr && i < slots->size()) {
            const std::uint32_t pool = (*slots)[i];
            if (pool < _texturePool.size() && _texturePool[pool].Valid()) {
                view = _texturePool[pool].View();
            }
        }
        bindings[i].sampler = _sampler;
        bindings[i].imageView = view;
        bindings[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return bindings;
}

void PathTracer::SetScene(const Scene& scene,
                          const std::vector<CompiledMaterial>& materials)
{
    _context.RequireLive("PathTracer::SetScene");

    _accelerator->Update(scene);

    // --- Instance table -----------------------------------------------------
    // Built in the same order the accelerator emits instances, because the
    // custom index a ray query reports is an index into this table.
    // --- Per-triangle materials ----------------------------------------------
    // One buffer for the whole scene, with each prototype's run at a known
    // offset. It is deliberately *not* part of the BLAS: a material index
    // changes whenever the material set changes, and folding it into the
    // acceleration structure would throw away the fingerprint reuse that makes
    // a static scene cheap to republish.
    std::vector<std::uint32_t> triangleMaterials;
    std::vector<std::size_t> prototypeMaterialOffset(scene.prototypes.size(),
                                                     std::size_t(-1));
    for (std::size_t i = 0; i < scene.prototypes.size(); ++i) {
        const MeshPrototype& prototype = scene.prototypes[i];
        // Per-primitive materials, whichever kind of primitive this prototype
        // has. A curve's are per segment and a mesh's per triangle, and the
        // buffer, the offsets and the shader's indexing are the same either
        // way: the primitive index a ray query reports is what indexes it.
        const std::vector<std::uint32_t>& perPrimitive =
            prototype.IsCurve() ? prototype.segmentMaterials
                                : prototype.triangleMaterials;
        if (perPrimitive.empty()) {
            continue;
        }
        prototypeMaterialOffset[i] = triangleMaterials.size();
        triangleMaterials.insert(triangleMaterials.end(), perPrimitive.begin(),
                                 perPrimitive.end());
    }

    _triangleMaterials = VulkanBuffer();
    if (!triangleMaterials.empty()) {
        const VkDeviceSize size = triangleMaterials.size() * sizeof(std::uint32_t);
        BufferDescription staging;
        staging.size = size;
        staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.domain = BufferDomain::HostUpload;
        staging.debugName = "triangleMaterials.staging";
        VulkanBuffer upload(_allocator, staging);
        upload.Write(triangleMaterials.data(), size);

        _triangleMaterials = MakeStorage(_allocator, size, "triangleMaterials");
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(command, upload.Handle(), _triangleMaterials.Handle(),
                            1, &region);
        });
    }

    std::vector<InstanceGeometry> table;
    table.reserve(scene.instances.size());

    for (const MeshInstance& instance : scene.instances) {
        const BottomLevelStructure* blas = _accelerator->Blas(instance.prototype);
        if (blas == nullptr) {
            continue;
        }
        InstanceGeometry entry{};
        entry.positions = blas->Positions().DeviceAddress();
        entry.indices = blas->Indices().DeviceAddress();
        entry.segments =
            blas->Segments().Valid() ? blas->Segments().DeviceAddress() : 0;
        entry.normals = blas->Normals().Valid() ? blas->Normals().DeviceAddress() : 0;
        entry.uvs = blas->Uvs().Valid() ? blas->Uvs().DeviceAddress() : 0;

        entry.triangleMaterials = 0;
        if (instance.prototype < prototypeMaterialOffset.size() &&
            prototypeMaterialOffset[instance.prototype] != std::size_t(-1) &&
            _triangleMaterials.Valid()) {
            entry.triangleMaterials =
                _triangleMaterials.DeviceAddress() +
                prototypeMaterialOffset[instance.prototype] *
                    sizeof(std::uint32_t);
        }
        std::memcpy(entry.objectToWorld, instance.transform.m, sizeof(entry.objectToWorld));
        InvertTransform3x4(instance.transform.m, entry.worldToObject);
        std::memcpy(entry.previousObjectToWorld,
                    instance.hasPreviousTransform ? instance.previousTransform.m
                                                  : instance.transform.m,
                    sizeof(entry.previousObjectToWorld));
        entry.material = instance.material;
        if (instance.prototype < scene.prototypes.size()) {
            entry.uvsPerCorner =
                scene.prototypes[instance.prototype].uvsPerCorner ? 1u : 0u;
            entry.normalsPerCorner =
                scene.prototypes[instance.prototype].normalsPerCorner ? 1u : 0u;
        }
        table.push_back(entry);
    }
    _instanceCount = static_cast<std::uint32_t>(table.size());

    if (!table.empty()) {
        const VkDeviceSize size = table.size() * sizeof(InstanceGeometry);
        BufferDescription staging;
        staging.size = size;
        staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.domain = BufferDomain::HostUpload;
        staging.debugName = "instanceTable.staging";
        VulkanBuffer upload(_allocator, staging);
        upload.Write(table.data(), size);

        _instanceTable = MakeStorage(_allocator, size, "instanceTable");
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(command, upload.Handle(), _instanceTable.Handle(), 1,
                            &region);
        });
    }

    // --- Light table --------------------------------------------------------
    // Always allocated, even when the scene has no lights: a descriptor set
    // must point at a real buffer, and a null binding is a validation error
    // rather than an empty table. The kernels read frame.lightCount, not the
    // buffer's size, so a one-entry placeholder is never sampled.
    {
        const std::size_t count = std::max<std::size_t>(1, scene.lights.size());
        const VkDeviceSize size = count * sizeof(Light);

        BufferDescription staging;
        staging.size = size;
        staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.domain = BufferDomain::HostUpload;
        staging.debugName = "lightTable.staging";
        VulkanBuffer upload(_allocator, staging);

        std::vector<Light> lightTable = scene.lights;
        lightTable.resize(count);
        upload.Write(lightTable.data(), size);

        _lightTable = MakeStorage(_allocator, size, "lightTable");
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(command, upload.Handle(), _lightTable.Handle(), 1,
                            &region);
        });
        _lightCount = static_cast<std::uint32_t>(scene.lights.size());
    }

    // --- Light-linking membership ---------------------------------------------
    // One bit a category, `_linkWords` words an instance, indexed by the
    // instance's position in the scene -- which is the custom index a ray query
    // reports, not its position in the instance table. Always allocated, for the
    // same reason as the light table.
    {
        _linkWords = (scene.linkCategoryCount + 31u) / 32u;
        std::vector<std::uint32_t> membership(
            std::max<std::size_t>(1, scene.instances.size() * _linkWords), 0u);
        for (std::size_t i = 0; i < scene.instances.size(); ++i) {
            for (const std::uint32_t category : scene.instances[i].linkCategories) {
                if (category < scene.linkCategoryCount) {
                    membership[i * _linkWords + category / 32u] |=
                        1u << (category % 32u);
                }
            }
        }
        const VkDeviceSize size = membership.size() * sizeof(std::uint32_t);

        BufferDescription staging;
        staging.size = size;
        staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.domain = BufferDomain::HostUpload;
        staging.debugName = "instanceLinks.staging";
        VulkanBuffer upload(_allocator, staging);
        upload.Write(membership.data(), size);

        _instanceLinks = MakeStorage(_allocator, size, "instanceLinks");
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(command, upload.Handle(), _instanceLinks.Handle(), 1,
                            &region);
        });
        _domeLightLink = scene.domeLightLink;
        _domeShadowLink = scene.domeShadowLink;
    }

    // --- Textures ------------------------------------------------------------
    UploadTextures(scene.textures);

    // The dome map is bound separately from the array, so it is remembered
    // here rather than resolved per frame.
    _domeTexture = VulkanImage();
    if (scene.domeTexture >= 0 &&
        static_cast<std::size_t>(scene.domeTexture) < scene.textures.size()) {
        const TextureImage& dome = scene.textures[scene.domeTexture];
        if (dome.Valid()) {
            _domeTexture = UploadTexture(dome);
        }
    }
    std::memcpy(_domeWorldToLight, scene.domeWorldToLight,
                sizeof(_domeWorldToLight));
    std::memcpy(_domeLightToWorld, scene.domeLightToWorld,
                sizeof(_domeLightToWorld));
    _hasDomeLight = scene.hasDomeLight;
    _domeColorTemperature = scene.domeColorTemperature;
    _domeTemperatureScale = scene.domeTemperatureScale;

    // --- The environment's sampling distribution ----------------------------
    // Built from the dome map, if there is one with any light in it. A constant
    // environment needs none: uniform sphere sampling is already its exact
    // density, and building a distribution over one texel would say nothing.
    {
        EnvironmentDistribution distribution;
        if (_domeTexture.Valid()) {
            distribution = BuildEnvironmentDistribution(
                scene.textures[static_cast<std::size_t>(scene.domeTexture)]);
        }

        const std::vector<float> fallback{0.0f};
        const std::vector<float>& values =
            distribution.Valid() ? distribution.data : fallback;
        const VkDeviceSize size = values.size() * sizeof(float);

        BufferDescription staging;
        staging.size = size;
        staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging.domain = BufferDomain::HostUpload;
        staging.debugName = "environmentDistribution.staging";
        VulkanBuffer upload(_allocator, staging);
        upload.Write(values.data(), size);

        _environmentDistribution =
            MakeStorage(_allocator, size, "environmentDistribution");
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            VkBufferCopy region{};
            region.size = size;
            vkCmdCopyBuffer(command, upload.Handle(),
                            _environmentDistribution.Handle(), 1, &region);
        });

        _environmentWidth = distribution.Valid() ? distribution.width : 0;
        _environmentHeight = distribution.Valid() ? distribution.height : 0;
        _environmentConditional =
            distribution.Valid()
                ? static_cast<std::uint32_t>(distribution.ConditionalOffset())
                : 0;
        _environmentDensity =
            distribution.Valid()
                ? static_cast<std::uint32_t>(distribution.DensityOffset())
                : 0;
    }

    // --- Shading pipelines --------------------------------------------------
    // One per material. Each is that material's generated program joined to the
    // shade kernel, so the dispatch contains only that material's code.
    const std::vector<BindingDescription> bindings = KernelBindings();
    _shade.clear();
    _shade.reserve(materials.size());
    _materialTextureSlots.clear();
    _materialTextureSlots.reserve(materials.size());
    _materialDispersion.clear();
    _materialDispersion.reserve(materials.size());
    _materialThinWalled.clear();
    _materialThinWalled.reserve(materials.size());
    for (const CompiledMaterial& material : materials) {
        _shade.push_back(ComputePipeline(_context, material.spirv, bindings,
                                         sizeof(ShadePush),
                                         "shade." + material.debugName));
        _materialTextureSlots.push_back(material.textureSlots);
        _materialDispersion.push_back(material.dispersionAbbe);
        _materialThinWalled.push_back(material.thinWalled ? 1u : 0u);
    }

    // --- The sort's tables ---------------------------------------------------
    // Sized here rather than with the path state, because their stride is the
    // number of materials rather than the number of pixels. Both are allocated
    // even when the scene has no material at all: a descriptor set must name a
    // real buffer, and the kernels read frame.materialCount rather than a
    // buffer's size.
    const auto materialCount = static_cast<std::uint32_t>(_shade.size());
    const std::uint32_t tableEntries = std::max<std::uint32_t>(1, materialCount);
    BufferDescription argsDescription;
    argsDescription.size =
        (VkDeviceSize(kDispatchSlotFirstMaterial) + tableEntries) * kDispatchArgStride;
    argsDescription.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    argsDescription.domain = BufferDomain::DeviceLocal;
    argsDescription.debugName = "sort.dispatchArgs";
    for (FrameSlot& slot : _slots) {
        slot.materialTable = MakeStorage(
            _allocator, VkDeviceSize(tableEntries) * 3 * 4, "sort.materialTable");
        slot.dispatchArgs = VulkanBuffer(_allocator, argsDescription);
    }

    // The command processor reads every slot of an indirect buffer it is
    // pointed at, including one this frame's kernels never wrote. Zeroing
    // means an unwritten slot dispatches nothing rather than whatever the
    // allocation happened to contain.
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        for (FrameSlot& slot : _slots) {
            vkCmdFillBuffer(command, slot.dispatchArgs.Handle(), 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(command, slot.materialTable.Handle(), 0, VK_WHOLE_SIZE, 0);
        }
    });

    // A published scene replaces the shading pipelines, so every set that named
    // the old ones is stale.
    ++_resourceGeneration;
}

void PathTracer::EnsureResolution(std::uint32_t width, std::uint32_t height)
{
    if (width == _width && height == _height) {
        return;
    }

    const std::uint32_t paths = width * height;

    // The film first, and once: every slot accumulates into the same one.
    VulkanBuffer accumulation = MakeStorage(_allocator, paths * 16, "film");

    // Each slot's own path state. Built into locals and published together, so
    // a failure part-way leaves the previous resolution's buffers intact rather
    // than a half-resized set (docs/architecture.md 6 rule 1).
    for (FrameSlot& slot : _slots) {
        VulkanBuffer origin = MakeStorage(_allocator, paths * 12, "path.origin");
        VulkanBuffer direction = MakeStorage(_allocator, paths * 12, "path.direction");
        // Sixteen bytes, not twelve: a path carries four spectral lanes, not three
        // colour channels.
        VulkanBuffer throughput = MakeStorage(_allocator, paths * 16, "path.throughput");
        VulkanBuffer radiance = MakeStorage(_allocator, paths * 16, "path.radiance");
        VulkanBuffer wavelengths = MakeStorage(_allocator, paths * 16, "path.wavelengths");
        VulkanBuffer pixel = MakeStorage(_allocator, paths * 4, "path.pixel");
        VulkanBuffer rng = MakeStorage(_allocator, paths * 4, "path.rng");
        // The density of the scattering behind each path's current ray, for the
        // MIS weight the environment kernel applies.
        VulkanBuffer scatterPdf = MakeStorage(_allocator, paths * 4, "path.scatterPdf");
        // The interior medium a path is inside, as an absorption coefficient.
        VulkanBuffer medium = MakeStorage(_allocator, paths * 32, "path.medium");
        // Whether a dispersive surface has already collapsed the path's packet onto
        // its hero wavelength.
        VulkanBuffer heroOnly = MakeStorage(_allocator, paths * 4, "path.heroOnly");
        // The instance each path last scattered from, for light linking.
        VulkanBuffer lastInstance =
            MakeStorage(_allocator, paths * 4, "path.lastInstance");
        VulkanBuffer hits = MakeStorage(_allocator, paths * 16, "path.hits");
        VulkanBuffer guideDepth =
            MakeStorage(_allocator, paths * 4, "guide.depth");
        VulkanBuffer guideMotion =
            MakeStorage(_allocator, paths * 8, "guide.motion");
        // Three vec4 per pixel: normal and roughness, diffuse albedo, specular
        // albedo.
        VulkanBuffer guideSurface =
            MakeStorage(_allocator, paths * 48, "guide.surface");
        VulkanBuffer guideSpecularRay =
            MakeStorage(_allocator, paths * 32, "guide.specularRay");
        // Eight uints: activeCount, nextActiveCount, shadowCount, a pad, the two
        // ray accumulators, and the two hashes -- over what the rays were and over
        // what they hit. Everything past byte 16 is per call rather than per
        // bounce, which is why it sits there: the inter-bounce reset fills bytes 4
        // to 16 and would otherwise clear it every bounce.
        VulkanBuffer counters = MakeStorage(_allocator, 32, "counters");

        // A host-visible landing place for the accumulators and the hashes. Allocated with
        // the rest of the resolution-dependent state so it is created once rather
        // than per frame, though it does not depend on the resolution at all.
        BufferDescription rayReadbackDescription;
        rayReadbackDescription.size = 16;
        rayReadbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        rayReadbackDescription.domain = BufferDomain::HostReadback;
        rayReadbackDescription.debugName = "counters.rayReadback";
        VulkanBuffer rayReadback(_allocator, rayReadbackDescription);
        VulkanBuffer activeQueue = MakeStorage(_allocator, paths * 4, "queue.active");
        VulkanBuffer nextQueue = MakeStorage(_allocator, paths * 4, "queue.nextActive");
        VulkanBuffer shadowRays = MakeStorage(_allocator, paths * 64, "queue.shadow");
        // The sorted queue holds the active paths that hit geometry, which is at
        // most every path.
        VulkanBuffer materialQueue = MakeStorage(_allocator, paths * 4, "queue.material");

        BufferDescription uniformDescription;
        uniformDescription.size = sizeof(FrameBlock);
        uniformDescription.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uniformDescription.domain = BufferDomain::HostUpload;
        uniformDescription.debugName = "frame";
        VulkanBuffer frameUniforms(_allocator, uniformDescription);

        BufferDescription guideReadbackDescription;
        guideReadbackDescription.size = paths * 4;
        guideReadbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        guideReadbackDescription.domain = BufferDomain::HostReadback;
        guideReadbackDescription.debugName = "guide.readback";
        VulkanBuffer guideReadback(_allocator, guideReadbackDescription);

        BufferDescription motionReadbackDescription = guideReadbackDescription;
        motionReadbackDescription.size = paths * 8;
        motionReadbackDescription.debugName = "guide.motionReadback";
        VulkanBuffer motionReadback(_allocator, motionReadbackDescription);

        BufferDescription surfaceReadbackDescription = guideReadbackDescription;
        surfaceReadbackDescription.size = paths * 48;
        surfaceReadbackDescription.debugName = "guide.surfaceReadback";
        VulkanBuffer surfaceReadback(_allocator, surfaceReadbackDescription);

        BufferDescription specularRayReadbackDescription = guideReadbackDescription;
        specularRayReadbackDescription.size = paths * 32;
        specularRayReadbackDescription.debugName = "guide.specularRayReadback";
        VulkanBuffer specularRayReadback(_allocator, specularRayReadbackDescription);

        BufferDescription readbackDescription;
        readbackDescription.size = paths * 16;
        readbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackDescription.domain = BufferDomain::HostReadback;
        readbackDescription.debugName = "film.readback";
        VulkanBuffer readback(_allocator, readbackDescription);

        slot.origin = std::move(origin);
        slot.direction = std::move(direction);
        slot.throughput = std::move(throughput);
        slot.radiance = std::move(radiance);
        slot.wavelengths = std::move(wavelengths);
        slot.pixel = std::move(pixel);
        slot.rng = std::move(rng);
        slot.scatterPdf = std::move(scatterPdf);
        slot.medium = std::move(medium);
        slot.heroOnly = std::move(heroOnly);
        slot.lastInstance = std::move(lastInstance);
        slot.hits = std::move(hits);
        slot.guideDepth = std::move(guideDepth);
        slot.guideMotion = std::move(guideMotion);
        slot.guideSurface = std::move(guideSurface);
        slot.guideSpecularRay = std::move(guideSpecularRay);
        slot.counters = std::move(counters);
        slot.rayReadback = std::move(rayReadback);
        slot.activeQueue = std::move(activeQueue);
        slot.nextActiveQueue = std::move(nextQueue);
        slot.shadowRays = std::move(shadowRays);
        slot.materialQueue = std::move(materialQueue);
        slot.frameUniforms = std::move(frameUniforms);
        slot.readback = std::move(readback);
        slot.guideReadback = std::move(guideReadback);
        slot.motionReadback = std::move(motionReadback);
        slot.surfaceReadback = std::move(surfaceReadback);
        slot.specularRayReadback = std::move(specularRayReadback);
        }

    // The film is shared, so it is built once outside the loop.
    _accumulation = std::move(accumulation);
    _width = width;
    _height = height;

    // Every descriptor set now names a buffer that no longer exists.
    ++_resourceGeneration;
}

void PathTracer::WriteDescriptors(VkDescriptorSet set,
                                  const ComputePipeline& pipeline,
                                  const FrameSlot& slot, int material)
{
    pipeline.WriteBuffer(set, 0, slot.frameUniforms,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    pipeline.WriteBuffer(set, 1, slot.origin);
    pipeline.WriteBuffer(set, 2, slot.direction);
    pipeline.WriteBuffer(set, 3, slot.throughput);
    pipeline.WriteBuffer(set, 4, slot.radiance);
    pipeline.WriteBuffer(set, 5, slot.pixel);
    pipeline.WriteBuffer(set, 6, slot.rng);
    pipeline.WriteBuffer(set, 7, slot.hits);
    pipeline.WriteBuffer(set, 8, slot.counters);
    pipeline.WriteBuffer(set, 9, slot.activeQueue);
    pipeline.WriteBuffer(set, 10, slot.nextActiveQueue);
    pipeline.WriteBuffer(set, 11, slot.shadowRays);
    pipeline.WriteBuffer(set, 12, _accumulation);

    VkAccelerationStructureKHR tlas = _accelerator->Tlas().Handle();
    VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    accelerationWrite.accelerationStructureCount = 1;
    accelerationWrite.pAccelerationStructures = &tlas;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 13;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    write.pNext = &accelerationWrite;
    vkUpdateDescriptorSets(_context.Device(), 1, &write, 0, nullptr);

    pipeline.WriteBuffer(set, 14, _instanceTable);
    pipeline.WriteBuffer(set, 15, _lightTable);
    pipeline.WriteSampledImageArray(set, 16, TextureBindingsFor(material));

    VkDescriptorImageInfo dome{};
    dome.sampler = _sampler;
    dome.imageView = _domeTexture.Valid() ? _domeTexture.View()
                                          : _placeholderTexture.View();
    dome.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pipeline.WriteSampledImageArray(set, 17, {dome});

    pipeline.WriteBuffer(set, 18, slot.materialQueue);
    pipeline.WriteBuffer(set, 19, slot.materialTable);
    pipeline.WriteBuffer(set, 20, slot.dispatchArgs);
    pipeline.WriteBuffer(set, 21, slot.scatterPdf);
    pipeline.WriteBuffer(set, 25, slot.medium);
    pipeline.WriteBuffer(set, 26, slot.heroOnly);
    pipeline.WriteBuffer(set, 27, slot.guideDepth);
    pipeline.WriteBuffer(set, 28, slot.guideMotion);
    pipeline.WriteBuffer(set, 29, slot.guideSurface);
    pipeline.WriteBuffer(set, 30, slot.guideSpecularRay);
    pipeline.WriteBuffer(set, 31, _instanceLinks);
    pipeline.WriteBuffer(set, 32, slot.lastInstance);
    pipeline.WriteBuffer(set, 22, _environmentDistribution);
    pipeline.WriteBuffer(set, 23, slot.wavelengths);
    pipeline.WriteBuffer(set, 24, _spectralTables);
}

std::vector<std::uint32_t> PathTracer::MaterialCounts() const
{
    const FrameSlot& slot = _slots[_lastSlot];
    if (_shade.empty() || !slot.materialTable.Valid()) {
        return {};
    }
    const VkDeviceSize size = VkDeviceSize(_shade.size()) * 4;

    BufferDescription description;
    description.size = size;
    description.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    description.domain = BufferDomain::HostReadback;
    description.debugName = "sort.materialTable.readback";
    VulkanBuffer readback(_allocator, description);

    _context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(command, slot.materialTable.Handle(), readback.Handle(), 1,
                        &region);
    });

    std::vector<std::uint32_t> counts(_shade.size());
    std::memcpy(counts.data(), readback.MappedData(), size);
    return counts;
}

bool PathTracer::InvalidateFor(const FrameDescription& description,
                               bool reconstructionChanged)
{
    // Everything that could invalidate anything, decided here and nowhere else.
    //
    // The caller's own `resetAccumulation` is an input rather than the answer.
    // It says what the caller believes; these say what the renderer knows, and
    // a caller that forgets one of them gets a correct frame anyway. That
    // asymmetry is the point: hdCodex's independent setters each had to guess
    // what the others implied and each guessed wrong differently
    // (docs/lessons-from-hdcodex.md D3).
    const bool resized = description.width != _width || description.height != _height;
    const bool modeChanged =
        _hasPreviousFrame && description.mode != _previousMode;
    const bool sceneChanged =
        _hasPreviousFrame && description.sceneRevision != _previousSceneRevision;
    // The camera, which this decision did not previously include and should
    // have. An accumulated film is an average of one integral, and a camera
    // that has moved makes it an average of two. A caller that moves the camera
    // and forgets to ask for a reset was the one case the asymmetry this
    // function exists for did not actually cover.
    const bool cameraMoved =
        _hasPreviousFrame && description.camera != _previousCamera;

    // The resize is the only one with work attached. The others invalidate an
    // accumulation that is about to be cleared anyway, so they need no more
    // than to be noticed.
    if (resized) {
        EnsureResolution(description.width, description.height);
    }

    _previousMode = description.mode;
    _previousSceneRevision = description.sceneRevision;
    _previousCamera = description.camera;
    _hasPreviousFrame = true;

    // What a reconstruction backend must discard its history for, which is not
    // the same set. A camera that merely *moved* is the case reprojection
    // exists to handle: the history is still of this scene and motion vectors
    // say where it went. A resize, a mode switch or a changed scene leaves
    // nothing to reproject from.
    // `reconstructionChanged` is the one of these the description does not
    // carry: a quality mode that traces at a different extent, or a backend
    // that has just appeared or gone away, leaves a reconstructor's history
    // describing an image of a different size. It arrives as an argument
    // because the plan is decided by BeginFrame, which is also where the
    // extents this function compares against come from.
    //
    // `resetAccumulation` counts here only in reference mode. An interactive
    // frame's film holds one frame's samples and its history lives in the
    // backend (docs/architecture.md 5), so that film restarts every frame by
    // contract -- and a restart that happens every frame is evidence of
    // nothing. Folding it in was wrong and measurably so: it raised
    // `historyReset` on every interactive frame, which reached DLSS as
    // `InReset` and threw away the history on the frame that had just built
    // it, leaving a temporal reconstructor with nothing temporal about it. A
    // caller that genuinely means a cut says so with `resetHistory`.
    const bool interactive = description.mode == RenderMode::Interactive;
    _historyReset = description.settings.resetHistory || resized ||
                    modeChanged || sceneChanged || reconstructionChanged ||
                    (!interactive && description.settings.resetAccumulation);

    return _historyReset || cameraMoved || interactive ||
           description.settings.resetAccumulation;
}

// --- Reconstruction ---------------------------------------------------------

PathTracer::ReconstructionPlan PathTracer::PlanReconstruction(
    const FrameDescription& description)
{
    ReconstructionPlan plan;
    plan.resolution.renderWidth = description.width;
    plan.resolution.renderHeight = description.height;
    plan.resolution.outputWidth = description.width;
    plan.resolution.outputHeight = description.height;
    plan.resolution.quality = description.settings.reconstructionQuality;
    plan.resolution.preset = description.settings.reconstructionPreset;
    plan.resolution.model = description.settings.reconstructionModel;

    // Reference frames never reach a backend, and the test is here rather than
    // at the call site so there is one place it can be read from
    // (docs/dlss-integration.md 6).
    if (description.mode != RenderMode::Interactive ||
        !description.settings.reconstruct) {
        return plan;
    }

    if (!_reconstructionAttempted) {
        _reconstructionAttempted = true;
        std::string reason;
        _reconstruction = CreateNgxBackend(_context, &reason);
        if (!_reconstruction) {
            // An ordinary answer rather than an error: a build without the SDK,
            // a device from another vendor, a driver too old. The frame renders
            // without reconstruction and says so.
            _reconstructionUnavailable = reason;
        }
    }
    if (!_reconstruction) {
        return plan;
    }

    // The render extent is the backend's answer, not the caller's request:
    // DLSS chooses it per quality mode, and tracing at a size it did not ask
    // for either wastes work or hands its model less than it expects.
    const ReconstructionSizing sizing = _reconstruction->QuerySizing(
        description.width, description.height,
        description.settings.reconstructionQuality,
        description.settings.reconstructionModel);
    if (!sizing.valid || sizing.renderWidth == 0 || sizing.renderHeight == 0) {
        _reconstructionUnavailable = sizing.reason.empty()
                                         ? "backend declined these extents"
                                         : sizing.reason;
        return plan;
    }

    _reconstructionUnavailable.clear();
    plan.active = true;
    plan.resolution.renderWidth = sizing.renderWidth;
    plan.resolution.renderHeight = sizing.renderHeight;
    return plan;
}

bool PathTracer::EnsureReconstructionImages(
    const ReconstructionResolution& resolution, std::string* reason)
{
    const bool unchanged =
        _reconstructionBuilt &&
        _reconstructionResolution.renderWidth == resolution.renderWidth &&
        _reconstructionResolution.renderHeight == resolution.renderHeight &&
        _reconstructionResolution.outputWidth == resolution.outputWidth &&
        _reconstructionResolution.outputHeight == resolution.outputHeight &&
        _reconstructionResolution.quality == resolution.quality &&
        _reconstructionResolution.preset == resolution.preset &&
        _reconstructionResolution.model == resolution.model;
    if (unchanged) {
        return true;
    }

    // The packing kernel, compiled the first time a frame asks to be
    // reconstructed and never in a run that does not.
    if (!_reconstructPack.Valid()) {
        std::vector<BindingDescription> bindings;
        auto add = [&bindings](std::uint32_t binding, VkDescriptorType type,
                               const char* name) {
            BindingDescription description;
            description.binding = binding;
            description.type = type;
            description.debugName = name;
            bindings.push_back(description);
        };
        add(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "accumulation");
        add(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guideDepth");
        add(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guideMotion");
        add(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "colorImage");
        add(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "depthImage");
        add(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "motionImage");
        add(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "partials");
        add(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guideSurface");
        add(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "normalRoughnessImage");
        add(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "diffuseAlbedoImage");
        add(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "specularAlbedoImage");
        add(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "guideSpecularRay");
        add(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, "specularHitDistanceImage");

        const std::string source =
            LoadKernel(_shaderDirectory, "reconstruct_inputs.comp.glsl");
        GlslCompileOptions options;
        options.moduleName = "reconstruct_inputs.comp.glsl";
        const GlslCompileResult compiled = _compiler.Compile(source, options);
        if (!compiled.ok) {
            if (reason != nullptr) {
                *reason = "reconstruct_inputs.comp.glsl failed:\n" + compiled.log;
            }
            return false;
        }
        _reconstructPack =
            ComputePipeline(_context, compiled.spirv, bindings,
                            sizeof(std::uint32_t) * 2, "reconstruct_inputs");
        _reconstructSet = _reconstructPack.AllocateSet();
    }

    // The kernel that finishes the exposure reduction, compiled alongside it
    // and on the same terms: never in a run that reconstructs nothing.
    if (!_reconstructExposure.Valid()) {
        std::vector<BindingDescription> exposureBindings;
        BindingDescription partialsBinding;
        partialsBinding.binding = 0;
        partialsBinding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        partialsBinding.debugName = "partials";
        exposureBindings.push_back(partialsBinding);
        BindingDescription exposureBinding;
        exposureBinding.binding = 1;
        exposureBinding.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        exposureBinding.debugName = "exposureImage";
        exposureBindings.push_back(exposureBinding);

        const std::string exposureSource =
            LoadKernel(_shaderDirectory, "reconstruct_exposure.comp.glsl");
        GlslCompileOptions exposureOptions;
        exposureOptions.moduleName = "reconstruct_exposure.comp.glsl";
        const GlslCompileResult exposureCompiled =
            _compiler.Compile(exposureSource, exposureOptions);
        if (!exposureCompiled.ok) {
            if (reason != nullptr) {
                *reason = "reconstruct_exposure.comp.glsl failed:" +
                          exposureCompiled.log;
            }
            return false;
        }
        _reconstructExposure = ComputePipeline(
            _context, exposureCompiled.spirv, exposureBindings,
            sizeof(std::uint32_t) * 2, "reconstruct_exposure");
        _reconstructExposureSet = _reconstructExposure.AllocateSet();
    }

    // Storage support on every format the kernel writes, asked of the device
    // rather than assumed. Vulkan requires it of R16G16B16A16_SFLOAT and of
    // neither of the others, so a device that cannot should say so here rather
    // than through a validation error at the first dispatch.
    const auto storageSupported = [this](VkFormat format) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_context.PhysicalDevice(), format,
                                            &properties);
        return (properties.optimalTilingFeatures &
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    };
    for (const VkFormat format :
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
          VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16_SFLOAT}) {
        if (!storageSupported(format)) {
            if (reason != nullptr) {
                *reason = "device supports no storage image in Vulkan format " +
                          std::to_string(static_cast<int>(format));
            }
            return false;
        }
    }

    const auto makeImage = [this](std::uint32_t width, std::uint32_t height,
                                  VkFormat format, VkImageUsageFlags usage,
                                  const char* name) {
        ImageDescription description;
        description.width = width;
        description.height = height;
        description.format = format;
        description.usage = usage;
        description.debugName = name;
        return VulkanImage(_allocator, description);
    };

    // Sampled because the backend reads them, storage because the packing
    // kernel writes them.
    constexpr VkImageUsageFlags kInputUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    _reconstructColor =
        makeImage(resolution.renderWidth, resolution.renderHeight,
                  VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage, "reconstruct.color");
    _reconstructDepth =
        makeImage(resolution.renderWidth, resolution.renderHeight,
                  VK_FORMAT_R32_SFLOAT, kInputUsage, "reconstruct.depth");
    _reconstructMotion =
        makeImage(resolution.renderWidth, resolution.renderHeight,
                  VK_FORMAT_R16G16_SFLOAT, kInputUsage, "reconstruct.motion");
    _reconstructNormalRoughness = makeImage(
        resolution.renderWidth, resolution.renderHeight,
        VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage, "reconstruct.normalRoughness");
    _reconstructDiffuseAlbedo = makeImage(
        resolution.renderWidth, resolution.renderHeight,
        VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage, "reconstruct.diffuseAlbedo");
    _reconstructSpecularAlbedo = makeImage(
        resolution.renderWidth, resolution.renderHeight,
        VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage, "reconstruct.specularAlbedo");
    _reconstructSpecularHitDistance = makeImage(
        resolution.renderWidth, resolution.renderHeight, VK_FORMAT_R32_SFLOAT,
        kInputUsage, "reconstruct.specularHitDistance");
    // Storage, because NGX refuses a read-write resource whose image was not
    // created with it; transfer-destination, because DLSS clears the output
    // itself before writing; transfer-source, because this is where the frame
    // is read back from.
    _reconstructOutput =
        makeImage(resolution.outputWidth, resolution.outputHeight,
                  VK_FORMAT_R16G16B16A16_SFLOAT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  "reconstruct.output");

    BufferDescription readback;
    readback.size = static_cast<VkDeviceSize>(resolution.outputWidth) *
                    resolution.outputHeight * 4 * sizeof(std::uint16_t);
    readback.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readback.domain = BufferDomain::HostReadback;
    readback.debugName = "reconstruct.readback";
    _reconstructReadback = VulkanBuffer(_allocator, readback);

    // One float per workgroup of the packing kernel, which is what decides the
    // size: the reduction's first stage leaves exactly that many partials.
    const std::uint32_t packGroupsX = (resolution.renderWidth + 7) / 8;
    const std::uint32_t packGroupsY = (resolution.renderHeight + 7) / 8;
    BufferDescription luminance;
    luminance.size = static_cast<VkDeviceSize>(packGroupsX) * packGroupsY *
                     sizeof(float);
    luminance.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    luminance.domain = BufferDomain::DeviceLocal;
    luminance.debugName = "reconstruct.luminance";
    _reconstructLuminance = VulkanBuffer(_allocator, luminance);

    BufferDescription exposureReadback;
    exposureReadback.size = sizeof(std::uint16_t);
    exposureReadback.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    exposureReadback.domain = BufferDomain::HostReadback;
    exposureReadback.debugName = "reconstruct.exposureReadback";
    _reconstructExposureReadback = VulkanBuffer(_allocator, exposureReadback);

    // Sampled because DLSS reads it, storage because the reduction writes it.
    // One texel, which is the shape the guide specifies (3.9).
    // Transfer-source as well as the usual pair, because the value is copied
    // back so it can be reported. Believing a number was delivered is not the
    // same as knowing it, and this is a single texel.
    _reconstructExposureImage = makeImage(
        1, 1, VK_FORMAT_R16_SFLOAT,
        kInputUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "reconstruct.exposure");

    _reconstructPack.WriteStorageImage(_reconstructSet, 3, _reconstructColor);
    _reconstructPack.WriteStorageImage(_reconstructSet, 4, _reconstructDepth);
    _reconstructPack.WriteStorageImage(_reconstructSet, 5, _reconstructMotion);
    _reconstructPack.WriteBuffer(_reconstructSet, 6, _reconstructLuminance);
    _reconstructPack.WriteStorageImage(_reconstructSet, 8,
                                       _reconstructNormalRoughness);
    _reconstructPack.WriteStorageImage(_reconstructSet, 9,
                                       _reconstructDiffuseAlbedo);
    _reconstructPack.WriteStorageImage(_reconstructSet, 10,
                                       _reconstructSpecularAlbedo);
    _reconstructPack.WriteStorageImage(_reconstructSet, 12,
                                       _reconstructSpecularHitDistance);
    _reconstructExposure.WriteBuffer(_reconstructExposureSet, 0,
                                     _reconstructLuminance);
    _reconstructExposure.WriteStorageImage(_reconstructExposureSet, 1,
                                           _reconstructExposureImage);

    // The backend's own feature, built with a command buffer because DLSS's
    // creation is recorded rather than immediate, and submitted on its own: the
    // build has to have completed before an evaluation is recorded against it.
    bool built = false;
    std::string buildReason;
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        built = _reconstruction->Resize(command, resolution, &buildReason);
    });
    if (!built) {
        if (reason != nullptr) {
            *reason = buildReason.empty() ? "backend failed to resize" : buildReason;
        }
        _reconstructionBuilt = false;
        return false;
    }

    _reconstructionResolution = resolution;
    _reconstructionBuilt = true;
    return true;
}

std::vector<float> PathTracer::Reconstruct(
    const FrameSlot& slot, const ReconstructionResolution& resolution,
    const RenderSettings& settings, bool historyReset,
    const RenderCamera& camera)
{
    std::string reason;
    if (!EnsureReconstructionImages(resolution, &reason)) {
        _reconstructionUnavailable = reason;
        return {};
    }

    // The film and the guides are the same buffers every kernel indexes, so the
    // set is pointed at this frame's slot rather than written once: the slots
    // alternate.
    _reconstructPack.WriteBuffer(_reconstructSet, 0, _accumulation);
    _reconstructPack.WriteBuffer(_reconstructSet, 1, slot.guideDepth);
    _reconstructPack.WriteBuffer(_reconstructSet, 2, slot.guideMotion);
    _reconstructPack.WriteBuffer(_reconstructSet, 7, slot.guideSurface);
    _reconstructPack.WriteBuffer(_reconstructSet, 11, slot.guideSpecularRay);

    const std::uint32_t extent[2] = {resolution.renderWidth,
                                     resolution.renderHeight};
    const std::uint32_t groupsX = (resolution.renderWidth + 7) / 8;
    const std::uint32_t groupsY = (resolution.renderHeight + 7) / 8;

    ReconstructionFrame frame;
    const auto describe = [](const VulkanImage& image) {
        ReconstructionTexture texture;
        texture.image = image.Handle();
        texture.view = image.View();
        texture.format = image.Description().format;
        texture.width = image.Description().width;
        texture.height = image.Description().height;
        return texture;
    };
    frame.color = describe(_reconstructColor);
    frame.depth = describe(_reconstructDepth);
    frame.motion = describe(_reconstructMotion);
    frame.output = describe(_reconstructOutput);
    frame.normalRoughness = describe(_reconstructNormalRoughness);
    frame.diffuseAlbedo = describe(_reconstructDiffuseAlbedo);
    frame.specularAlbedo = describe(_reconstructSpecularAlbedo);
    frame.specularHitDistance = describe(_reconstructSpecularHitDistance);
    // World to view is the inverse of the camera's placement, and view to clip
    // is the host's world-to-clip taken back through that placement, so the
    // two compose to exactly the projection the depth guide was written with.
    InvertMatrix4(camera.cameraToWorld, frame.worldToView);
    MultiplyMatrix4(camera.worldToClip, camera.cameraToWorld, frame.viewToClip);
    if (resolution.exposure == ReconstructionExposure::Measured) {
        frame.exposure = describe(_reconstructExposureImage);
    }
    frame.jitterX = settings.jitter[0];
    frame.jitterY = settings.jitter[1];
    // The film is linear HDR with nothing folded into it: hdClaude applies
    // exposure in the display transform, downstream of everything here.
    frame.preExposure = 1.0f;
    frame.reset = historyReset;

    _context.SubmitImmediate([&](VkCommandBuffer command) {
        // The inputs go from whatever they held -- nothing on the first frame,
        // the previous frame's guides on every one after, neither of which this
        // kernel reads -- to general, which is the layout a storage image is
        // written in.
        for (const VulkanImage* image :
             {&_reconstructColor, &_reconstructDepth, &_reconstructMotion,
              &_reconstructExposureImage, &_reconstructNormalRoughness,
              &_reconstructDiffuseAlbedo, &_reconstructSpecularAlbedo,
              &_reconstructSpecularHitDistance}) {
            image->RecordBarrier(command, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        _reconstructOutput.RecordBarrier(
            command, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_ACCESS_2_MEMORY_WRITE_BIT);

        _reconstructPack.Dispatch(command, _reconstructSet, groupsX, groupsY, 1,
                                  extent, sizeof(extent));

        // The reduction's second stage. It reads what the pass above wrote, and
        // a dispatch does not order itself against the one before it, so the
        // partials need a barrier of their own -- the images below get theirs
        // for the backend, and this is the same statement about the buffer.
        VkMemoryBarrier2 partialsBarrier{};
        partialsBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        partialsBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        partialsBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        partialsBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        partialsBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &partialsBarrier;
        vkCmdPipelineBarrier2(command, &dependency);

        const std::uint32_t reduction[2] = {groupsX * groupsY,
                                            resolution.renderWidth *
                                                resolution.renderHeight};
        _reconstructExposure.Dispatch(command, _reconstructExposureSet, 1, 1, 1,
                                      reduction, sizeof(reduction));

        // Written by a compute shader here, read by whatever the backend does
        // with them there, which this side of the boundary knows nothing about
        // and so names every stage.
        for (const VulkanImage* image :
             {&_reconstructColor, &_reconstructDepth, &_reconstructMotion,
              &_reconstructExposureImage, &_reconstructNormalRoughness,
              &_reconstructDiffuseAlbedo, &_reconstructSpecularAlbedo,
              &_reconstructSpecularHitDistance}) {
            image->RecordBarrier(command, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_ACCESS_2_MEMORY_READ_BIT);
        }

        // What the backend was told the exposure was, copied back so it can be
        // reported rather than assumed. One texel; it costs nothing and it is
        // the difference between believing a value was delivered and knowing
        // it.
        _reconstructExposureImage.RecordBarrier(
            command, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy exposureRegion{};
        exposureRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        exposureRegion.imageSubresource.layerCount = 1;
        exposureRegion.imageExtent.width = 1;
        exposureRegion.imageExtent.height = 1;
        exposureRegion.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(command, _reconstructExposureImage.Handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               _reconstructExposureReadback.Handle(), 1,
                               &exposureRegion);
        _reconstructExposureImage.RecordBarrier(
            command, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_MEMORY_READ_BIT);

        _reconstruction->Evaluate(command, frame);

        _reconstructOutput.RecordBarrier(
            command, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = resolution.outputWidth;
        region.imageExtent.height = resolution.outputHeight;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(command, _reconstructOutput.Handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               _reconstructReadback.Handle(), 1, &region);
    });

    {
        std::uint16_t stored = 0;
        std::memcpy(&stored, _reconstructExposureReadback.MappedData(),
                    sizeof(stored));
        _lastExposure = HalfToFloat(stored);
    }

    const std::size_t pixels =
        static_cast<std::size_t>(resolution.outputWidth) * resolution.outputHeight;
    std::vector<std::uint16_t> half(pixels * 4);
    std::memcpy(half.data(), _reconstructReadback.MappedData(),
                half.size() * sizeof(std::uint16_t));

    std::vector<float> image(pixels * 4);
    for (std::size_t i = 0; i < image.size(); ++i) {
        image[i] = HalfToFloat(half[i]);
    }
    // The alpha is the renderer's, not the backend's: every other path out of
    // the film returns an opaque image and so does this one.
    for (std::size_t i = 0; i < pixels; ++i) {
        image[i * 4 + 3] = 1.0f;
    }
    return image;
}

FrameHandle PathTracer::BeginFrame(const FrameDescription& description)
{
    _context.RequireLive("PathTracer::BeginFrame");

    FrameHandle handle;
    handle.index = ++_frameIndex;
    handle.valid = true;

    // What this frame will be traced at, which is not always what it was asked
    // for: an upscaling backend chooses its own render extent, and everything
    // below -- the invalidation, the trace, the guides -- is in those extents.
    // Only the image that comes out is in the requested ones.
    const ReconstructionPlan plan = PlanReconstruction(description);
    // A backend that turned on or off, or one rebuilt as a different model,
    // preset or quality -- each is a feature with no history, even at an
    // unchanged extent. Only the extent used to be noticed, through the resize,
    // so switching Super Resolution to Ray Reconstruction at one size reset the
    // backend's history without the frame saying so.
    const bool reconstructionChanged =
        _hasPreviousFrame &&
        (plan.active != _previousReconstructed ||
         (plan.active &&
          (plan.resolution.model != _previousReconstructionResolution.model ||
           plan.resolution.preset != _previousReconstructionResolution.preset ||
           plan.resolution.quality != _previousReconstructionResolution.quality)));
    _previousReconstructed = plan.active;
    _previousReconstructionResolution = plan.resolution;

    FrameDescription traced = description;
    traced.width = plan.resolution.renderWidth;
    traced.height = plan.resolution.renderHeight;

    RenderSettings settings = description.settings;
    settings.resetAccumulation = InvalidateFor(traced, reconstructionChanged);

    // The frame's sub-pixel offset, decided here unless the caller stated one.
    //
    // Only an interactive frame has one. A reference render accumulates
    // hundreds of samples and jitters each independently, which is the correct
    // estimator and has no single offset to report; giving it a fixed one would
    // land every sample of a frame in the same place and turn an average into a
    // point sample. A caller that asks for one anyway is told, rather than
    // quietly given something else.
    //
    // Indexed by the frame rather than from zero, because the radical inverse
    // of zero is zero and a first frame with no jitter at all is the one frame
    // a reconstructor most needs jittered.
    //
    // A caller may state the offset instead, and it is then used exactly as
    // given and reported back unchanged. That is what a host already driving a
    // temporal pattern of its own needs -- the DLSS guide asks a renderer that
    // has TAA to jitter the way its TAA does (3.7.2) -- and it is what lets a
    // test hold the offset still, which is the only condition under which a
    // sign error in it is a displacement rather than a blur.
    if (description.mode == RenderMode::Interactive) {
        if (!settings.fixedJitter) {
            const auto index = static_cast<std::uint32_t>(handle.index);
            settings.jitter[0] = RadicalInverse(index, 2) - 0.5f;
            settings.jitter[1] = RadicalInverse(index, 3) - 0.5f;
            settings.fixedJitter = true;
        }
    } else {
        if (settings.fixedJitter && !_warnedReferenceJitter) {
            _warnedReferenceJitter = true;
            std::fprintf(stderr,
                         "hdClaude: a reference render was given a fixed "
                         "sub-pixel offset (%.4f %.4f) and is ignoring it; a "
                         "reference render jitters every sample independently "
                         "and has no single offset\n",
                         static_cast<double>(settings.jitter[0]),
                         static_cast<double>(settings.jitter[1]));
        }
        settings.jitter[0] = 0.0f;
        settings.jitter[1] = 0.0f;
        settings.fixedJitter = false;
    }

    _pendingFrame = FrameResult{};
    _pendingFrame.index = handle.index;
    _pendingFrame.width = description.width;
    _pendingFrame.height = description.height;
    _pendingFrame.renderWidth = traced.width;
    _pendingFrame.renderHeight = traced.height;
    _pendingFrame.firstSample = settings.firstSample;
    _pendingFrame.sampleCount = settings.samplesPerPixel;
    _pendingFrame.accumulationReset = settings.resetAccumulation;
    _pendingFrame.jitter[0] = settings.jitter[0];
    _pendingFrame.jitter[1] = settings.jitter[1];
    _pendingFrame.historyReset = _historyReset;
    _pendingFrame.image =
        Trace(traced.width, traced.height, description.camera, settings);
    _pendingFrame.depth = std::move(_lastDepth);
    _pendingFrame.motion = std::move(_lastMotion);
    _pendingFrame.normalRoughness = std::move(_lastNormalRoughness);
    _pendingFrame.diffuseAlbedo = std::move(_lastDiffuseAlbedo);
    _pendingFrame.specularAlbedo = std::move(_lastSpecularAlbedo);
    _pendingFrame.specularHitDistance = std::move(_lastSpecularHitDistance);

    if (plan.active) {
        // The traced image is what the backend is *given*, by way of the film
        // it came out of; what it produces replaces it. A backend that fails
        // here leaves the frame exactly as it was traced rather than failing
        // the frame: at the render extent, with `reconstructed` false and
        // `ReconstructionUnavailable` saying why, which is a preview at the
        // wrong size and never a black image.
        std::vector<float> reconstructed =
            Reconstruct(_slots[_lastSlot], plan.resolution, settings,
                        _pendingFrame.historyReset, description.camera);
        if (!reconstructed.empty()) {
            _pendingFrame.image = std::move(reconstructed);
            _pendingFrame.reconstructed = true;
            _pendingFrame.reconstructionBackend = _reconstruction->Name();
        } else {
            _pendingFrame.width = traced.width;
            _pendingFrame.height = traced.height;
        }
    }

    return handle;
}

FrameResult PathTracer::EndFrame(FrameHandle handle)
{
    if (!handle.valid || handle.index != _pendingFrame.index) {
        // A handle from a frame that was never begun, or one that has already
        // been taken. Returning an empty result rather than the previous
        // frame's is what stops a caller writing a stale image into a buffer
        // and never knowing.
        return FrameResult{};
    }
    FrameResult result = std::move(_pendingFrame);
    _pendingFrame = FrameResult{};
    return result;
}

std::vector<float> PathTracer::Render(std::uint32_t width, std::uint32_t height,
                                      const RenderCamera& camera,
                                      const RenderSettings& settings)
{
    FrameDescription description;
    description.width = width;
    description.height = height;
    description.camera = camera;
    description.settings = settings;
    // No scene revision and no mode: a caller using this overload is not
    // tracking either, so every frame names the same ones and the invalidation
    // above reduces to the resize it would have done anyway. That is what keeps
    // this exactly the function it was.
    return EndFrame(BeginFrame(description)).image;
}

std::vector<float> PathTracer::Trace(std::uint32_t width, std::uint32_t height,
                                     const RenderCamera& camera,
                                     const RenderSettings& settings)
{
    _context.RequireLive("PathTracer::Trace");

    const std::uint32_t paths = width * height;

    // Return last frame's sets before taking new ones.
    //
    // Every submit this function makes is waited on before it returns, so no
    // set from a previous call can still be in flight here. Without the reset
    // the fixed-size pools are exhausted after a handful of progressive
    // frames, which is exactly how this surfaced: a one-shot render never
    // reached the limit.
    //
    // Allocating once and rewriting only on a resource change would be
    // cheaper still; it is recorded in docs/roadmap.md rather than done here,
    // because it needs the descriptor-generation tracking to be authoritative.
    // The slot this frame owns. Frames alternate, so the one still being read
    // back is never the one being written.
    _lastSlot = static_cast<std::size_t>(_frameIndex % kFrameSlots);
    FrameSlot& slot = _slots[_lastSlot];

    // Sets are allocated once per slot and rewritten only when what they name
    // has moved, rather than allocated afresh out of a pool reset at the top of
    // every trace. The reset was the thing standing in the way: resetting a
    // pool whose sets an earlier frame's command buffers still reference is
    // undefined behaviour, and it is only safe today because every submit waits
    // for the device. A frame that owns its sets does not need anyone else to
    // have finished with theirs.
    if (slot.descriptorGeneration != _resourceGeneration) {
        // The sets a previous generation allocated are freed by resetting the
        // pool, which is safe *here* precisely because nothing is in flight:
        // the resource change that bumped the generation happened between
        // frames, not during one.
        _raygen.ResetSets();
        _extend.ResetSets();
        _prepareDispatch.ResetSets();
        _materialSort.ResetSets();
        _environment.ResetSets();
        _shadow.ResetSets();
        _film.ResetSets();
        _guides.ResetSets();
        _specularHit.ResetSets();
        for (ComputePipeline& pipeline : _shade) {
            pipeline.ResetSets();
        }
        for (FrameSlot& other : _slots) {
            other.descriptorGeneration = 0;
            other.shadeSets.clear();
        }

        // Descriptor sets are allocated per pipeline but written identically:
        // the binding table is shared, so every kernel sees the same state.
        for (FrameSlot& other : _slots) {
            other.raygenSet = _raygen.AllocateSet();
            other.extendSet = _extend.AllocateSet();
            other.prepareSet = _prepareDispatch.AllocateSet();
            other.sortSet = _materialSort.AllocateSet();
            other.environmentSet = _environment.AllocateSet();
            other.shadowSet = _shadow.AllocateSet();
            other.filmSet = _film.AllocateSet();
            other.guidesSet = _guides.AllocateSet();
            other.specularHitSet = _specularHit.AllocateSet();
            other.shadeSets.reserve(_shade.size());
            for (ComputePipeline& pipeline : _shade) {
                other.shadeSets.push_back(pipeline.AllocateSet());
            }

            WriteDescriptors(other.raygenSet, _raygen, other);
            WriteDescriptors(other.extendSet, _extend, other);
            WriteDescriptors(other.prepareSet, _prepareDispatch, other);
            WriteDescriptors(other.sortSet, _materialSort, other);
            WriteDescriptors(other.environmentSet, _environment, other);
            WriteDescriptors(other.shadowSet, _shadow, other);
            WriteDescriptors(other.filmSet, _film, other);
            WriteDescriptors(other.guidesSet, _guides, other);
            WriteDescriptors(other.specularHitSet, _specularHit, other);
            for (std::size_t i = 0; i < _shade.size(); ++i) {
                // Each material's set carries its own textures, which is what
                // lets the generator number a material's samplers from zero.
                WriteDescriptors(other.shadeSets[i], _shade[i], other,
                                 static_cast<int>(i));
            }
            other.descriptorGeneration = _resourceGeneration;
        }
    }

    const VkDescriptorSet raygenSet = slot.raygenSet;
    const VkDescriptorSet extendSet = slot.extendSet;
    const VkDescriptorSet prepareSet = slot.prepareSet;
    const VkDescriptorSet sortSet = slot.sortSet;
    const VkDescriptorSet environmentSet = slot.environmentSet;
    const VkDescriptorSet shadowSet = slot.shadowSet;
    const VkDescriptorSet filmSet = slot.filmSet;
    const VkDescriptorSet guidesSet = slot.guidesSet;
    const VkDescriptorSet specularHitSet = slot.specularHitSet;
    const std::vector<VkDescriptorSet>& shadeSets = slot.shadeSets;

    // Clear the film once; samples accumulate into it. A progressive caller
    // asks not to, and its samples land on top of what is already there.
    // EnsureResolution reallocates on a resolution change, so a continued
    // accumulation can never read a film of the wrong size.
    if (settings.resetAccumulation) {
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            vkCmdFillBuffer(command, _accumulation.Handle(), 0, VK_WHOLE_SIZE, 0);
        });
    }

    FrameBlock block{};
    std::memcpy(block.cameraToWorld, camera.cameraToWorld, sizeof(block.cameraToWorld));
    std::memcpy(block.environmentColor, settings.environmentColor, 3 * sizeof(float));
    std::memcpy(block.sunRadiance, settings.sunRadiance, 3 * sizeof(float));
    block.sunDirection[0] = settings.sunDirection[0];
    block.sunDirection[1] = settings.sunDirection[1];
    block.sunDirection[2] = settings.sunDirection[2];
    block.sunDirection[3] = settings.sunAngularRadius;
    block.resolution[0] = width;
    block.resolution[1] = height;
    block.maxBounces = settings.maxBounces;
    block.tanHalfFov = camera.tanHalfFov;
    block.aspect = camera.aspect;
    block.pathCount = paths;
    block.lightCount = _lightCount;
    block.materialCount = static_cast<std::uint32_t>(_shade.size());
    block.hasDomeTexture = _domeTexture.Valid() ? 1u : 0u;
    block.hasDomeLight = _hasDomeLight ? 1u : 0u;
    block.hasEnvironmentDistribution = _environmentWidth > 0 ? 1u : 0u;
    block.environmentWidth = _environmentWidth;
    block.environmentHeight = _environmentHeight;
    block.environmentConditional = _environmentConditional;
    block.environmentDensity = _environmentDensity;
    block.spectralSamples = _spectralSamples;
    block.chromaTableOffset = _chromaTableOffset;
    block.chromaTableSize = _chromaTableSize;
    block.spectralNormalisation = _spectralNormalisation;
    block.environmentTemperature = _domeColorTemperature;
    block.environmentTemperatureScale = _domeTemperatureScale;
    std::memcpy(block.domeWorldToLight, _domeWorldToLight,
                sizeof(block.domeWorldToLight));
    std::memcpy(block.domeLightToWorld, _domeLightToWorld,
                sizeof(block.domeLightToWorld));
    std::memcpy(block.worldToClip, camera.worldToClip, sizeof(block.worldToClip));
    // The previous frame's matrix, or this one's when there is no previous
    // frame. The second reports no motion, which is the truth: a surface seen
    // for the first time has not moved on screen, and inventing a displacement
    // would send a reconstructor to fetch history that does not exist.
    if (_hasPreviousClip) {
        std::memcpy(block.previousWorldToClip, _previousWorldToClip,
                    sizeof(block.previousWorldToClip));
    } else {
        std::memcpy(block.previousWorldToClip, camera.worldToClip,
                    sizeof(block.previousWorldToClip));
    }
    block.jitter[0] = settings.jitter[0];
    block.jitter[1] = settings.jitter[1];
    block.useFixedJitter = settings.fixedJitter ? 1u : 0u;
    block.lightGeometry = settings.lightGeometry ? 1u : 0u;
    block.linkWords = _linkWords;
    block.domeLightLink = _domeLightLink;
    block.domeShadowLink = _domeShadowLink;

    const std::uint32_t pathGroups = (paths + 63) / 64;
    const std::uint32_t pixelGroupsX = (width + 7) / 8;
    const std::uint32_t pixelGroupsY = (height + 7) / 8;

    // One command buffer per sample, holding every bounce and the film.
    //
    // It used to be one per bounce plus one for the film, which at the
    // gallery's 32 samples and 8 bounces is 288 submits per call -- and every
    // `SubmitImmediate` allocates a command buffer and a fence, submits, and
    // *waits for the device to go idle* before returning. The GPU therefore
    // drained 288 times per frame for no reason: nothing between two bounces is
    // read by the host.
    //
    // Nothing was read by the host except the frame uniform, which is written
    // by it. That is what made the split necessary and what removing it needed:
    // `bounce` was a field of a host-written uniform, so every dispatch in a
    // batched buffer would have read whichever value was written last. It is a
    // push constant now, on the two kernels that vary with it, and the uniform
    // varies per *sample* -- which is exactly the granularity left here.
    //
    // The barriers were always there. A submit boundary is a full barrier for
    // free, so removing it means the explicit ones now carry the whole weight;
    // the synchronisation validation gate is what says they do, and it is part
    // of the GPU suite for exactly this class of change (2026-09-06).
    // Under HDCLAUDE_POISON_PATH_STATE: poison the path state before the
    // frame touches it.
    //
    // A rare, scale-dependent difference that is stable within a process and
    // varies between them is the shape of a read of uninitialised memory: the
    // contents are whatever the driver last left in that allocation, which is
    // fixed for one process and arbitrary across them. Filling every path
    // buffer with a known pattern makes that content the same everywhere, so if
    // the renderer becomes reproducible across processes with this on, the
    // cause is a slot read before it was written.
    // The kernel profile, asked for once. Declined rather than approximated
    // where the device or its queue cannot timestamp: a period of zero or no
    // valid bits means the numbers would be noise, and reporting noise as a
    // breakdown is worse than reporting nothing.
    // The frame log turns this on by itself: its whole purpose is to say
    // whether a slow frame was slow on the device or waiting on the host,
    // and it cannot answer that without the device's own time.
    const bool profileAsked =
        hdclaude::EnvironmentFlag("HDCLAUDE_PROFILE_KERNELS") ||
        hdclaude::EnvironmentFlag("HDCLAUDE_FRAME_LOG");
    if (!_profileKernels && profileAsked &&
        _context.Capabilities().timestampPeriod > 0.0f &&
        _context.Capabilities().timestampValidBits > 0) {
        _profileKernels = true;
    }

    if (hdclaude::EnvironmentFlag("HDCLAUDE_POISON_PATH_STATE")) {
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            for (VulkanBuffer* buffer :
                 {&slot.origin, &slot.direction, &slot.throughput, &slot.radiance,
                  &slot.wavelengths, &slot.pixel, &slot.rng, &slot.scatterPdf, &slot.medium,
                  &slot.heroOnly, &slot.hits, &slot.activeQueue, &slot.nextActiveQueue,
                  &slot.shadowRays, &slot.materialQueue}) {
                if (buffer->Valid()) {
                    vkCmdFillBuffer(command, buffer->Handle(), 0, VK_WHOLE_SIZE,
                                    0xCDCDCDCDu);
                }
            }
            Barrier(command);
        });
    }

    // The kernel profile's query slots. Twelve spans a bounce -- prepare,
    // extend, sort, environment, shade, shadow -- and two for the film, which
    // runs once per sample rather than once per bounce.
    constexpr std::uint32_t kStampsPerBounce = 12;
    const std::uint32_t stampCount = settings.maxBounces * kStampsPerBounce + 2;
    if (_profileKernels && _timestampPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = stampCount;
        if (vkCreateQueryPool(_context.Device(), &poolInfo, nullptr,
                              &_timestampPool) != VK_SUCCESS) {
            _timestampPool = VK_NULL_HANDLE;
            _profileKernels = false;
        }
    }
    // Only the first sample is measured; every other one records nothing.
    const bool profileThisCall = _profileKernels && _timestampPool != VK_NULL_HANDLE;

    for (std::uint32_t sample = 0; sample < settings.samplesPerPixel; ++sample) {
        const bool stamping = profileThisCall && sample == 0;
        block.sampleIndex = settings.firstSample + sample;
        slot.frameUniforms.Write(&block, sizeof(block));

        const bool readStamps = stamping;
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            // Written with ALL_COMMANDS at both ends of a span. Every kernel is
            // already separated by a full barrier, so a span measures that
            // kernel and nothing either side of it.
            const auto stamp = [&](std::uint32_t index) {
                if (stamping) {
                    vkCmdWriteTimestamp2(command,
                                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         _timestampPool, index);
                }
            };
            if (stamping) {
                vkCmdResetQueryPool(command, _timestampPool, 0, stampCount);
            }
            for (std::uint32_t bounce = 0; bounce < settings.maxBounces;
                 ++bounce) {
                const std::uint32_t base = bounce * kStampsPerBounce;
                if (bounce == 0) {
                    if (sample == 0) {
                        // The accumulators measure this call, so they start it
                        // at zero. Cleared here rather than by raygen because
                        // raygen runs once per *sample* and this must not.
                        vkCmdFillBuffer(command, slot.counters.Handle(), 16, 16, 0);
                        Barrier(command);
                    }
                    _raygen.Dispatch(command, raygenSet, pixelGroupsX, pixelGroupsY);
                    Barrier(command);
                } else {
                    // Promote the compacted queue into the active one, then
                    // reset the counters this bounce writes.
                    //
                    // Copied rather than swapped so the descriptor sets stay
                    // valid across bounces: rebinding would mean rewriting
                    // every set every bounce, and the copy is one pass over an
                    // index buffer against a bounce of tracing.
                    //
                    // Order matters. activeCount must take the previous
                    // nextActiveCount *before* that counter is zeroed, or the
                    // bounce dispatches over nothing.
                    VkBufferCopy queueRegion{};
                    queueRegion.size = static_cast<VkDeviceSize>(paths) * 4;
                    vkCmdCopyBuffer(command, slot.nextActiveQueue.Handle(),
                                    slot.activeQueue.Handle(), 1, &queueRegion);

                    VkBufferCopy countRegion{};
                    countRegion.srcOffset = 4;   // nextActiveCount
                    countRegion.dstOffset = 0;   // activeCount
                    countRegion.size = 4;
                    vkCmdCopyBuffer(command, slot.counters.Handle(), slot.counters.Handle(),
                                    1, &countRegion);
                    Barrier(command);

                    // nextActiveCount and shadowCount start this bounce at zero.
                    vkCmdFillBuffer(command, slot.counters.Handle(), 4, 12, 0);
                    Barrier(command);
                }

                // Every dispatch from here is sized by the GPU. The counters
                // that size them -- how many paths are still active, how many
                // each material claims, how many shadow rays shading produced
                // -- are written on the device, and reading one back to size
                // the next dispatch would stall the middle of every bounce
                // (docs/wavefront-integrator.md 2).
                stamp(base + 0);
                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareActive, sizeof(kPrepareActive));
                Barrier(command);
                stamp(base + 1);

                stamp(base + 2);
                _extend.DispatchIndirect(command, extendSet, slot.dispatchArgs,
                                         kDispatchSlotActive * kDispatchArgStride);
                Barrier(command);
                stamp(base + 3);

                // The guides, from the primary hit and only from it. This is
                // the one point in a frame where the first bounce's hit
                // exists: the next bounce overwrites the record and the origin
                // it left behind.
                if (bounce == 0) {
                    _guides.Dispatch(command, guidesSet, pixelGroupsX,
                                     pixelGroupsY);
                    Barrier(command);
                }

                // Group the hits by material: count, prefix-sum, scatter.
                stamp(base + 4);
                _materialSort.DispatchIndirect(
                    command, sortSet, slot.dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride, &kSortCount,
                    sizeof(kSortCount));
                Barrier(command);

                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareMaterials,
                                          sizeof(kPrepareMaterials));
                Barrier(command);

                _materialSort.DispatchIndirect(
                    command, sortSet, slot.dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride, &kSortScatter,
                    sizeof(kSortScatter));
                Barrier(command);
                stamp(base + 5);

                // The misses are still on the active queue; the sort left them
                // there rather than giving them a group.
                stamp(base + 6);
                EnvironmentPush environmentPush;
                environmentPush.bounce = bounce;
                _environment.DispatchIndirect(
                    command, environmentSet, slot.dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride, &environmentPush,
                    sizeof(environmentPush));
                Barrier(command);
                stamp(base + 7);

                stamp(base + 8);
                for (std::size_t i = 0; i < _shade.size(); ++i) {
                    ShadePush push;
                    push.materialId = static_cast<std::uint32_t>(i);
                    push.dispersionAbbe = _materialDispersion[i];
                    push.thinWalled = _materialThinWalled[i];
                    push.bounce = bounce;
                    _shade[i].DispatchIndirect(
                        command, shadeSets[i], slot.dispatchArgs,
                        (kDispatchSlotFirstMaterial + push.materialId) *
                            kDispatchArgStride,
                        &push, sizeof(push));
                    Barrier(command);
                }

                // The specular hit distance, from the probes the first
                // bounce's shading recorded and before the next bounce's
                // extend moves anything.
                if (bounce == 0) {
                    _specularHit.Dispatch(command, specularHitSet, pixelGroupsX,
                                          pixelGroupsY);
                    Barrier(command);
                }

                stamp(base + 9);
                stamp(base + 10);
                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareShadow, sizeof(kPrepareShadow));
                Barrier(command);

                _shadow.DispatchIndirect(command, shadowSet, slot.dispatchArgs,
                                         kDispatchSlotShadow * kDispatchArgStride);
                Barrier(command);
                stamp(base + 11);
            }

            // The film, in the same buffer as the bounces that filled the
            // radiance it reads. The barrier that ends the last bounce is
            // what orders it; it used to be a submit boundary, which was a
            // full barrier obtained by stalling the device.
            stamp(settings.maxBounces * kStampsPerBounce + 0);
            _film.Dispatch(command, filmSet, pathGroups);
            Barrier(command);
            stamp(settings.maxBounces * kStampsPerBounce + 1);
        });

        // Read back while the sample that wrote them is the last thing the
        // device did, which `SubmitImmediate` guarantees by waiting.
        if (readStamps) {
            std::vector<std::uint64_t> ticks(stampCount, 0);
            if (vkGetQueryPoolResults(
                    _context.Device(), _timestampPool, 0, stampCount,
                    ticks.size() * sizeof(std::uint64_t), ticks.data(),
                    sizeof(std::uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS) {
                const double period =
                    static_cast<double>(_context.Capabilities().timestampPeriod);
                // Ticks to milliseconds. A span whose end is not after its start
                // is dropped rather than counted: a timestamp the queue did not
                // write reads as zero, and subtracting it would manufacture a
                // negative or a vast interval out of nothing.
                const auto span = [&](std::uint32_t a, std::uint32_t b) {
                    if (ticks[b] <= ticks[a]) {
                        return 0.0;
                    }
                    return static_cast<double>(ticks[b] - ticks[a]) * period /
                           1.0e6;
                };
                KernelProfile profile;
                for (std::uint32_t bounce = 0; bounce < settings.maxBounces;
                     ++bounce) {
                    const std::uint32_t base = bounce * kStampsPerBounce;
                    profile.prepareMs += span(base + 0, base + 1);
                    profile.extendMs += span(base + 2, base + 3);
                    profile.sortMs += span(base + 4, base + 5);
                    profile.environmentMs += span(base + 6, base + 7);
                    profile.shadeMs += span(base + 8, base + 9);
                    profile.shadowMs += span(base + 10, base + 11);
                }
                const std::uint32_t filmBase =
                    settings.maxBounces * kStampsPerBounce;
                profile.filmMs = span(filmBase + 0, filmBase + 1);
                profile.valid = true;
                _kernelProfile = profile;
            }
        }
    }

    // --- Resolve ------------------------------------------------------------
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = static_cast<VkDeviceSize>(paths) * 16;
        vkCmdCopyBuffer(command, _accumulation.Handle(), slot.readback.Handle(), 1,
                        &region);
        // The ray accumulators, copied in the same submit as the film. One
        // readback for a whole call, after every dispatch it describes has
        // finished, which is the only point a count of rays can be taken
        // without stalling the frame that is producing it.
        VkBufferCopy guides{};
        guides.size = static_cast<VkDeviceSize>(paths) * 4;
        vkCmdCopyBuffer(command, slot.guideDepth.Handle(),
                        slot.guideReadback.Handle(), 1, &guides);

        VkBufferCopy motion{};
        motion.size = static_cast<VkDeviceSize>(paths) * 8;
        vkCmdCopyBuffer(command, slot.guideMotion.Handle(),
                        slot.motionReadback.Handle(), 1, &motion);

        VkBufferCopy surface{};
        surface.size = static_cast<VkDeviceSize>(paths) * 48;
        vkCmdCopyBuffer(command, slot.guideSurface.Handle(),
                        slot.surfaceReadback.Handle(), 1, &surface);

        VkBufferCopy specularRay{};
        specularRay.size = static_cast<VkDeviceSize>(paths) * 32;
        vkCmdCopyBuffer(command, slot.guideSpecularRay.Handle(),
                        slot.specularRayReadback.Handle(), 1, &specularRay);

        VkBufferCopy rays{};
        rays.srcOffset = 16;
        rays.size = 16;
        vkCmdCopyBuffer(command, slot.counters.Handle(), slot.rayReadback.Handle(), 1,
                        &rays);
    });

    std::uint32_t rayCounts[4] = {0, 0, 0, 0};
    std::memcpy(rayCounts, slot.rayReadback.MappedData(), sizeof(rayCounts));
    _tracedRayCount += rayCounts[0];
    _shadowRayCount += rayCounts[1];
    // Folded rather than summed, so the order calls arrive in is part of the
    // answer: a render is the whole sequence, not a bag of them.
    _hitHash = _hitHash * 1099511628211ull + rayCounts[2];
    _rayHash = _rayHash * 1099511628211ull + rayCounts[3];

    _lastDepth.assign(static_cast<std::size_t>(paths), 1.0f);
    std::memcpy(_lastDepth.data(), slot.guideReadback.MappedData(),
                _lastDepth.size() * sizeof(float));
    _lastMotion.assign(static_cast<std::size_t>(paths) * 2, 0.0f);
    std::memcpy(_lastMotion.data(), slot.motionReadback.MappedData(),
                _lastMotion.size() * sizeof(float));

    // Split into the three guides a consumer asks for by name, rather than
    // handed on in the buffer's own stride.
    {
        const std::size_t pixels = static_cast<std::size_t>(paths);
        const auto* surface =
            static_cast<const float*>(slot.surfaceReadback.MappedData());
        _lastNormalRoughness.assign(pixels * 4, 0.0f);
        _lastDiffuseAlbedo.assign(pixels * 3, 0.0f);
        _lastSpecularAlbedo.assign(pixels * 3, 0.0f);
        for (std::size_t i = 0; i < pixels; ++i) {
            const float* s = surface + i * 12;
            std::memcpy(&_lastNormalRoughness[i * 4], s, 4 * sizeof(float));
            std::memcpy(&_lastDiffuseAlbedo[i * 3], s + 4, 3 * sizeof(float));
            std::memcpy(&_lastSpecularAlbedo[i * 3], s + 8, 3 * sizeof(float));
        }
        const auto* probes =
            static_cast<const float*>(slot.specularRayReadback.MappedData());
        _lastSpecularHitDistance.assign(pixels, 65504.0f);
        for (std::size_t i = 0; i < pixels; ++i) {
            _lastSpecularHitDistance[i] = probes[i * 8 + 7];
        }
    }

    // This frame becomes the next frame's past.
    std::memcpy(_previousWorldToClip, camera.worldToClip,
                sizeof(_previousWorldToClip));
    _hasPreviousClip = true;

    std::vector<float> image(static_cast<std::size_t>(paths) * 4);
    std::memcpy(image.data(), slot.readback.MappedData(), image.size() * sizeof(float));

    // Divide by the sample count each pixel actually received.
    for (std::size_t i = 0; i < paths; ++i) {
        const float count = image[i * 4 + 3];
        if (count > 0.0f) {
            const float inverse = 1.0f / count;
            image[i * 4 + 0] *= inverse;
            image[i * 4 + 1] *= inverse;
            image[i * 4 + 2] *= inverse;
        }
        image[i * 4 + 3] = 1.0f;
    }
    return image;
}

}  // namespace hdclaude
