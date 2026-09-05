// The first images.
//
// A MaterialX material, generated through genglsl_pt and joined to the shade
// kernel, shading real geometry through the wavefront integrator. Scenes are
// built in C++ rather than read from USD: geometry reaches the backend only
// through the Scene struct the Hydra adapter will fill, and a stage here would
// make a failure ambiguous between the two.
//
// The assertions are about light transport, not about pixel values: a lit
// surface is brighter than an unlit one, a shadowed region is darker than its
// surroundings, a red surface is red. Those hold for any correct renderer and
// none of them holds for a renderer that is merely producing output.

#include "test_support.h"

#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/gpu/path_tracer.h"
#include "hdclaude/gpu/scene.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXCore/Document.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/GenOptions.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace mx = MaterialX;
using namespace hdclaude;

namespace {

constexpr std::uint32_t kWidth = 128;
constexpr std::uint32_t kHeight = 128;

mx::NodePtr AddNode(mx::DocumentPtr doc, const std::string& category,
                    const std::string& name, const std::string& type)
{
    mx::NodePtr node = doc->addNode(category, name, type);
    if (!node || !node->getNodeDef()) {
        std::fprintf(stderr, "  no nodedef for '%s'\n", category.c_str());
        return nullptr;
    }
    return node;
}

template <typename T>
void SetValue(mx::NodePtr node, const std::string& input, const T& value)
{
    if (!node) return;
    if (mx::InputPtr port = node->addInputFromNodeDef(input)) port->setValue(value);
}

void Connect(mx::NodePtr node, const std::string& input, mx::NodePtr source)
{
    if (!node || !source) return;
    if (mx::InputPtr port = node->addInputFromNodeDef(input)) {
        port->setConnectedNode(source);
    }
}

/// Generate a diffuse material of the given colour and compile it with the
/// shade kernel into a shading pipeline.
CompiledMaterial MakeDiffuseMaterial(mx::DocumentPtr libraries,
                                     const GlslCompiler& compiler,
                                     const std::string& shadeKernel,
                                     const mx::Color3& colour,
                                     const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr bsdf = AddNode(doc, "oren_nayar_diffuse_bsdf", "d", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "color", colour);
    SetValue(bsdf, "roughness", 0.0f);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    mx::ShaderGeneratorPtr generator = PathTracerShaderGenerator::create();
    mx::GenContext genContext(generator);
    genContext.registerSourceCodeSearchPath(DefaultMaterialXSourceSearchPath());
    genContext.getOptions().shaderInterfaceType = mx::SHADER_INTERFACE_REDUCED;

    std::vector<mx::TypedElementPtr> renderable;
    mx::findRenderableElements(doc, renderable);

    if (renderable.empty()) {
        std::fprintf(stderr, "  no renderable element for %s\n", name.c_str());
        return CompiledMaterial{};
    }
    mx::ShaderPtr shader = generator->generate(name, renderable.front(), genContext);
    const std::string source = shader->getSourceCode(mx::Stage::PIXEL) + shadeKernel;

    GlslCompileOptions options;
    options.moduleName = name;
    const GlslCompileResult compiled = compiler.Compile(source, options);
    if (!compiled.ok) {
        std::fprintf(stderr, "  material %s failed:\n%s\n", name.c_str(),
                     compiled.log.c_str());
    }
    return CompiledMaterial{compiled.spirv, name};
}

MeshPrototype MakeQuad()
{
    MeshPrototype prototype;
    prototype.debugName = "quad";
    prototype.positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    prototype.indices = {0, 1, 2, 0, 2, 3};
    prototype.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    return prototype;
}

Transform3x4 Transform(float sx, float sy, float sz, float tx, float ty, float tz)
{
    Transform3x4 t;
    t.m[0] = sx;  t.m[3] = tx;
    t.m[5] = sy;  t.m[7] = ty;
    t.m[10] = sz; t.m[11] = tz;
    return t;
}

/// Camera looking down -Z from +Z, USD convention.
RenderCamera LookDownZ(float distance)
{
    RenderCamera camera;
    // Column-major identity with a translation in the last column.
    camera.cameraToWorld[12] = 0.0f;
    camera.cameraToWorld[13] = 0.0f;
    camera.cameraToWorld[14] = distance;
    camera.tanHalfFov = 0.5f;
    camera.aspect = 1.0f;
    return camera;
}

struct Pixel { float r, g, b; };

Pixel At(const std::vector<float>& image, float u, float v)
{
    const auto x = static_cast<std::uint32_t>(u * (kWidth - 1));
    const auto y = static_cast<std::uint32_t>(v * (kHeight - 1));
    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4;
    return {image[i], image[i + 1], image[i + 2]};
}

float Luminance(const Pixel& p)
{
    return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
}

/// Write a PPM so a failure can be looked at rather than only read about.
void SavePpm(const std::vector<float>& image, const std::string& name)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("hdclaude-" + name + ".ppm");
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << kWidth << " " << kHeight << "\n255\n";
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float linear = image[i * 4 + static_cast<std::size_t>(c)];
            // sRGB encode for viewing only; the renderer's output is linear.
            const float encoded =
                linear <= 0.0031308f
                    ? linear * 12.92f
                    : 1.055f * std::pow(std::max(linear, 0.0f), 1.0f / 2.4f) - 0.055f;
            const auto byte = static_cast<unsigned char>(
                std::min(255.0f, std::max(0.0f, encoded * 255.0f + 0.5f)));
            file.put(static_cast<char>(byte));
        }
    }
    std::printf("  wrote %s\n", path.string().c_str());
}

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("hdClaudeRenderTests\n");

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
        std::fprintf(stderr, "FAIL: validation layer unavailable\n");
        return 1;
    }

    const GlslCompiler compiler;
    try {
        VulkanAllocator allocator(*context);
        std::printf("  building path tracer (shaders: %s)\n", HDCLAUDE_SHADER_DIR);
        PathTracer tracer(*context, allocator, HDCLAUDE_SHADER_DIR);
        std::printf("  kernels compiled\n");
        mx::DocumentPtr libraries = LoadDefaultMaterialXLibraries();

        std::vector<CompiledMaterial> materials;
        materials.push_back(MakeDiffuseMaterial(libraries, compiler,
                                                tracer.ShadeKernelSource(),
                                                mx::Color3(0.8f, 0.8f, 0.8f),
                                                "white"));
        materials.push_back(MakeDiffuseMaterial(libraries, compiler,
                                                tracer.ShadeKernelSource(),
                                                mx::Color3(0.8f, 0.1f, 0.1f),
                                                "red"));
        for (const CompiledMaterial& material : materials) {
            CHECK(!material.spirv.empty());
        }
        if (materials[0].spirv.empty()) {
            return hdclaude_test::Summarize("hdClaudeRenderTests");
        }

        RenderSettings settings;
        settings.samplesPerPixel = 32;
        settings.maxBounces = 3;

        // --- A lit quad ------------------------------------------------------
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, materials);

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "lit-quad");

            const Pixel centre = At(image, 0.5f, 0.5f);
            const Pixel corner = At(image, 0.02f, 0.02f);

            std::printf("  quad centre  %.4f %.4f %.4f\n", centre.r, centre.g,
                        centre.b);
            std::printf("  background   %.4f %.4f %.4f\n", corner.r, corner.g,
                        corner.b);

            // The surface is lit, so it is brighter than the sky behind it.
            CHECK(Luminance(centre) > Luminance(corner));
            // And it is finite and positive: a NaN or a negative would survive
            // an eyeball check of a tone-mapped image.
            CHECK(std::isfinite(centre.r) && centre.r > 0.0f);
            CHECK(std::isfinite(centre.g) && centre.g > 0.0f);
            CHECK(std::isfinite(centre.b) && centre.b > 0.0f);
            // A grey material under a warm sun stays near-neutral.
            CHECK_NEAR(centre.r / centre.g, 1.0, 0.25);
        }

        // --- Material identity is per instance -------------------------------
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back(
                {0, Transform(0.8f, 0.8f, 1.0f, -1.0f, 0.0f, 0.0f), 0, true});
            scene.instances.push_back(
                {0, Transform(0.8f, 0.8f, 1.0f, 1.0f, 0.0f, 0.0f), 1, true});
            tracer.SetScene(scene, materials);

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(6.0f), settings);
            SavePpm(image, "two-materials");

            const Pixel left = At(image, 0.28f, 0.5f);
            const Pixel right = At(image, 0.72f, 0.5f);
            std::printf("  left (white) %.4f %.4f %.4f\n", left.r, left.g, left.b);
            std::printf("  right (red)  %.4f %.4f %.4f\n", right.r, right.g,
                        right.b);

            // Two instances of one prototype, two materials. Each shade
            // pipeline must claim only its own instances, so the red one is
            // red and the white one is not.
            CHECK(right.r > right.g * 2.0f);
            CHECK_NEAR(left.r / left.g, 1.0, 0.25);
        }

        // --- A cast shadow ---------------------------------------------------
        {
            // A large backdrop with a smaller quad in front of it, both facing
            // the camera, and the sun off to one side. Everything stays in the
            // camera's own plane because a translation-only matrix looks
            // straight down -Z; tilting would need a look-at, which the Hydra
            // camera adapter will provide and this test does not need.
            //
            // With the sun offset diagonally, the near quad's shadow lands on
            // the backdrop beside it rather than hidden behind it.
            Scene scene;
            scene.prototypes.push_back(MakeQuad());

            Transform3x4 backdrop = Transform(4.0f, 4.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            scene.instances.push_back({0, backdrop, 0, true});

            Transform3x4 blocker = Transform(0.7f, 0.7f, 1.0f, -0.6f, 0.6f, 1.5f);
            scene.instances.push_back({0, blocker, 0, true});

            tracer.SetScene(scene, materials);

            RenderSettings shadowSettings = settings;
            // Sun from up and to the left, so the shadow falls down and right
            // of the blocker, onto backdrop the camera can see.
            const float inv = 1.0f / std::sqrt(3.0f);
            shadowSettings.sunDirection[0] = -inv;
            shadowSettings.sunDirection[1] = inv;
            shadowSettings.sunDirection[2] = inv;
            shadowSettings.environmentColor[0] = 0.01f;
            shadowSettings.environmentColor[1] = 0.01f;
            shadowSettings.environmentColor[2] = 0.015f;

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(7.0f), shadowSettings);
            SavePpm(image, "shadow");

            // Search the backdrop for its darkest and brightest lit points
            // rather than sampling fixed coordinates. The assertion is that a
            // shadow exists and is substantially darker than lit backdrop --
            // not that it lands on a particular pixel, which depends on framing
            // rather than on transport.
            float brightest = 0.0f;
            float darkestLit = 1.0e9f;
            for (std::uint32_t y = kHeight / 2; y < kHeight; ++y) {
                for (std::uint32_t x = kWidth / 2; x < kWidth; ++x) {
                    const std::size_t i =
                        (static_cast<std::size_t>(y) * kWidth + x) * 4;
                    const Pixel p{image[i], image[i + 1], image[i + 2]};
                    const float lum = Luminance(p);
                    // Ignore the sky, which is darker than any lit surface.
                    if (lum < 0.02f) {
                        continue;
                    }
                    brightest = std::max(brightest, lum);
                    darkestLit = std::min(darkestLit, lum);
                }
            }
            std::printf("  backdrop lit %.4f, darkest surface %.4f\n", brightest,
                        darkestLit);

            CHECK(brightest > 0.1f);
            CHECK(darkestLit < brightest * 0.6f);
        }

        const std::uint64_t errors = context->ValidationErrorCount();
        if (errors != 0) {
            std::fprintf(stderr, "FAIL: %llu validation error(s). Last: %s\n",
                         static_cast<unsigned long long>(errors),
                         context->LastValidationError().c_str());
        }
        CHECK_EQ(errors, std::uint64_t(0));
    } catch (const std::exception& error) {
        // Reported rather than left to terminate: a kernel that fails to
        // compile should name itself, not abort with a status code.
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }

    return hdclaude_test::Summarize("hdClaudeRenderTests");
}
