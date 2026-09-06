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

#include <algorithm>
#include <chrono>
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

/// Octaves of fractal noise in the deliberately expensive material used to
/// measure per-material dispatch. Large enough to dominate a shading dispatch,
/// small enough that generating and compiling it stays quick.
constexpr int kHeavyOctaves = 32;

/// Resolution of the timed scene. Larger than the assertion scenes because a
/// measurement of shading has to be big enough that shading is what it
/// measures.
constexpr std::uint32_t kTimingSize = 512;

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

/// Generate and compile the one renderable element of `doc`.
CompiledMaterial CompileMaterial(mx::DocumentPtr doc, const GlslCompiler& compiler,
                                 const std::string& shadeKernel,
                                 const std::string& name)
{
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

/// Generate a diffuse material of the given colour and compile it with the
/// shade kernel into a shading pipeline.
///
/// `noiseOctaves` inflates the pattern graph in front of the colour without
/// changing what the material looks like from a distance: each octave is a
/// 3D fractal noise mixed towards the base colour by a vanishing weight. It is
/// how the scaling claim behind the per-material dispatch is measured -- a big
/// graph on one object must cost that object and not the frame.
CompiledMaterial MakeDiffuseMaterial(mx::DocumentPtr libraries,
                                     const GlslCompiler& compiler,
                                     const std::string& shadeKernel,
                                     const mx::Color3& colour,
                                     const std::string& name,
                                     int noiseOctaves = 0)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr tint;
    for (int octave = 0; octave < noiseOctaves; ++octave) {
        const std::string suffix = std::to_string(octave);
        mx::NodePtr noise = AddNode(doc, "fractal3d", "n" + suffix, "color3");
        SetValue(noise, "amplitude", mx::Vector3(1.0f, 1.0f, 1.0f));
        SetValue(noise, "octaves", 4);
        SetValue(noise, "lacunarity", 2.0f + 0.01f * float(octave));

        mx::NodePtr mixer = AddNode(doc, "mix", "x" + suffix, "color3");
        Connect(mixer, "fg", noise);
        if (tint) {
            Connect(mixer, "bg", tint);
        } else {
            SetValue(mixer, "bg", colour);
        }
        // Small enough that the graph cannot change the image, large enough
        // that no generator is entitled to fold it away.
        SetValue(mixer, "mix", 1.0e-6f);
        tint = mixer;
    }

    mx::NodePtr bsdf = AddNode(doc, "oren_nayar_diffuse_bsdf", "d", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "color", colour);
    SetValue(bsdf, "roughness", 0.0f);
    if (tint) {
        Connect(bsdf, "color", tint);
    }

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// A material whose colour *is* its texture coordinate.
///
/// Every image node reads through `texcoord`, so this asks the one question
/// that matters about UVs without needing an image on disk: does the value the
/// kernel interpolated reach the material at all, and the right way round.
CompiledMaterial MakeTexcoordMaterial(mx::DocumentPtr libraries,
                                      const GlslCompiler& compiler,
                                      const std::string& shadeKernel,
                                      const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr uv = AddNode(doc, "texcoord", "uv", "vector2");

    // The nodedef is named rather than inferred: `convert` is overloaded on its
    // input type, and a node created before its input is connected resolves to
    // the float overload and then fails to generate.
    mx::NodePtr colour = doc->addNode("convert", "c", "color3");
    colour->setNodeDefString("ND_convert_vector2_color3");
    if (mx::InputPtr in = colour->addInput("in", "vector2")) {
        in->setConnectedNode(uv);
    }

    mx::NodePtr bsdf = AddNode(doc, "oren_nayar_diffuse_bsdf", "d", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "roughness", 0.0f);
    Connect(bsdf, "color", colour);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// A material whose albedo is a sampled image.
///
/// The file name is never opened: the test publishes the decoded image straight
/// into the scene's texture pool and points the material's one slot at it, so
/// this asks about the renderer's sampling convention without involving an
/// image decoder or a file on disk.
CompiledMaterial MakeImageMaterial(mx::DocumentPtr libraries,
                                   const GlslCompiler& compiler,
                                   const std::string& shadeKernel,
                                   const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr uv = AddNode(doc, "texcoord", "uv", "vector2");

    mx::NodePtr image = doc->addNode("image", "orientation", "color3");
    image->setNodeDefString("ND_image_color3");
    if (mx::InputPtr file = image->addInput("file", "filename")) {
        file->setValueString("orientation.png");
    }
    if (mx::InputPtr texcoord = image->addInput("texcoord", "vector2")) {
        texcoord->setConnectedNode(uv);
    }

    mx::NodePtr bsdf = AddNode(doc, "oren_nayar_diffuse_bsdf", "d", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "roughness", 0.0f);
    Connect(bsdf, "color", image);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    CompiledMaterial compiled = CompileMaterial(doc, compiler, shadeKernel, name);
    // One image, at pool slot 0.
    compiled.textureSlots = {0};
    return compiled;
}

/// An image in four solid quadrants, in the renderer's own row order: row 0 is
/// v = 0.
///
///     v = 1   blue    white
///     v = 0   red     green
///             u = 0   u = 1
///
/// Sized well above 2x2 on purpose. Four texels would be filtered into one
/// smooth gradient by the linear sampler and no point on the surface would
/// carry a single quadrant's colour; solid blocks make the middle of each
/// quadrant exact.
TextureImage MakeOrientationTexture()
{
    constexpr std::uint32_t kSize = 64;
    TextureImage image;
    image.width = kSize;
    image.height = kSize;
    image.debugName = "orientation";
    image.rgba.resize(static_cast<std::size_t>(kSize) * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool right = x >= kSize / 2;
            const bool top = y >= kSize / 2;
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            image.rgba[i + 0] = (!top && !right) || (top && right) ? 255 : 0;
            image.rgba[i + 1] = (!top && right) || (top && right) ? 255 : 0;
            image.rgba[i + 2] = (top && !right) || (top && right) ? 255 : 0;
            image.rgba[i + 3] = 255;
        }
    }
    return image;
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
    // PPM stores its top row first and the renderer's row 0 is the bottom, so
    // the rows go out in reverse. Without this the debug images are upside
    // down while the renderer is right, which is a confusing way to hunt a bug.
    for (std::size_t row = 0; row < kHeight; ++row) {
      const std::size_t y = kHeight - 1 - row;
      for (std::size_t x = 0; x < kWidth; ++x) {
        const std::size_t i = y * kWidth + x;
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

        // --- The material sort covers each material's paths and no others ----
        //
        // The claim the per-material dispatch rests on: a shading pipeline is
        // dispatched over the paths that hit *its* material, not over the
        // frame. That is invisible in an image -- dispatching every pipeline
        // over every path and discarding the misfits produces the same picture
        // at several times the cost -- so it is asserted against the counts the
        // sort wrote, which is also the only place those GPU-written counters
        // are ever read.
        //
        // One sample and one bounce, so the counts left on the device describe
        // the camera rays and nothing else.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back(
                {0, Transform(0.8f, 0.8f, 1.0f, -1.0f, 0.0f, 0.0f), 0, true});
            scene.instances.push_back(
                {0, Transform(0.8f, 0.8f, 1.0f, 1.0f, 0.0f, 0.0f), 1, true});
            tracer.SetScene(scene, materials);

            RenderSettings single;
            single.samplesPerPixel = 1;
            single.maxBounces = 1;
            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(6.0f), single);

            const std::vector<std::uint32_t> counts = tracer.MaterialCounts();
            CHECK_EQ(counts.size(), std::size_t(2));
            if (counts.size() == 2) {
                // Every pixel that is not the untouched background hit one of
                // the two quads, so the groups must account for exactly those
                // and no more. A background pixel carries the environment
                // constant unchanged, which no lit surface in this scene
                // produces.
                std::size_t hits = 0;
                for (std::size_t i = 0; i < kWidth * kHeight; ++i) {
                    const Pixel pixel{image[i * 4], image[i * 4 + 1],
                                      image[i * 4 + 2]};
                    if (std::abs(pixel.r - single.environmentColor[0]) > 1e-5f ||
                        std::abs(pixel.g - single.environmentColor[1]) > 1e-5f ||
                        std::abs(pixel.b - single.environmentColor[2]) > 1e-5f) {
                        ++hits;
                    }
                }
                std::printf("  sorted %u + %u paths, %zu shaded pixels\n",
                            counts[0], counts[1], hits);

                CHECK_EQ(std::size_t(counts[0]) + counts[1], hits);
                CHECK(counts[0] > 0);
                CHECK(counts[1] > 0);
                // Two congruent quads placed symmetrically, so neither group
                // may have swallowed the other's paths.
                CHECK_NEAR(double(counts[0]) / double(counts[1]), 1.0, 0.05);
            }
        }

        // --- Per-material dispatch scales with the object, not the frame -----
        //
        // The phase 5 gate. A large pattern graph on one object must change
        // that object's shading cost and not the frame's, which is the whole
        // reason the integrator is wavefront. The same two-quad scene is timed
        // three ways: neither quad heavy, one heavy, both heavy. If shading is
        // per material, the one-heavy frame costs about half of what the
        // both-heavy frame adds; if every pipeline ran over every path it would
        // cost nearly all of it.
        {
            const CompiledMaterial heavyWhite = MakeDiffuseMaterial(
                libraries, compiler, tracer.ShadeKernelSource(),
                mx::Color3(0.8f, 0.8f, 0.8f), "white_heavy", kHeavyOctaves);
            const CompiledMaterial heavyRed = MakeDiffuseMaterial(
                libraries, compiler, tracer.ShadeKernelSource(),
                mx::Color3(0.8f, 0.1f, 0.1f), "red_heavy", kHeavyOctaves);
            CHECK(!heavyWhite.spirv.empty());
            CHECK(!heavyRed.spirv.empty());

            // Two quads filling the frame between them, at a resolution and a
            // sample count chosen so that shading dominates. At the 128-pixel
            // size the other assertions use, a frame is almost entirely queue
            // submission and the shading difference disappears into it -- which
            // is a statement about how small those scenes are, not about the
            // dispatch.
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back(
                {0, Transform(1.0f, 2.0f, 1.0f, -1.0f, 0.0f, 0.0f), 0, true});
            scene.instances.push_back(
                {0, Transform(1.0f, 2.0f, 1.0f, 1.0f, 0.0f, 0.0f), 1, true});

            RenderSettings timed;
            timed.samplesPerPixel = 8;
            timed.maxBounces = 1;

            // The fastest of a few runs, after a warm-up frame: a slow one
            // measures the machine, a fast one measures the renderer. The
            // warm-up matters more than it looks -- the driver finishes
            // compiling a pipeline the first time it is dispatched, and a
            // 32-octave program takes tens of milliseconds to do that, which
            // is enough to swamp the shading this is measuring.
            auto timeScene = [&](const std::vector<CompiledMaterial>& set) {
                tracer.SetScene(scene, set);
                tracer.Render(kTimingSize, kTimingSize, LookDownZ(2.2f), timed);

                double best = 1.0e30;
                for (int run = 0; run < 3; ++run) {
                    const auto start = std::chrono::steady_clock::now();
                    tracer.Render(kTimingSize, kTimingSize, LookDownZ(2.2f), timed);
                    const std::chrono::duration<double, std::milli> elapsed =
                        std::chrono::steady_clock::now() - start;
                    best = std::min(best, elapsed.count());
                }
                return best;
            };

            const double neither = timeScene({materials[0], materials[1]});
            const double one = timeScene({materials[0], heavyRed});
            const double both = timeScene({heavyWhite, heavyRed});

            std::printf("  shading %d-octave graph: none %.1f ms, one %.1f ms, "
                        "both %.1f ms\n",
                        kHeavyOctaves, neither, one, both);

            // Both quads carry the same number of paths, so one heavy material
            // should account for about half the added cost. The bound is loose
            // because this is a wall clock on a boosting GPU; what it has to
            // separate is half from all, and the pre-sort renderer sat at all.
            const double added = both - neither;
            CHECK(added > 0.0);
            if (added > 0.0) {
                const double share = (one - neither) / added;
                std::printf("  one heavy material costs %.2f of both\n", share);
                CHECK(share < 0.75);
            }
        }

        // --- Texture coordinates reach the material --------------------------
        //
        // A material whose albedo is its own UV, on a quad with authored UVs.
        // The image must brighten in red from left to right and in green from
        // bottom to top, which is what says the kernel's interpolated
        // coordinate arrived, in the right channel and the right orientation.
        //
        // Every image node in MaterialX reads through `texcoord`, so this is
        // the assertion standing behind every textured material.
        {
            const CompiledMaterial uvMaterial = MakeTexcoordMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), "texcoord");
            CHECK(!uvMaterial.spirv.empty());

            MeshPrototype quad = MakeQuad();
            quad.uvs = {0, 0, 1, 0, 1, 1, 0, 1};

            Scene scene;
            scene.prototypes.push_back(quad);
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, {uvMaterial});

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "texcoord");

            const Pixel left = At(image, 0.3f, 0.5f);
            const Pixel right = At(image, 0.7f, 0.5f);
            const Pixel bottom = At(image, 0.5f, 0.3f);
            const Pixel top = At(image, 0.5f, 0.7f);
            std::printf("  uv left %.4f right %.4f, bottom %.4f top %.4f\n",
                        left.r, right.r, bottom.g, top.g);

            CHECK(right.r > left.r * 1.5f);
            CHECK(top.g > bottom.g * 1.5f);
        }

        // --- A texture arrives the way round it was decoded -------------------
        //
        // The quad's UVs put v = 0 at the bottom, and the texture's first row
        // is v = 0, so each texel must land in the corner that names it. This
        // is the assertion that was missing while every texture in the gallery
        // was uploaded upside down: a noise or a gradient map looks equally
        // plausible flipped, and it took a backdrop with printed numbers on it
        // for anyone to notice.
        {
            const CompiledMaterial imageMaterial = MakeImageMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), "orientation");
            CHECK(!imageMaterial.spirv.empty());

            MeshPrototype quad = MakeQuad();
            quad.uvs = {0, 0, 1, 0, 1, 1, 0, 1};

            Scene scene;
            scene.prototypes.push_back(quad);
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.textures.push_back(MakeOrientationTexture());
            tracer.SetScene(scene, {imageMaterial});

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "texture-orientation");

            // At the texel centres, where bilinear filtering returns one texel
            // exactly. The quad covers the middle half of the frame, so the
            // texel centres at uv 0.25 and 0.75 are at 0.375 and 0.625 of the
            // image; sampling further out blends across the wrap seam and the
            // corners stop being one colour each.
            const Pixel bottomLeft = At(image, 0.375f, 0.375f);
            const Pixel bottomRight = At(image, 0.625f, 0.375f);
            const Pixel topLeft = At(image, 0.375f, 0.625f);
            const Pixel topRight = At(image, 0.625f, 0.625f);
            std::printf("  texture corners: bl %.2f %.2f %.2f, br %.2f %.2f %.2f, "
                        "tl %.2f %.2f %.2f\n",
                        bottomLeft.r, bottomLeft.g, bottomLeft.b, bottomRight.r,
                        bottomRight.g, bottomRight.b, topLeft.r, topLeft.g,
                        topLeft.b, topRight.r, topRight.g, topRight.b);

            // Each corner is dominated by its own channel. Absolute values
            // depend on the lighting; which channel wins does not.
            CHECK(bottomLeft.r > bottomLeft.g && bottomLeft.r > bottomLeft.b);
            CHECK(bottomRight.g > bottomRight.r && bottomRight.g > bottomRight.b);
            CHECK(topLeft.b > topLeft.r && topLeft.b > topLeft.g);
            // White at the fourth corner: no channel dominates.
            CHECK_NEAR(topRight.r / std::max(topRight.g, 1.0e-6f), 1.0, 0.2);
        }

        // --- An instance transform is the same as baking it ------------------
        //
        // The same surface in the same place in the world, expressed two ways:
        // a tilted quad reached through an instance transform, and a quad whose
        // vertices and normals were tilted on the host and instanced with the
        // identity. Both describe identical world geometry, so they must shade
        // identically.
        //
        // This is what catches a transform used where its transpose belongs.
        // A wrong basis still produces a picture -- the surface is in the right
        // place, because positions come from the acceleration structure -- and
        // only the shading is off, by an amount that looks like a lighting
        // choice until it is compared against the same surface built the other
        // way.
        {
            const float angle = 0.6f;   // radians about Y
            const float c = std::cos(angle);
            const float s = std::sin(angle);

            Transform3x4 rotation;
            rotation.m[0] = c;  rotation.m[2] = s;
            rotation.m[8] = -s; rotation.m[10] = c;

            MeshPrototype baked = MakeQuad();
            for (std::size_t i = 0; i < baked.positions.size(); i += 3) {
                const float x = baked.positions[i];
                const float z = baked.positions[i + 2];
                baked.positions[i] = c * x + s * z;
                baked.positions[i + 2] = -s * x + c * z;

                const float nx = baked.normals[i];
                const float nz = baked.normals[i + 2];
                baked.normals[i] = c * nx + s * nz;
                baked.normals[i + 2] = -s * nx + c * nz;
            }

            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.prototypes.push_back(baked);
            scene.instances.push_back({0, rotation, 0, true});
            tracer.SetScene(scene, materials);
            const std::vector<float> transformed =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);

            Scene bakedScene;
            bakedScene.prototypes.push_back(MakeQuad());
            bakedScene.prototypes.push_back(baked);
            bakedScene.instances.push_back({1, Transform3x4{}, 0, true});
            tracer.SetScene(bakedScene, materials);
            const std::vector<float> onHost =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);

            SavePpm(transformed, "tilted-by-transform");
            SavePpm(onHost, "tilted-on-host");

            const Pixel byTransform = At(transformed, 0.5f, 0.5f);
            const Pixel byHost = At(onHost, 0.5f, 0.5f);
            std::printf("  tilted by transform %.4f %.4f %.4f, on host "
                        "%.4f %.4f %.4f\n",
                        byTransform.r, byTransform.g, byTransform.b, byHost.r,
                        byHost.g, byHost.b);

            CHECK(Luminance(byHost) > 0.0f);
            CHECK_NEAR(Luminance(byTransform), Luminance(byHost), 0.02);
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
            for (std::uint32_t y = 0; y < kHeight; ++y) {
                for (std::uint32_t x = 0; x < kWidth; ++x) {
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
