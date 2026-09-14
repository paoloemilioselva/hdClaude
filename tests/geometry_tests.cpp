// Geometry and traversal tests.
//
// Scenes are built programmatically, never read from USD: the GPU backend has
// no USD dependency, and geometry reaches it only through the Scene struct that
// the Hydra mesh adapter will fill. Testing with a stage here would couple the
// two and hide whichever of them was wrong.
//
// The traversal check compares a hit *pattern*, not a hit count. A structure
// built at the wrong scale, or with an instance transform transposed, still
// produces hits -- just the wrong ones.

#include "test_support.h"

#include "hdclaude/gpu/acceleration_structure.h"
#include "hdclaude/gpu/compute_pipeline.h"
#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/gpu/scene.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

using namespace hdclaude;

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;

struct PushParams {
    std::uint32_t width;
    std::uint32_t height;
    float orthoHalfExtent;
    float rayOriginZ;
};

struct Hit {
    std::int32_t instance;
    std::int32_t primitive;
    std::int32_t baryU;
    std::int32_t distance;
};

std::string ReadFile(const char* path)
{
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

/// A unit quad in the z = 0 plane, spanning [-1, 1] in x and y.
MeshPrototype MakeQuad(const char* name)
{
    MeshPrototype prototype;
    prototype.debugName = name;
    prototype.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,
    };
    prototype.indices = {0, 1, 2, 0, 2, 3};
    prototype.normals = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    return prototype;
}

Transform3x4 Translation(float x, float y, float z)
{
    Transform3x4 t;
    t.m[3] = x;
    t.m[7] = y;
    t.m[11] = z;
    return t;
}

class Tracer {
  public:
    Tracer(const VulkanContext& context, VulkanAllocator& allocator,
           const GlslCompiler& compiler)
        : _context(context), _allocator(allocator)
    {
        const GlslCompileResult compiled =
            compiler.Compile(ReadFile(HDCLAUDE_TRAVERSAL_KERNEL),
                             GlslCompileOptions{ShaderStage::Compute, "main",
                                                "traversal", false});
        if (!compiled.ok) {
            std::fprintf(stderr, "traversal kernel failed:\n%s\n",
                         compiled.log.c_str());
            return;
        }

        std::vector<BindingDescription> bindings(2);
        bindings[0].binding = 0;
        bindings[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[0].debugName = "scene";
        bindings[1].binding = 1;
        bindings[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].debugName = "results";

        _pipeline = ComputePipeline(context, compiled.spirv, bindings,
                                    sizeof(PushParams), "traversal");
        _ready = true;
    }

    bool Ready() const { return _ready; }

    std::vector<Hit> Trace(const TopLevelStructure& tlas, float halfExtent,
                           float originZ)
    {
        BufferDescription description;
        description.size = kWidth * kHeight * sizeof(Hit);
        description.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        description.domain = BufferDomain::HostReadback;
        description.debugName = "traversal.results";
        VulkanBuffer results(_allocator, description);
        std::memset(results.MappedData(), 0,
                    static_cast<std::size_t>(description.size));

        VkDescriptorSet set = _pipeline.AllocateSet();

        VkAccelerationStructureKHR handle = tlas.Handle();
        VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        accelerationWrite.accelerationStructureCount = 1;
        accelerationWrite.pAccelerationStructures = &handle;

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        write.pNext = &accelerationWrite;
        vkUpdateDescriptorSets(_context.Device(), 1, &write, 0, nullptr);

        _pipeline.WriteBuffer(set, 1, results);

        PushParams push{kWidth, kHeight, halfExtent, originZ};
        _context.SubmitImmediate([&](VkCommandBuffer command) {
            _pipeline.Dispatch(command, set, (kWidth + 7) / 8, (kHeight + 7) / 8, 1,
                               &push, sizeof(push));
        });

        std::vector<Hit> hits(kWidth * kHeight);
        std::memcpy(hits.data(), results.MappedData(),
                    static_cast<std::size_t>(description.size));
        return hits;
    }

  private:
    const VulkanContext& _context;
    VulkanAllocator& _allocator;
    ComputePipeline _pipeline;
    bool _ready = false;
};

std::uint32_t CountHits(const std::vector<Hit>& hits)
{
    std::uint32_t count = 0;
    for (const Hit& hit : hits) {
        if (hit.instance >= 0) ++count;
    }
    return count;
}

/// The hit at a normalised viewport coordinate, for pattern assertions.
const Hit& At(const std::vector<Hit>& hits, float u, float v)
{
    const auto x = static_cast<std::uint32_t>(u * (kWidth - 1));
    const auto y = static_cast<std::uint32_t>(v * (kHeight - 1));
    return hits[y * kWidth + x];
}

void TestSingleQuadIsHitEverywhere(Tracer& tracer, const VulkanContext& context,
                                   VulkanAllocator& allocator)
{
    Scene scene;
    scene.prototypes.push_back(MakeQuad("quad"));
    scene.instances.push_back({0, Transform3x4{}, 0, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);

    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(0));
    CHECK(accelerator.Tlas().Valid());
    CHECK_EQ(accelerator.Tlas().InstanceCount(), std::uint32_t(1));

    // Viewport exactly covers the quad, so every ray must hit it.
    const std::vector<Hit> hits = tracer.Trace(accelerator.Tlas(), 0.99f, 5.0f);
    CHECK_EQ(CountHits(hits), kWidth * kHeight);

    // Distance from z = 5 to the plane at z = 0.
    const Hit& centre = At(hits, 0.5f, 0.5f);
    CHECK_EQ(centre.instance, 0);
    CHECK_NEAR(centre.distance / 65536.0, 5.0, 0.01);

    // Both triangles of the quad must be represented, or the index buffer was
    // uploaded wrong in a way a hit count alone would not reveal.
    bool sawFirst = false;
    bool sawSecond = false;
    for (const Hit& hit : hits) {
        if (hit.primitive == 0) sawFirst = true;
        if (hit.primitive == 1) sawSecond = true;
    }
    CHECK(sawFirst);
    CHECK(sawSecond);
}

void TestViewportLargerThanQuadMissesOutside(Tracer& tracer,
                                             const VulkanContext& context,
                                             VulkanAllocator& allocator)
{
    Scene scene;
    scene.prototypes.push_back(MakeQuad("quad"));
    scene.instances.push_back({0, Transform3x4{}, 0, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);

    // Viewport twice the quad's extent: the quad covers the centre quarter, so
    // roughly a quarter of rays hit. This is what catches a structure built at
    // the wrong scale, which a full-coverage test cannot.
    const std::vector<Hit> hits = tracer.Trace(accelerator.Tlas(), 2.0f, 5.0f);
    const double coverage =
        static_cast<double>(CountHits(hits)) / (kWidth * kHeight);
    CHECK_NEAR(coverage, 0.25, 0.03);

    CHECK(At(hits, 0.5f, 0.5f).instance == 0);
    CHECK(At(hits, 0.02f, 0.02f).instance == -1);
    CHECK(At(hits, 0.98f, 0.98f).instance == -1);
}

void TestInstanceTransformsArePlaced(Tracer& tracer, const VulkanContext& context,
                                     VulkanAllocator& allocator)
{
    // One prototype, two placements. Reuse must give them a single structure,
    // and the transforms must actually move them apart.
    Scene scene;
    scene.prototypes.push_back(MakeQuad("quad"));
    scene.instances.push_back({0, Translation(-2.0f, 0.0f, 0.0f), 0, true});
    scene.instances.push_back({0, Translation(2.0f, 0.0f, 0.0f), 1, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);

    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.Tlas().InstanceCount(), std::uint32_t(2));

    const std::vector<Hit> hits = tracer.Trace(accelerator.Tlas(), 4.0f, 5.0f);

    // Left instance at x = -2, right at x = +2, nothing at the centre. A
    // transposed or ignored transform puts both at the origin and fails here.
    CHECK_EQ(At(hits, 0.25f, 0.5f).instance, 0);
    CHECK_EQ(At(hits, 0.75f, 0.5f).instance, 1);
    CHECK_EQ(At(hits, 0.5f, 0.5f).instance, -1);
}

void TestIdenticalGeometryReusesOneStructure(const VulkanContext& context,
                                             VulkanAllocator& allocator)
{
    // Two prototypes with identical geometry and different names. Reuse follows
    // the fingerprint, so they must share one structure.
    Scene scene;
    scene.prototypes.push_back(MakeQuad("first"));
    scene.prototypes.push_back(MakeQuad("second"));
    scene.instances.push_back({0, Transform3x4{}, 0, true});
    scene.instances.push_back({1, Translation(3.0f, 0.0f, 0.0f), 0, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);

    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.Tlas().InstanceCount(), std::uint32_t(2));
}

/// A deforming mesh keeps its structure and moves its bounds.
///
/// Two claims, and the second is the one that matters. The counters must say a
/// refit happened -- a refit that silently turns into a rebuild is a
/// performance defect with no trace in the image, which is the same reason the
/// reuse counters exist. And the refit must actually move the geometry: an
/// update that wrote new vertices but left the tree describing the old ones
/// would still report a hit, at the place the mesh used to be, and every
/// counter would look right.
void TestDeformationRefitsRatherThanRebuilds(Tracer& tracer,
                                             const VulkanContext& context,
                                             VulkanAllocator& allocator)
{
    Scene scene;
    scene.prototypes.push_back(MakeQuad("deforming"));
    scene.instances.push_back({0, Transform3x4{}, 0, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastRefitCount(), std::uint32_t(0));

    // The same mesh at a later moment: the vertices move, the topology does
    // not. Nothing holds an updatable structure for it yet -- the first build
    // had no evidence this geometry deforms -- so this rebuilds, and asks for
    // one, which is what makes the frame after it cheap.
    const auto shift = [&scene](float by) {
        for (std::size_t i = 0; i < scene.prototypes[0].positions.size(); i += 3) {
            scene.prototypes[0].positions[i] += by;
        }
    };

    shift(2.0f);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastRefitCount(), std::uint32_t(0));

    // Moved again. Now there is an updatable structure of this topology, and
    // the tree is kept.
    shift(-2.0f);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(0));
    CHECK_EQ(accelerator.LastRefitCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(0));

    // Back at the origin, which is where the last shift put it. The view spans
    // x in [-4, 4], so the quad's own two units are around the middle.
    {
        const std::vector<Hit> hits = tracer.Trace(accelerator.Tlas(), 4.0f, 5.0f);
        CHECK_EQ(At(hits, 0.5f, 0.5f).instance, 0);
        CHECK_EQ(At(hits, 0.8f, 0.5f).instance, -1);
    }

    // And now the assertion the counters cannot make: refit once more, to a
    // place the quad has never been, and look. A tree still describing the old
    // bounds answers at 0.5 and misses at 0.75.
    shift(2.0f);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastRefitCount(), std::uint32_t(1));
    {
        const std::vector<Hit> hits = tracer.Trace(accelerator.Tlas(), 4.0f, 5.0f);
        CHECK_EQ(At(hits, 0.75f, 0.5f).instance, 0);
        CHECK_EQ(At(hits, 0.5f, 0.5f).instance, -1);
    }

    // A topology change is not refittable and must rebuild: the tree partitions
    // primitives, and there is no sense in which a different set of them is the
    // same tree moved.
    scene.prototypes[0].indices = {0, 1, 2};
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastRefitCount(), std::uint32_t(0));

    std::printf("  deformation refits, and the refit moves the geometry\n");
}

void TestUnchangedSceneRebuildsNothing(const VulkanContext& context,
                                       VulkanAllocator& allocator)
{
    Scene scene;
    scene.prototypes.push_back(MakeQuad("a"));
    MeshPrototype other = MakeQuad("b");
    other.positions[0] = -1.5f;  // genuinely different geometry
    scene.prototypes.push_back(other);
    scene.instances.push_back({0, Transform3x4{}, 0, true});
    scene.instances.push_back({1, Translation(3.0f, 0.0f, 0.0f), 0, true});

    SceneAccelerator accelerator(context, allocator);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(2));

    // Republishing the same scene must rebuild nothing. Reuse that silently
    // fails is a performance defect invisible in an image, so it is asserted
    // rather than assumed (docs/lessons-from-hdcodex.md R7).
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(0));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(2));

    // Moving an instance is not a geometry change: still no BLAS rebuild.
    scene.instances[1].transform = Translation(5.0f, 1.0f, 0.0f);
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(0));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(2));

    // Changing a vertex is, and must rebuild exactly the one prototype.
    scene.prototypes[1].positions[1] = -1.25f;
    accelerator.Update(scene);
    CHECK_EQ(accelerator.LastBuiltCount(), std::uint32_t(1));
    CHECK_EQ(accelerator.LastReusedCount(), std::uint32_t(1));
}

void TestOpacityClassIsPartOfIdentity()
{
    // A structure built for opaque geometry uses different build flags and
    // cannot be reused for cut-out geometry, so the opacity class has to be
    // part of the fingerprint.
    MeshPrototype opaque = MakeQuad("q");
    MeshPrototype cutout = MakeQuad("q");
    cutout.opacity = OpacityClass::Cutout;
    CHECK(opaque.Fingerprint() != cutout.Fingerprint());

    // A rename is not a geometry change.
    MeshPrototype renamed = MakeQuad("different name");
    CHECK_EQ(opaque.Fingerprint(), renamed.Fingerprint());
}

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("hdClaudeGeometryTests\n");

    TestOpacityClassIsPartOfIdentity();

    std::unique_ptr<VulkanContext> context;
    try {
        VulkanContextOptions options;
        options.enableValidation = true;
        context = std::make_unique<VulkanContext>(options);
    } catch (const VulkanError& error) {
        std::printf("SKIP: no usable Vulkan device (%s)\n", error.what());
        return 0;
    }
    if (!context->ValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: validation layer unavailable; see docs/building.md\n");
        return 1;
    }

    // Core validation alone is not the gate. Synchronisation validation is what
    // reports a buffer one kernel writes that the next cannot yet see, and it
    // can be off with the layer present, so a clean count would say nothing.
    if (!context->SynchronisationValidationEnabled()) {
        std::fprintf(stderr,
                     "FAIL: the validation layer is running without "
                     "synchronisation validation (it lacks "
                     "VK_EXT_layer_settings), so the validation gate would "
                     "pass without checking kernel hazards; see "
                     "docs/building.md\n");
        return 1;
    }

    const GlslCompiler compiler;
    {
        VulkanAllocator allocator(*context);
        Tracer tracer(*context, allocator, compiler);
        CHECK(tracer.Ready());
        if (tracer.Ready()) {
            TestSingleQuadIsHitEverywhere(tracer, *context, allocator);
            TestViewportLargerThanQuadMissesOutside(tracer, *context, allocator);
            TestInstanceTransformsArePlaced(tracer, *context, allocator);
            TestDeformationRefitsRatherThanRebuilds(tracer, *context, allocator);
        }
        TestIdenticalGeometryReusesOneStructure(*context, allocator);
        TestUnchangedSceneRebuildsNothing(*context, allocator);

        const std::uint64_t errors = context->ValidationErrorCount();
        if (errors != 0) {
            std::fprintf(stderr, "FAIL: %llu validation error(s). Last: %s\n",
                         static_cast<unsigned long long>(errors),
                         context->LastValidationError().c_str());
        }
        CHECK_EQ(errors, std::uint64_t(0));
    }

    return hdclaude_test::Summarize("hdClaudeGeometryTests");
}
