// End-to-end proof of the genglsl_pt target: build a MaterialX material,
// generate GLSL for the path-tracing target, and compile it to SPIR-V.
//
// No GPU and no OpenUSD stage are required -- only the MaterialX libraries from
// the OpenUSD distribution. This is the test that says whether the shading
// architecture in docs/materialx-codegen.md actually works.

#include "test_support.h"

#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace mx = MaterialX;
using namespace hdclaude;

namespace {

const char* kStdlibDir = HDCLAUDE_MATERIALX_STDLIB_DIR;
const char* kHdClaudeMtlxDir = HDCLAUDE_MTLX_LIBRARY_DIR;

/// Load the stock MaterialX libraries plus hdClaude's genglsl_pt target.
///
/// Both trees are loaded into one document. The targetdef's `inherit="genglsl"`
/// is what makes every node hdClaude does not override resolve to the stock
/// implementation.
mx::DocumentPtr LoadLibraries() { return LoadDefaultMaterialXLibraries(); }

mx::FileSearchPath SourceSearchPath()
{
    return DefaultMaterialXSourceSearchPath();
}

/// Add a node, reporting rather than crashing when its nodedef does not
/// resolve. An unresolved nodedef makes every later addInputFromNodeDef return
/// null, so without this the first symptom is an access violation with no
/// indication of which node was at fault.
mx::NodePtr AddNode(mx::DocumentPtr doc, const std::string& category,
                    const std::string& name, const std::string& type)
{
    mx::NodePtr node = doc->addNode(category, name, type);
    if (!node || !node->getNodeDef()) {
        std::fprintf(stderr, "  no nodedef resolved for '%s' (type %s)\n",
                     category.c_str(), type.c_str());
        return nullptr;
    }
    return node;
}

/// Add an input from the node's nodedef, reporting a missing one by name.
mx::InputPtr AddInput(mx::NodePtr node, const std::string& input)
{
    if (!node) {
        return nullptr;
    }
    mx::InputPtr port = node->addInputFromNodeDef(input);
    if (!port) {
        std::fprintf(stderr, "  node '%s' has no input '%s'\n",
                     node->getCategory().c_str(), input.c_str());
    }
    return port;
}

template <typename T>
void SetValue(mx::NodePtr node, const std::string& input, const T& value)
{
    if (mx::InputPtr port = AddInput(node, input)) {
        port->setValue(value);
    }
}

void Connect(mx::NodePtr node, const std::string& input, mx::NodePtr source)
{
    if (mx::InputPtr port = AddInput(node, input); port && source) {
        port->setConnectedNode(source);
    }
}

mx::DocumentPtr BuildOverriddenClosureMaterial(mx::DocumentPtr libraries)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr diffuse = AddNode(doc, "oren_nayar_diffuse_bsdf", "ptDiffuse", "BSDF");
    SetValue(diffuse, "weight", 1.0f);
    SetValue(diffuse, "color", mx::Color3(0.8f, 0.6f, 0.4f));
    SetValue(diffuse, "roughness", 0.3f);

    mx::NodePtr metal = AddNode(doc, "conductor_bsdf", "ptMetal", "BSDF");
    SetValue(metal, "weight", 1.0f);
    SetValue(metal, "roughness", mx::Vector2(0.2f, 0.2f));

    // mix: the combinator whose density rule the whole protocol exists for.
    mx::NodePtr mixed = AddNode(doc, "mix", "ptMix", "BSDF");
    Connect(mixed, "fg", metal);
    Connect(mixed, "bg", diffuse);
    SetValue(mixed, "mix", 0.35f);

    // multiply: scales the response, leaves the density alone.
    mx::NodePtr tinted = AddNode(doc, "multiply", "ptTint", "BSDF");
    Connect(tinted, "in1", mixed);
    SetValue(tinted, "in2", mx::Color3(0.9f, 0.9f, 1.0f));

    // layer: selection probability comes from the top layer's throughput.
    mx::NodePtr coat = AddNode(doc, "conductor_bsdf", "ptCoat", "BSDF");
    SetValue(coat, "weight", 0.5f);
    SetValue(coat, "roughness", mx::Vector2(0.05f, 0.05f));

    mx::NodePtr layered = AddNode(doc, "layer", "ptLayer", "BSDF");
    Connect(layered, "top", coat);
    Connect(layered, "base", tinted);

    mx::NodePtr surface = AddNode(doc, "surface", "ptSurface", "surfaceshader");
    Connect(surface, "bsdf", layered);
    SetValue(surface, "opacity", 1.0f);

    mx::NodePtr material = AddNode(doc, "surfacematerial", "ptMaterial", "material");
    Connect(material, "surfaceshader", surface);

    return doc;
}

struct Generated {
    bool ok = false;
    std::string source;
    std::string error;
};

Generated GenerateMaterial(mx::DocumentPtr doc, const std::string& name)
{
    Generated result;
    try {
        mx::ShaderGeneratorPtr generator = PathTracerShaderGenerator::create();
        mx::GenContext context(generator);
        context.registerSourceCodeSearchPath(SourceSearchPath());

        // No rasteriser lighting. This is not a tuning knob: with light sources
        // enabled, MaterialX emits light node implementations that construct
        // ClosureData themselves, and a path tracer supplies its own light
        // sample. Zero is the only correct value for this target.
        context.getOptions().hwMaxActiveLightSources = 0;

        std::vector<mx::TypedElementPtr> renderable;
        mx::findRenderableElements(doc, renderable);
        if (renderable.empty()) {
            result.error = "no renderable element in the document";
            return result;
        }

        mx::ShaderPtr shader = generator->generate(name, renderable.front(), context);
        if (!shader) {
            result.error = "generator returned no shader";
            return result;
        }
        result.source = shader->getSourceCode(mx::Stage::PIXEL);
        result.ok = !result.source.empty();
        if (!result.ok) {
            result.error = "generated source is empty";
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

void SaveForInspection(const std::string& name, const std::string& source)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("hdclaude-" + name + ".glsl");
    std::ofstream file(path);
    file << source;
    std::printf("  generated source written to %s\n", path.string().c_str());
}

void TestTargetIsRegistered()
{
    mx::ShaderGeneratorPtr generator = PathTracerShaderGenerator::create();
    CHECK_EQ(generator->getTarget(), std::string("genglsl_pt"));
}

void TestOverriddenClosuresGenerateAndCompile(const GlslCompiler& compiler)
{
    std::printf("  loading libraries...\n");
    mx::DocumentPtr libraries = LoadLibraries();
    std::printf("  loaded %zu nodedefs\n", libraries->getNodeDefs().size());
    CHECK(libraries->getNodeDefs().size() > 0);

    // The targetdef must have loaded, or every implementation lookup silently
    // falls back to genglsl and the overrides do nothing.
    CHECK(libraries->getChild("genglsl_pt") != nullptr);

    std::printf("  building material...\n");
    mx::DocumentPtr doc = BuildOverriddenClosureMaterial(libraries);
    std::printf("  generating...\n");
    const Generated generated = GenerateMaterial(doc, "hdclaude_pt_closures");

    if (!generated.ok) {
        std::fprintf(stderr, "  generation failed: %s\n", generated.error.c_str());
    }
    CHECK(generated.ok);
    if (!generated.ok) {
        return;
    }

    SaveForInspection("pt_closures", generated.source);

    // The generated code must show the target actually took effect.
    CHECK(generated.source.find("CLOSURE_TYPE_PT_SAMPLE") != std::string::npos);
    CHECK(generated.source.find("hdclaude_material_shade") != std::string::npos);
    CHECK(generated.source.find("hdclaude_bsdf") != std::string::npos);
    CHECK(generated.source.find("float pdf") != std::string::npos);
    CHECK(generated.source.find("sampledL") != std::string::npos);

    // The surface node override must have been used. This is asserted
    // positively, by its own marker, because the failure mode is *silent*: an
    // implementation declaration naming a nodedef that does not resolve makes
    // MaterialX fall back to the stock SurfaceNodeGlsl through target
    // inheritance, and the first version of this test passed while generating
    // a rasteriser light loop.
    CHECK(generated.source.find("hdClaude: evaluate the BSDF against the "
                                "caller's closure data") != std::string::npos);

    // And negatively, by the rasteriser constructs the stock node emits.
    CHECK(generated.source.find("u_lightData") == std::string::npos);
    CHECK(generated.source.find("u_viewPosition") == std::string::npos);
    CHECK(generated.source.find("mx_environment_radiance") == std::string::npos);
    CHECK(generated.source.find("Add environment contribution") == std::string::npos);

    // The caller's closureData must not be shadowed by a locally constructed
    // one: the whole point of the override is that the integrator chooses the
    // closure type, the direction, and the light sample.
    CHECK(generated.source.find("ClosureData closureData = ClosureData(") ==
          std::string::npos);

    // And it must compile -- as the `shade` kernel will actually use it.
    //
    // A generated material is a library, not a standalone shader: it defines
    // the ABI globals and `hdclaude_material_shade`, and the kernel supplies
    // the workgroup size, fills the SurfaceHit, and calls in. Compiling it with
    // that harness is what proves the ABI is callable, rather than only that
    // the text parses.
    const std::string kernel = generated.source + R"(
// --- test harness standing in for the wavefront shade kernel ----------------
layout(local_size_x = 64) in;

void main()
{
    // Geometry the kernel would interpolate from the hit record.
    vd.normalWorld = vec3(0.0, 0.0, 1.0);
    vd.positionWorld = vec3(0.0);
    vd.tangentWorld = vec3(1.0, 0.0, 0.0);

    // Per-invocation path state the closures read.
    hdclaude_wavelengths = vec4(450.0, 550.0, 600.0, 650.0);
    hdclaude_sample_u = vec3(0.31, 0.62, 0.47);

    vec3 V = normalize(vec3(0.0, 0.4, 1.0));
    vec3 N = vd.normalWorld;
    vec3 P = vd.positionWorld;

    // Pass 1: choose a direction.
    ClosureData sampleData =
        ClosureData(CLOSURE_TYPE_PT_SAMPLE, vec3(0.0), V, N, P, 1.0);
    hdclaude_material_shade(sampleData);
    vec3 L = hdclaude_bsdf.sampledL;

    // Pass 2: evaluate f and pdf at that direction.
    ClosureData evalData =
        ClosureData(CLOSURE_TYPE_REFLECTION, L, V, N, P, 1.0);
    hdclaude_material_shade(evalData);

    // Consume every ABI output so nothing is optimised away.
    float keep = hdclaude_bsdf.response.x + hdclaude_bsdf.pdf +
                 hdclaude_bsdf.isDelta + hdclaude_bsdf.guideRoughness +
                 hdclaude_bsdf.guideAlbedo.x + hdclaude_emission.x +
                 hdclaude_opacity + L.x;
    if (keep < -1.0e30)
    {
        hdclaude_opacity = keep;
    }
}
)";

    GlslCompileOptions options;
    options.moduleName = "hdclaude_pt_closures";
    const GlslCompileResult compiled = compiler.Compile(kernel, options);
    if (!compiled.ok) {
        std::fprintf(stderr, "  SPIR-V compilation failed:\n%s\n",
                     compiled.log.c_str());
    }
    CHECK(compiled.ok);
    if (compiled.ok) {
        std::printf("  compiled to %zu SPIR-V words\n", compiled.spirv.size());
    }
}

}  // namespace

int main()
{
    // Unbuffered: a crash inside MaterialX would otherwise discard every
    // progress line and leave nothing to locate it with.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("hdClaudeMaterialXTests\n");
    std::printf("  stdlib ......... %s\n", kStdlibDir);
    std::printf("  hdClaude mtlx .. %s\n", kHdClaudeMtlxDir);

    const GlslCompiler compiler;

    TestTargetIsRegistered();
    TestOverriddenClosuresGenerateAndCompile(compiler);

    return hdclaude_test::Summarize("hdClaudeMaterialXTests");
}
