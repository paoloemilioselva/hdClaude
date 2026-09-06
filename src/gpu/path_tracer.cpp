#include "hdclaude/gpu/path_tracer.h"

#include "hdclaude/gpu/environment_distribution.h"
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
    bindings.push_back(storage(22, "environmentDistribution"));
    bindings.push_back(storage(23, "pathWavelengths"));
    bindings.push_back(storage(24, "spectralTables"));

    return bindings;
}

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
    std::uint32_t bounce;
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
};

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
    std::uint64_t normals;
    std::uint64_t uvs;
    std::uint64_t triangleMaterials;
    float objectToWorld[12];
    float worldToObject[12];
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
    _environment = build("environment.comp.glsl", 0);
    _shadow = build("shadow.comp.glsl", 0);
    _film = build("film.comp.glsl", 0);

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
        if (prototype.triangleMaterials.empty()) {
            continue;
        }
        prototypeMaterialOffset[i] = triangleMaterials.size();
        triangleMaterials.insert(triangleMaterials.end(),
                                 prototype.triangleMaterials.begin(),
                                 prototype.triangleMaterials.end());
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
    for (const CompiledMaterial& material : materials) {
        _shade.push_back(ComputePipeline(_context, material.spirv, bindings,
                                         sizeof(std::uint32_t),
                                         "shade." + material.debugName));
        _materialTextureSlots.push_back(material.textureSlots);
    }

    // --- The sort's tables ---------------------------------------------------
    // Sized here rather than with the path state, because their stride is the
    // number of materials rather than the number of pixels. Both are allocated
    // even when the scene has no material at all: a descriptor set must name a
    // real buffer, and the kernels read frame.materialCount rather than a
    // buffer's size.
    const auto materialCount = static_cast<std::uint32_t>(_shade.size());
    const std::uint32_t tableEntries = std::max<std::uint32_t>(1, materialCount);
    _materialTable = MakeStorage(_allocator, VkDeviceSize(tableEntries) * 3 * 4,
                                 "sort.materialTable");

    BufferDescription argsDescription;
    argsDescription.size =
        (VkDeviceSize(kDispatchSlotFirstMaterial) + tableEntries) * kDispatchArgStride;
    argsDescription.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    argsDescription.domain = BufferDomain::DeviceLocal;
    argsDescription.debugName = "sort.dispatchArgs";
    _dispatchArgs = VulkanBuffer(_allocator, argsDescription);

    // The command processor reads every slot of an indirect buffer it is
    // pointed at, including one this frame's kernels never wrote. Zeroing
    // means an unwritten slot dispatches nothing rather than whatever the
    // allocation happened to contain.
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        vkCmdFillBuffer(command, _dispatchArgs.Handle(), 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(command, _materialTable.Handle(), 0, VK_WHOLE_SIZE, 0);
    });
}

void PathTracer::EnsureResolution(std::uint32_t width, std::uint32_t height)
{
    if (width == _width && height == _height) {
        return;
    }

    const std::uint32_t paths = width * height;

    // Built into locals and published together, so a failure part-way leaves
    // the previous resolution's buffers intact rather than a half-resized set
    // (docs/architecture.md 6 rule 1).
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
    VulkanBuffer hits = MakeStorage(_allocator, paths * 16, "path.hits");
    VulkanBuffer counters = MakeStorage(_allocator, 16, "counters");
    VulkanBuffer activeQueue = MakeStorage(_allocator, paths * 4, "queue.active");
    VulkanBuffer nextQueue = MakeStorage(_allocator, paths * 4, "queue.nextActive");
    VulkanBuffer shadowRays = MakeStorage(_allocator, paths * 64, "queue.shadow");
    // The sorted queue holds the active paths that hit geometry, which is at
    // most every path.
    VulkanBuffer materialQueue = MakeStorage(_allocator, paths * 4, "queue.material");
    VulkanBuffer accumulation = MakeStorage(_allocator, paths * 16, "film");

    BufferDescription uniformDescription;
    uniformDescription.size = sizeof(FrameBlock);
    uniformDescription.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uniformDescription.domain = BufferDomain::HostUpload;
    uniformDescription.debugName = "frame";
    VulkanBuffer frameUniforms(_allocator, uniformDescription);

    BufferDescription readbackDescription;
    readbackDescription.size = paths * 16;
    readbackDescription.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackDescription.domain = BufferDomain::HostReadback;
    readbackDescription.debugName = "film.readback";
    VulkanBuffer readback(_allocator, readbackDescription);

    _origin = std::move(origin);
    _direction = std::move(direction);
    _throughput = std::move(throughput);
    _radiance = std::move(radiance);
    _wavelengths = std::move(wavelengths);
    _pixel = std::move(pixel);
    _rng = std::move(rng);
    _scatterPdf = std::move(scatterPdf);
    _hits = std::move(hits);
    _counters = std::move(counters);
    _activeQueue = std::move(activeQueue);
    _nextActiveQueue = std::move(nextQueue);
    _shadowRays = std::move(shadowRays);
    _materialQueue = std::move(materialQueue);
    _accumulation = std::move(accumulation);
    _frameUniforms = std::move(frameUniforms);
    _readback = std::move(readback);
    _width = width;
    _height = height;
}

void PathTracer::WriteDescriptors(VkDescriptorSet set,
                                  const ComputePipeline& pipeline, int material)
{
    pipeline.WriteBuffer(set, 0, _frameUniforms, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    pipeline.WriteBuffer(set, 1, _origin);
    pipeline.WriteBuffer(set, 2, _direction);
    pipeline.WriteBuffer(set, 3, _throughput);
    pipeline.WriteBuffer(set, 4, _radiance);
    pipeline.WriteBuffer(set, 5, _pixel);
    pipeline.WriteBuffer(set, 6, _rng);
    pipeline.WriteBuffer(set, 7, _hits);
    pipeline.WriteBuffer(set, 8, _counters);
    pipeline.WriteBuffer(set, 9, _activeQueue);
    pipeline.WriteBuffer(set, 10, _nextActiveQueue);
    pipeline.WriteBuffer(set, 11, _shadowRays);
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

    pipeline.WriteBuffer(set, 18, _materialQueue);
    pipeline.WriteBuffer(set, 19, _materialTable);
    pipeline.WriteBuffer(set, 20, _dispatchArgs);
    pipeline.WriteBuffer(set, 21, _scatterPdf);
    pipeline.WriteBuffer(set, 22, _environmentDistribution);
    pipeline.WriteBuffer(set, 23, _wavelengths);
    pipeline.WriteBuffer(set, 24, _spectralTables);
}

std::vector<std::uint32_t> PathTracer::MaterialCounts() const
{
    if (_shade.empty() || !_materialTable.Valid()) {
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
        vkCmdCopyBuffer(command, _materialTable.Handle(), readback.Handle(), 1,
                        &region);
    });

    std::vector<std::uint32_t> counts(_shade.size());
    std::memcpy(counts.data(), readback.MappedData(), size);
    return counts;
}

std::vector<float> PathTracer::Render(std::uint32_t width, std::uint32_t height,
                                      const RenderCamera& camera,
                                      const RenderSettings& settings)
{
    _context.RequireLive("PathTracer::Render");
    EnsureResolution(width, height);

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
    _raygen.ResetSets();
    _extend.ResetSets();
    _prepareDispatch.ResetSets();
    _materialSort.ResetSets();
    _environment.ResetSets();
    _shadow.ResetSets();
    _film.ResetSets();
    for (ComputePipeline& pipeline : _shade) {
        pipeline.ResetSets();
    }

    // Descriptor sets are allocated per pipeline but written identically: the
    // binding table is shared, so every kernel sees the same state.
    VkDescriptorSet raygenSet = _raygen.AllocateSet();
    VkDescriptorSet extendSet = _extend.AllocateSet();
    VkDescriptorSet prepareSet = _prepareDispatch.AllocateSet();
    VkDescriptorSet sortSet = _materialSort.AllocateSet();
    VkDescriptorSet environmentSet = _environment.AllocateSet();
    VkDescriptorSet shadowSet = _shadow.AllocateSet();
    VkDescriptorSet filmSet = _film.AllocateSet();
    std::vector<VkDescriptorSet> shadeSets;
    shadeSets.reserve(_shade.size());
    for (ComputePipeline& pipeline : _shade) {
        shadeSets.push_back(pipeline.AllocateSet());
    }

    WriteDescriptors(raygenSet, _raygen);
    WriteDescriptors(extendSet, _extend);
    WriteDescriptors(prepareSet, _prepareDispatch);
    WriteDescriptors(sortSet, _materialSort);
    WriteDescriptors(environmentSet, _environment);
    WriteDescriptors(shadowSet, _shadow);
    WriteDescriptors(filmSet, _film);
    for (std::size_t i = 0; i < _shade.size(); ++i) {
        // Each material's set carries its own textures, which is what lets the
        // generator number a material's samplers from zero.
        WriteDescriptors(shadeSets[i], _shade[i], static_cast<int>(i));
    }

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

    const std::uint32_t pathGroups = (paths + 63) / 64;
    const std::uint32_t pixelGroupsX = (width + 7) / 8;
    const std::uint32_t pixelGroupsY = (height + 7) / 8;

    for (std::uint32_t sample = 0; sample < settings.samplesPerPixel; ++sample) {
        block.sampleIndex = settings.firstSample + sample;

        for (std::uint32_t bounce = 0; bounce < settings.maxBounces; ++bounce) {
            block.bounce = bounce;
            _frameUniforms.Write(&block, sizeof(block));

            _context.SubmitImmediate([&](VkCommandBuffer command) {
                if (bounce == 0) {
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
                    vkCmdCopyBuffer(command, _nextActiveQueue.Handle(),
                                    _activeQueue.Handle(), 1, &queueRegion);

                    VkBufferCopy countRegion{};
                    countRegion.srcOffset = 4;   // nextActiveCount
                    countRegion.dstOffset = 0;   // activeCount
                    countRegion.size = 4;
                    vkCmdCopyBuffer(command, _counters.Handle(), _counters.Handle(),
                                    1, &countRegion);
                    Barrier(command);

                    // nextActiveCount and shadowCount start this bounce at zero.
                    vkCmdFillBuffer(command, _counters.Handle(), 4, 12, 0);
                    Barrier(command);
                }

                // Every dispatch from here is sized by the GPU. The counters
                // that size them -- how many paths are still active, how many
                // each material claims, how many shadow rays shading produced
                // -- are written on the device, and reading one back to size
                // the next dispatch would stall the middle of every bounce
                // (docs/wavefront-integrator.md 2).
                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareActive, sizeof(kPrepareActive));
                Barrier(command);

                _extend.DispatchIndirect(command, extendSet, _dispatchArgs,
                                         kDispatchSlotActive * kDispatchArgStride);
                Barrier(command);

                // Group the hits by material: count, prefix-sum, scatter.
                _materialSort.DispatchIndirect(
                    command, sortSet, _dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride, &kSortCount,
                    sizeof(kSortCount));
                Barrier(command);

                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareMaterials,
                                          sizeof(kPrepareMaterials));
                Barrier(command);

                _materialSort.DispatchIndirect(
                    command, sortSet, _dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride, &kSortScatter,
                    sizeof(kSortScatter));
                Barrier(command);

                // The misses are still on the active queue; the sort left them
                // there rather than giving them a group.
                _environment.DispatchIndirect(
                    command, environmentSet, _dispatchArgs,
                    kDispatchSlotActive * kDispatchArgStride);
                Barrier(command);

                for (std::size_t i = 0; i < _shade.size(); ++i) {
                    const auto materialId = static_cast<std::uint32_t>(i);
                    _shade[i].DispatchIndirect(
                        command, shadeSets[i], _dispatchArgs,
                        (kDispatchSlotFirstMaterial + materialId) * kDispatchArgStride,
                        &materialId, sizeof(materialId));
                    Barrier(command);
                }

                _prepareDispatch.Dispatch(command, prepareSet, 1, 1, 1,
                                          &kPrepareShadow, sizeof(kPrepareShadow));
                Barrier(command);

                _shadow.DispatchIndirect(command, shadowSet, _dispatchArgs,
                                         kDispatchSlotShadow * kDispatchArgStride);
                Barrier(command);
            });
        }

        _frameUniforms.Write(&block, sizeof(block));
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            _film.Dispatch(command, filmSet, pathGroups);
        });
    }

    // --- Resolve ------------------------------------------------------------
    _context.SubmitImmediate([&](VkCommandBuffer command) {
        VkBufferCopy region{};
        region.size = static_cast<VkDeviceSize>(paths) * 16;
        vkCmdCopyBuffer(command, _accumulation.Handle(), _readback.Handle(), 1,
                        &region);
    });

    std::vector<float> image(static_cast<std::size_t>(paths) * 4);
    std::memcpy(image.data(), _readback.MappedData(), image.size() * sizeof(float));

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
