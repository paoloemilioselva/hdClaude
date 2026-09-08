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
#include "hdclaude/core/spectrum.h"
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
#include <cstdlib>
#include <cstring>
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
CompiledMaterial CompileMaterial(
    mx::DocumentPtr doc, const GlslCompiler& compiler,
    const std::string& shadeKernel, const std::string& name,
    const std::map<std::string, std::vector<int>>& udimTiles = {})
{
    mx::ShaderGeneratorPtr generator = PathTracerShaderGenerator::create();
    // Which tiles a UDIM set has is the caller's to know -- in the delegate it
    // comes from the asset resolver -- and it has to be told before generation,
    // because it decides how many array slots the set takes.
    if (auto* pt = dynamic_cast<PathTracerShaderGenerator*>(generator.get())) {
        pt->SetUdimTiles(udimTiles);
    }
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

    // HDCLAUDE_DUMP_SHADERS=<dir> writes what was generated, the same facility
    // the Hydra compiler has. Generated code is the one artefact in this
    // pipeline nobody ever reads unless it fails to compile, and a material
    // that compiles cleanly and renders wrongly is exactly the case where it is
    // the only place the answer can be.
    if (const char* dumpDir = std::getenv("HDCLAUDE_DUMP_SHADERS")) {
        std::error_code code;
        std::filesystem::create_directories(dumpDir, code);
        std::ofstream out(std::filesystem::path(dumpDir) / (name + ".comp.glsl"));
        out << shader->getSourceCode(mx::Stage::PIXEL);
    }

    GlslCompileOptions options;
    options.moduleName = name;
    const GlslCompileResult compiled = compiler.Compile(source, options);
    if (!compiled.ok) {
        std::fprintf(stderr, "  material %s failed:\n%s\n", name.c_str(),
                     compiled.log.c_str());
    }
    CompiledMaterial material{compiled.spirv, name};
    // The same read the Hydra material compiler performs, for the same reason:
    // MaterialX drops dispersion during generation, so it has to be taken from
    // the document and carried beside the program. Done here rather than in
    // each test that needs it, so a test authoring the OpenPBR inputs exercises
    // the whole chain rather than a value set by hand.
    material.dispersionAbbe = AuthoredDispersion(doc);
    return material;
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

/// A transmissive dielectric with an absorbing interior, for Beer-Lambert.
///
/// `layer(top: dielectric_bsdf, base: anisotropic_vdf)` is how MaterialX says
/// "this surface encloses a medium". The volume node publishes a coefficient and
/// evaluates to nothing at the surface itself, because absorption happens along
/// the flight *between* surfaces and there is no distance to integrate over at a
/// point.
/// The same quad wound the other way, so a pair of them encloses a volume.
MeshPrototype MakeQuadFacingBack()
{
    MeshPrototype prototype;
    prototype.debugName = "quad.back";
    prototype.positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    prototype.indices = {0, 2, 1, 0, 3, 2};
    prototype.normals = {0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1};
    return prototype;
}

/// `layer(dielectric reflection, dielectric transmission)` by hand.
///
/// Exactly what `open_pbr_surface` builds around its specular lobe, and nothing
/// else. Standing between the bare closure and the whole OpenPBR graph, it says
/// which of the two owns an energy error the furnace finds.
CompiledMaterial MakeLayeredDielectric(mx::DocumentPtr libraries,
                                       const GlslCompiler& compiler,
                                       const std::string& shadeKernel,
                                       float ior,
                                       const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr reflection = AddNode(doc, "dielectric_bsdf", "dr", "BSDF");
    SetValue(reflection, "weight", 1.0f);
    SetValue(reflection, "ior", ior);
    SetValue(reflection, "roughness", mx::Vector2(0.0f, 0.0f));
    reflection->setInputValue("scatter_mode", std::string("R"), "string");

    mx::NodePtr transmission = AddNode(doc, "dielectric_bsdf", "dt", "BSDF");
    SetValue(transmission, "weight", 1.0f);
    SetValue(transmission, "ior", ior);
    SetValue(transmission, "roughness", mx::Vector2(0.0f, 0.0f));
    transmission->setInputValue("scatter_mode", std::string("T"), "string");

    mx::NodePtr layered = AddNode(doc, "layer", "ly", "BSDF");
    Connect(layered, "top", reflection);
    Connect(layered, "base", transmission);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", layered);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// Instantiate a *named* nodedef, rather than letting the category pick one.
///
/// `layer` is two nodedefs with the same category and the same output type:
/// `ND_layer_bsdf`, whose base is a BSDF, and `ND_layer_vdf`, whose base is a
/// VDF. Adding one by category resolves to the first, and connecting a VDF to
/// a base input declared BSDF then produces a graph that generates, compiles,
/// and silently layers the surface over a null closure -- a slab enclosing a
/// vacuum read 0.8095 against one before this existed. The name is the only
/// way to say which of the two is meant.
mx::NodePtr AddNodeOfDef(mx::DocumentPtr doc, const std::string& nodeDef,
                         const std::string& name)
{
    mx::NodeDefPtr definition = doc->getNodeDef(nodeDef);
    if (!definition) {
        std::fprintf(stderr, "  no nodedef '%s'\n", nodeDef.c_str());
        return nullptr;
    }
    return doc->addNodeInstance(definition, name);
}

/// A slab whose interior scatters, with the coefficients authored directly.
///
/// The structure is the one `open_pbr_surface` generates for a transmissive
/// material, built by hand: a reflection lobe layered over a transmission lobe
/// that in turn carries the volume, which is
/// `layer(R, layer_vdf(T, anisotropic_vdf))`. Going through it by hand rather
/// than through OpenPBR is the point -- OpenPBR derives its coefficients from a
/// colour and a depth, so a test written against it cannot state the medium it
/// is testing, and an earlier attempt at spectral scattering was reverted
/// against a measurement taken that way.
///
/// Neither the interface nor the layering is the thing under test. The same
/// graph without the volume is `MakeLayeredDielectric`, gated on its own at
/// 0.9953, so whatever this slab reads that the layered dielectric does not
/// belongs to the interior.
CompiledMaterial MakeScatteringMedium(mx::DocumentPtr libraries,
                                      const GlslCompiler& compiler,
                                      const std::string& shadeKernel,
                                      const mx::Vector3& absorption,
                                      const mx::Vector3& scattering,
                                      float anisotropy,
                                      const std::string& name,
                                      float ior = 1.5f)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr reflection = AddNode(doc, "dielectric_bsdf", "dr", "BSDF");
    SetValue(reflection, "weight", 1.0f);
    SetValue(reflection, "ior", ior);
    SetValue(reflection, "roughness", mx::Vector2(0.0f, 0.0f));
    reflection->setInputValue("scatter_mode", std::string("R"), "string");

    mx::NodePtr transmission = AddNode(doc, "dielectric_bsdf", "dt", "BSDF");
    SetValue(transmission, "weight", 1.0f);
    SetValue(transmission, "ior", ior);
    SetValue(transmission, "roughness", mx::Vector2(0.0f, 0.0f));
    transmission->setInputValue("scatter_mode", std::string("T"), "string");

    mx::NodePtr volume = AddNode(doc, "anisotropic_vdf", "vd", "VDF");
    // vector3, not color3: these are coefficients per unit distance and the
    // nodedef says so. Authoring them as a colour would put a medium's
    // parameters through a colour pipeline they are not colours in.
    SetValue(volume, "absorption", absorption);
    SetValue(volume, "scattering", scattering);
    SetValue(volume, "anisotropy", anisotropy);

    mx::NodePtr interior = AddNodeOfDef(doc, "ND_layer_vdf", "lv");
    Connect(interior, "top", transmission);
    Connect(interior, "base", volume);

    mx::NodePtr layered = AddNodeOfDef(doc, "ND_layer_bsdf", "ly");
    Connect(layered, "top", reflection);
    Connect(layered, "base", interior);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", layered);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// A subsurface material, for the conservation and colour gates the transport
/// needs.
///
/// `subsurface_bsdf` with unit albedo absorbs nothing whatever it does with the
/// light -- reflect it as a Lambertian, or carry it through a random walk and
/// out somewhere else -- so a closed body of it in a uniform environment must
/// render one either way. With an albedo below one the same furnace measures
/// something else entirely: the colour that comes back out, which is the
/// quantity van de Hulst's inversion exists to make equal to the authored one.
CompiledMaterial MakeSubsurfaceMaterial(mx::DocumentPtr libraries,
                                        const GlslCompiler& compiler,
                                        const std::string& shadeKernel,
                                        const mx::Color3& radius,
                                        const std::string& name,
                                        const mx::Color3& colour =
                                            mx::Color3(1.0f, 1.0f, 1.0f),
                                        float anisotropy = 0.0f)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr bsdf = AddNode(doc, "subsurface_bsdf", "ss", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "color", colour);
    SetValue(bsdf, "radius", radius);   // color3 in the nodedef, not vector3
    SetValue(bsdf, "anisotropy", anisotropy);

    mx::NodePtr surface = AddNode(doc, "surface", "s", "surfaceshader");
    Connect(surface, "bsdf", bsdf);
    SetValue(surface, "opacity", 1.0f);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

CompiledMaterial MakeAbsorbingMaterial(mx::DocumentPtr libraries,
                                       const GlslCompiler& compiler,
                                       const std::string& shadeKernel,
                                       float ior,
                                       const mx::Color3& transmissionColour,
                                       float depth,
                                       const std::string& name,
                                       const mx::Color3& scatter =
                                           mx::Color3(0.0f, 0.0f, 0.0f),
                                       float anisotropy = 0.0f,
                                       float dispersionScale = 0.0f,
                                       float abbeNumber = 20.0f,
                                       // Dense and strongly coloured, and
                                       // never selected: see the note at the
                                       // call to SetValue below.
                                       const mx::Color3& subsurfaceRadius =
                                           mx::Color3(0.02f, 0.005f, 0.01f),
                                       const mx::Color3& subsurfaceColour =
                                           mx::Color3(0.9f, 0.1f, 0.4f))
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    // Built as `open_pbr_surface` rather than by wiring `anisotropic_vdf` under
    // a `layer` by hand, because this is the route a real asset takes: OpenPBR
    // is what decides that a transmissive material with a depth carries its
    // colour in the volume instead of on the surface, and it derives the
    // coefficient itself as -log(colour) / depth.
    mx::NodePtr surface = AddNode(doc, "open_pbr_surface", "s", "surfaceshader");
    SetValue(surface, "base_weight", 0.0f);
    SetValue(surface, "specular_roughness", 0.0f);
    SetValue(surface, "specular_ior", ior);
    SetValue(surface, "transmission_weight", 1.0f);
    SetValue(surface, "transmission_color", transmissionColour);
    SetValue(surface, "transmission_depth", depth);
    SetValue(surface, "transmission_scatter", scatter);
    SetValue(surface, "transmission_scatter_anisotropy", anisotropy);
    // Authored on the surface node, which is the only place they can be. The
    // generated program will accept both and read neither, which is why they
    // have to be read back off the document instead.
    SetValue(surface, "transmission_dispersion_scale", dispersionScale);
    SetValue(surface, "transmission_dispersion_abbe_number", abbeNumber);
    // Zero weight, and a subsurface that would be visible if the weight were
    // ever ignored. `open_pbr_surface` instantiates `subsurface_bsdf`
    // unconditionally and gates it with a `mix` on this weight, so these two
    // inputs reach a closure that publishes a medium for every OpenPBR material
    // in the scene. They are authored here, rather than left at their defaults,
    // so that the furnaces below fail if a transmissive surface ever enters the
    // subsurface interior instead of its own.
    SetValue(surface, "subsurface_weight", 0.0f);
    SetValue(surface, "subsurface_color", subsurfaceColour);
    SetValue(surface, "subsurface_radius", 1.0f);
    SetValue(surface, "subsurface_radius_scale", subsurfaceRadius);
    SetValue(surface, "coat_weight", 0.0f);
    SetValue(surface, "fuzz_weight", 0.0f);

    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// A smooth dielectric that only reflects, for measuring Fresnel directly.
///
/// `dielectric_bsdf` with zero roughness and the default "R" scatter mode: no
/// diffuse underneath, no transmission, nothing but the Fresnel curve. Under a
/// uniform environment that makes the rendered value *be* the reflectance at
/// the viewing angle, which is the one number a glass material is right or
/// wrong by.
CompiledMaterial MakeDielectricMaterial(mx::DocumentPtr libraries,
                                        const GlslCompiler& compiler,
                                        const std::string& shadeKernel,
                                        float ior,
                                        const std::string& name,
                                        float roughness = 0.0f,
                                        const std::string& scatterMode = "R")
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr bsdf = AddNode(doc, "dielectric_bsdf", "di", "BSDF");
    SetValue(bsdf, "weight", 1.0f);
    SetValue(bsdf, "ior", ior);
    SetValue(bsdf, "roughness", mx::Vector2(roughness, roughness));
    bsdf->setInputValue("scatter_mode", scatterMode, "string");

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

/// A material whose albedo is its own world-space tangent.
///
/// The tangent is not a value a material invents: it is the direction the
/// surface moves in as `u` increases, so it belongs to the *parameterisation*
/// and the kernel has to solve it from the triangle. Making it the albedo is
/// the only way to see it from a rendered image.
CompiledMaterial MakeTangentMaterial(mx::DocumentPtr libraries,
                                     const GlslCompiler& compiler,
                                     const std::string& shadeKernel,
                                     const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr tangent = AddNode(doc, "tangent", "t", "vector3");
    SetValue(tangent, "space", std::string("world"));

    // Named rather than inferred, for the reason MakeTexcoordMaterial gives:
    // `convert` is overloaded on its input type and resolves to the wrong
    // overload before the input is connected.
    mx::NodePtr colour = doc->addNode("convert", "c", "color3");
    colour->setNodeDefString("ND_convert_vector3_color3");
    if (mx::InputPtr in = colour->addInput("in", "vector3")) {
        in->setConnectedNode(tangent);
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

/// A material whose albedo is its own world-space shading normal.
///
/// The only way to read back what the kernel interpolated. Every other view of
/// a normal is filtered through a lighting response, which hides a normal that
/// is merely the *wrong* one as long as it still faces the light.
CompiledMaterial MakeNormalMaterial(mx::DocumentPtr libraries,
                                    const GlslCompiler& compiler,
                                    const std::string& shadeKernel,
                                    const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr normal = AddNode(doc, "normal", "n", "vector3");
    SetValue(normal, "space", std::string("world"));

    mx::NodePtr colour = doc->addNode("convert", "c", "color3");
    colour->setNodeDefString("ND_convert_vector3_color3");
    if (mx::InputPtr in = colour->addInput("in", "vector3")) {
        in->setConnectedNode(normal);
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

/// A material whose one image is a UDIM set of three tiles.
///
/// The tiles are 1001, 1002 and 1012: right of the first and above it, with
/// 1011 deliberately absent so the set is neither contiguous nor a full
/// rectangle. Real sets are not: ALab's turntable ships 1001 to 1007 and then
/// 1013, and a lookup that assumed `first + offset` would pass on a contiguous
/// set and fail on that one.
CompiledMaterial MakeUdimMaterial(mx::DocumentPtr libraries,
                                  const GlslCompiler& compiler,
                                  const std::string& shadeKernel,
                                  const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr uv = AddNode(doc, "texcoord", "uv", "vector2");

    mx::NodePtr image = doc->addNode("image", "tiles", "color3");
    image->setNodeDefString("ND_image_color3");
    if (mx::InputPtr file = image->addInput("file", "filename")) {
        file->setValueString("tiles.<UDIM>.png");
    }
    if (mx::InputPtr texcoord = image->addInput("texcoord", "vector2")) {
        texcoord->setConnectedNode(uv);
    }
    // The value a sample outside the set reads. Distinct from every tile, so
    // the test can tell "no tile there" from "the wrong tile".
    if (mx::InputPtr fallback = image->addInput("default", "color3")) {
        fallback->setValueString("0.0, 0.0, 0.0");
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

    CompiledMaterial compiled = CompileMaterial(
        doc, compiler, shadeKernel, name,
        {{"tiles_file", std::vector<int>{1001, 1002, 1012}}});
    // Three tiles, at pool slots 0, 1 and 2, in ascending tile order -- which
    // is the order the generator assigned them.
    compiled.textureSlots = {0, 1, 2};
    return compiled;
}

/// A solid image of one colour, for a UDIM tile.
TextureImage MakeSolidTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                              const char* name)
{
    constexpr std::uint32_t kSize = 8;
    TextureImage image;
    image.width = kSize;
    image.height = kSize;
    image.debugName = name;
    image.texels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    for (std::size_t i = 0; i < image.texels.size(); i += 4) {
        image.texels[i + 0] = r;
        image.texels[i + 1] = g;
        image.texels[i + 2] = b;
    }
    return image;
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
    image.texels.resize(static_cast<std::size_t>(kSize) * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool right = x >= kSize / 2;
            const bool top = y >= kSize / 2;
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            image.texels[i + 0] = (!top && !right) || (top && right) ? 255 : 0;
            image.texels[i + 1] = (!top && right) || (top && right) ? 255 : 0;
            image.texels[i + 2] = (top && !right) || (top && right) ? 255 : 0;
            image.texels[i + 3] = 255;
        }
    }
    return image;
}

/// A half-float latitude-longitude dome, `lit(u, v)` deciding each texel.
///
/// Half-float on purpose: it is the format an HDRI arrives in, and the only one
/// that can carry a dome's real range. The two values used here -- 1.0 and 0.0
/// -- are exactly representable, so the test needs no float-to-half conversion
/// of its own and cannot be wrong about one.
template <typename Lit>
TextureImage MakeDome(std::uint32_t width, std::uint32_t height, Lit lit)
{
    constexpr std::uint16_t kOne = 0x3c00;   // 1.0h
    constexpr std::uint16_t kZero = 0x0000;

    TextureImage dome;
    dome.width = width;
    dome.height = height;
    dome.format = TexelFormat::Rgba16Sfloat;
    dome.debugName = "dome";
    dome.texels.assign(static_cast<std::size_t>(width) * height * 8, 0);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / float(width);
            const float v = (static_cast<float>(y) + 0.5f) / float(height);
            const std::uint16_t value = lit(u, v) ? kOne : kZero;
            const std::size_t texel =
                (static_cast<std::size_t>(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                std::memcpy(dome.texels.data() + (texel + c) * 2, &value,
                            sizeof(value));
            }
            std::memcpy(dome.texels.data() + (texel + 3) * 2, &kOne,
                        sizeof(kOne));
        }
    }
    return dome;
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

/// A closed sphere of unit radius, for a furnace that probes every angle.
///
/// The two flat quads the other furnaces use only ever present a surface at the
/// angles the camera happens to look through it at, which for a delta
/// refraction is one angle per pixel and never a steep one. A sphere presents
/// every angle of incidence at once, including everything past the critical
/// angle, and it is closed, so a path inside it cannot leave by the side and
/// take the environment early. Both of those are needed to see an energy error
/// that is zero at normal incidence and grows away from it.
MeshPrototype MakeSphere(int rings = 48, int segments = 96)
{
    MeshPrototype prototype;
    prototype.debugName = "sphere";
    constexpr double kPi = 3.14159265358979323846;
    for (int ring = 0; ring <= rings; ++ring) {
        const double theta = kPi * double(ring) / double(rings);
        for (int segment = 0; segment <= segments; ++segment) {
            const double phi = 2.0 * kPi * double(segment) / double(segments);
            const auto x = static_cast<float>(std::sin(theta) * std::cos(phi));
            const auto y = static_cast<float>(std::cos(theta));
            const auto z = static_cast<float>(std::sin(theta) * std::sin(phi));
            prototype.positions.insert(prototype.positions.end(), {x, y, z});
            prototype.normals.insert(prototype.normals.end(), {x, y, z});
        }
    }
    const int stride = segments + 1;
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const auto a = static_cast<std::uint32_t>(ring * stride + segment);
            const auto b = static_cast<std::uint32_t>(a + stride);
            // Wound so the *geometric* normal faces out. Authored normals are
            // not enough: which side of an interface a transmission crosses is
            // decided from the geometric normal, so an inside-out sphere shades
            // plausibly -- the closures read the shading normal and never
            // notice -- while every path that leaves it is recorded as
            // *entering* the medium it just left. Half the interior walks then
            // run outside the sphere with no boundary ahead of them at all.
            prototype.indices.insert(prototype.indices.end(),
                                     {a, a + 1u, b, a + 1u, b + 1u, b});
        }
    }
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

/// The mean of a square window, for assertions about *colour*.
///
/// Spectral transport has chromatic noise where RGB transport has none: a
/// pixel's colour comes from four wavelengths drawn at random, so a grey
/// surface renders a slightly different grey in every pixel and converges only
/// in the mean. A single-pixel read of a colour ratio is therefore measuring
/// the sampler as much as the renderer. Averaging a window costs nothing here,
/// because every test that uses one asks about a region that is uniform by
/// construction.
Pixel Window(const std::vector<float>& image, float u, float v, int halfWidth)
{
    const auto cx = static_cast<int>(u * (kWidth - 1));
    const auto cy = static_cast<int>(v * (kHeight - 1));
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    int count = 0;
    for (int y = cy - halfWidth; y <= cy + halfWidth; ++y) {
        for (int x = cx - halfWidth; x <= cx + halfWidth; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) ||
                y >= static_cast<int>(kHeight)) {
                continue;
            }
            const std::size_t i =
                (static_cast<std::size_t>(y) * kWidth + x) * 4;
            r += image[i];
            g += image[i + 1];
            b += image[i + 2];
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return {static_cast<float>(r / count), static_cast<float>(g / count),
            static_cast<float>(b / count)};
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

            // The same frame with the geometry moved behind the camera, which
            // makes every pixel a background pixel.
            //
            // Comparing against the environment constant no longer works: with
            // spectral transport a background pixel is a four-wavelength
            // estimate of the environment's spectrum, so it lands near the
            // authored colour rather than exactly on it and every pixel differs
            // from it. But the sampler is seeded from the pixel, the sample
            // index and the bounce and from nothing else, so a pixel that only
            // ever saw the environment draws the same packet in both renders
            // and comes back bit-identical. A pixel that hit a quad does not.
            Scene empty = scene;
            empty.instances[0].transform =
                Transform(0.8f, 0.8f, 1.0f, -1.0f, 0.0f, 1000.0f);
            empty.instances[1].transform =
                Transform(0.8f, 0.8f, 1.0f, 1.0f, 0.0f, 1000.0f);
            tracer.SetScene(empty, materials);
            const std::vector<float> background =
                tracer.Render(kWidth, kHeight, LookDownZ(6.0f), single);
            CHECK_EQ(counts.size(), std::size_t(2));
            if (counts.size() == 2) {
                // Every pixel that is not the untouched background hit one of
                // the two quads, so the groups must account for exactly those
                // and no more. A background pixel carries the environment
                // constant unchanged, which no lit surface in this scene
                // produces.
                std::size_t hits = 0;
                for (std::size_t i = 0; i < kWidth * kHeight; ++i) {
                    if (image[i * 4] != background[i * 4] ||
                        image[i * 4 + 1] != background[i * 4 + 1] ||
                        image[i * 4 + 2] != background[i * 4 + 2]) {
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
                for (int run = 0; run < 5; ++run) {
                    const auto start = std::chrono::steady_clock::now();
                    tracer.Render(kTimingSize, kTimingSize, LookDownZ(2.2f), timed);
                    const std::chrono::duration<double, std::milli> elapsed =
                        std::chrono::steady_clock::now() - start;
                    best = std::min(best, elapsed.count());
                }
                return best;
            };

            // The baseline is measured at both ends and the faster taken. It is
            // the term the ratio below is most sensitive to -- it appears in the
            // numerator and the denominator -- and it is also the first thing
            // timed, so it carries whatever the GPU had not finished settling
            // into: clocks still ramping, a driver still compiling, a cache
            // still cold. A single warm-up render does not always clear that,
            // and when it does not the baseline reads slow, `added` collapses,
            // and the ratio explodes. This test failed twice on that and passed
            // on re-run both times, which is the signature.
            const double neitherFirst = timeScene({materials[0], materials[1]});
            const double one = timeScene({materials[0], heavyRed});
            const double both = timeScene({heavyWhite, heavyRed});
            const double neitherLast = timeScene({materials[0], materials[1]});
            const double neither = std::min(neitherFirst, neitherLast);

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

        // --- The tangent follows the UVs, not the triangle's first edge ------
        //
        // A tangent-space normal map is defined against the texture's own axes:
        // its x perturbs the surface along increasing u. So the frame has to be
        // solved from how position and coordinate vary together across the
        // triangle. Taking the first edge instead also produces a unit vector
        // orthogonal to the normal -- it is simply a different rotation on
        // every triangle, which turns a normal map into per-triangle noise and
        // makes the two triangles of a quad disagree by ninety degrees.
        //
        // This quad's coordinates are laid out deliberately across its edges:
        // u runs along world +Y and v along world -X, while the first edge of
        // both triangles runs along +X. A material whose albedo is its own
        // tangent must therefore render *green*. The edge-derived tangent this
        // replaced rendered it red, and every chess piece and every OpenPBR
        // Playground surface read its normal map through that frame.
        {
            const CompiledMaterial tangentMaterial = MakeTangentMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), "tangent");
            CHECK(!tangentMaterial.spirv.empty());

            MeshPrototype quad = MakeQuad();
            // Vertices are (-1,-1), (1,-1), (1,1), (-1,1) in the XY plane.
            // u = (y + 1) / 2, v = (1 - x) / 2.
            quad.uvs = {0, 1, 0, 0, 1, 0, 1, 1};

            Scene scene;
            scene.prototypes.push_back(quad);
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, {tangentMaterial});

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "tangent");

            const Pixel centre = At(image, 0.5f, 0.5f);
            std::printf("  tangent as albedo %.4f %.4f %.4f\n", centre.r,
                        centre.g, centre.b);

            // (0, 1, 0), not (1, 0, 0): green dominates, and by a lot, because
            // the two candidate frames are a right angle apart.
            CHECK(centre.g > 0.0f);
            CHECK(centre.g > centre.r * 4.0f);
            CHECK(centre.g > centre.b * 4.0f);
        }

        // --- Per-corner normals belong to a triangle, not to a vertex --------
        //
        // Face-varying normals are how a hard edge is authored: the two sides
        // of a crease need different normals at the same point, so they cannot
        // be stored per vertex at all. The kernel must therefore index them by
        // *primitive*, and a kernel that indexes them by vertex still produces
        // a plausible image -- it just averages the crease away.
        //
        // The two triangles of this quad are given normals that differ in a
        // channel the albedo shows directly: the lower-right half leans in x
        // and the upper-left half in y. Indexed by vertex, both halves would
        // read the same three entries and neither lean would appear.
        {
            const CompiledMaterial normalMaterial = MakeNormalMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), "normal");
            CHECK(!normalMaterial.spirv.empty());

            MeshPrototype quad = MakeQuad();
            quad.normalsPerCorner = true;
            // Triangle 0 is (v0, v1, v2), the lower-right half; triangle 1 is
            // (v0, v2, v3), the upper-left. Both still lean towards the camera,
            // so both are lit and the difference is in the albedo alone.
            const float lean = 0.6f;
            const float face = 0.8f;
            quad.normals = {
                lean, 0.0f, face,  lean, 0.0f, face,  lean, 0.0f, face,
                0.0f, lean, face,  0.0f, lean, face,  0.0f, lean, face,
            };

            Scene scene;
            scene.prototypes.push_back(quad);
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, {normalMaterial});

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "corner-normals");

            const Pixel lowerRight = At(image, 0.7f, 0.3f);
            const Pixel upperLeft = At(image, 0.3f, 0.7f);
            std::printf("  corner normals lower-right %.4f %.4f, "
                        "upper-left %.4f %.4f\n",
                        lowerRight.r, lowerRight.g, upperLeft.r, upperLeft.g);

            CHECK(lowerRight.r > lowerRight.g * 4.0f);
            CHECK(upperLeft.g > upperLeft.r * 4.0f);
        }

        // --- A furnace: a diffuse surface under a uniform sky -----------------
        //
        // The one lighting assertion with a closed form. A Lambertian surface
        // of albedo a under a uniform environment of radiance L leaves exactly
        // a*L, whatever the sampling strategy: the estimator integrates
        // f * L * cos over the hemisphere, and albedo/pi times pi is albedo.
        //
        // It is the test that says whether the environment is a *light* --
        // sampled by next-event estimation and weighted against BSDF sampling
        // -- or merely something a lucky ray runs into. The second answer also
        // converges, eventually, to a much noisier version of this number.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, {materials[0]});   // albedo 0.8, grey

            RenderSettings furnace;
            furnace.samplesPerPixel = 256;
            furnace.maxBounces = 2;
            // A white sky and no sun, so the only light in the scene is the one
            // being tested and the expected value has no second term.
            furnace.environmentColor[0] = 1.0f;
            furnace.environmentColor[1] = 1.0f;
            furnace.environmentColor[2] = 1.0f;
            furnace.sunRadiance[0] = 0.0f;
            furnace.sunRadiance[1] = 0.0f;
            furnace.sunRadiance[2] = 0.0f;

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), furnace);
            const Pixel centre = Window(image, 0.5f, 0.5f, 12);
            std::printf("  furnace: %.4f %.4f %.4f (expected 0.80)\n", centre.r,
                        centre.g, centre.b);

            // Five per cent, which is sampling noise at 256 samples and nothing
            // like the factor a missing strategy costs.
            CHECK_NEAR(centre.r, 0.8, 0.05);
            CHECK_NEAR(centre.g, 0.8, 0.05);
            CHECK_NEAR(centre.b, 0.8, 0.05);
        }

        // --- A dielectric reflects exactly its Fresnel share ------------------
        //
        // What a glass ball is right or wrong by. A smooth dielectric viewed
        // head on reflects ((n-1)/(n+1))^2 of what is in front of it -- 4.0 per
        // cent at n = 1.5 -- and under a uniform environment of unit radiance
        // the rendered pixel *is* that number, because every reflected
        // direction returns the same radiance and nothing else contributes.
        //
        // It is worth asserting because the answer is small and a plausible
        // image survives getting it wrong. Glass reflecting a light at a
        // twentieth of gold's strength looks, to the eye, a great deal like
        // glass not reflecting it at all, and the difference between 4 per cent
        // and 8 per cent is invisible without a number to check against.
        {
            struct Case { float ior; const char* name; };
            const Case cases[] = {
                {1.5f, "n=1.50"},
                // The IOR the Standard Shader Ball's glass authors.
                {1.54107f, "n=1.54"},
                {2.0f, "n=2.00"},
            };

            for (const Case& probe : cases) {
                const CompiledMaterial dielectric = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), probe.ior,
                    probe.name);
                CHECK(!dielectric.spirv.empty());
                if (dielectric.spirv.empty()) {
                    continue;
                }

                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                tracer.SetScene(scene, {dielectric});

                RenderSettings mirror;
                mirror.samplesPerPixel = 64;
                mirror.maxBounces = 2;
                // A uniform white sky and nothing else, so the reflected
                // radiance is one whichever way the surface sends the ray.
                for (int i = 0; i < 3; ++i) {
                    mirror.environmentColor[i] = 1.0f;
                    mirror.sunRadiance[i] = 0.0f;
                }

                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(6.0f), mirror);
                // A small window: only the centre of the quad is viewed at
                // normal incidence, and Fresnel climbs away from it.
                const Pixel centre = Window(image, 0.5f, 0.5f, 4);

                const double n = probe.ior;
                const double expected = ((n - 1.0) / (n + 1.0)) *
                                        ((n - 1.0) / (n + 1.0));
                std::printf("  fresnel %s: %.4f (closed form %.4f)\n",
                            probe.name, centre.g, expected);
                CHECK_NEAR(centre.g, expected, expected * 0.08);
            }
        }

        // --- A sharp gloss under a light must not lose what MIS splits --------
        //
        // The case the shader ball's glass actually is, and the one an MIS
        // weight is easiest to get wrong on. A near-mirror lobe puts almost all
        // of its density in a tiny cone, so at a direction toward the light the
        // closure's density dwarfs the light's and the balance heuristic gives
        // next-event estimation almost nothing. Everything then depends on the
        // *other* strategy -- the scattered ray hitting the light -- and if that
        // half is missing, the highlight does not become noisier, it disappears,
        // while every furnace test still passes because a furnace has no light
        // in it to lose.
        //
        // So the assertion is that the answer does not depend on the split. A
        // rect light large enough to swallow the whole specular lobe, mirrored
        // straight back at the camera, must read `R(0) * L` whether the surface
        // is a delta mirror -- which takes it entirely by hitting the light --
        // or a narrow gloss, which splits it between the two strategies.
        {
            const float ior = 1.5f;
            const float emitted = 2.0f;
            const double reflectance =
                ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));

            // The third case is what the shader ball's glass actually is: a
            // dielectric that *transmits*. Its front face has to reflect the
            // same Fresnel share as one that cannot transmit at all -- what a
            // surface does with the light it does not reflect cannot change how
            // much it reflects. With nothing behind the quad but blackness,
            // whatever is transmitted leaves the scene, so this pixel is the
            // reflection alone and must read the same number as the other two.
            const struct {
                float roughness;
                const char* mode;
                const char* name;
            } lobes[] = {
                {0.0f, "R", "delta"},
                {0.02f, "R", "gloss 0.02"},
                {0.0f, "RT", "delta, transmissive"},
                // Glossy *and* transmissive, which is what the shader ball's
                // glass is and the one combination the three above do not
                // reach. It is also where the two densities have to agree about
                // a convention: the closure picks a lobe before it picks a
                // direction, and if next-event estimation is weighed against a
                // reflection density that has not been multiplied by the chance
                // of choosing reflection -- about one in twenty-two for glass at
                // normal incidence -- while the scattered ray reports one that
                // has, the balance heuristic is being handed two different
                // quantities and the shortfall lands exactly here.
                {0.02f, "RT", "gloss, transmissive"},
            };
            constexpr int kLobes = 4;

            double measured[kLobes] = {0.0, 0.0, 0.0, 0.0};
            for (int which = 0; which < kLobes; ++which) {
                const CompiledMaterial mirror = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), ior,
                    lobes[which].name, lobes[which].roughness,
                    lobes[which].mode);
                CHECK(!mirror.spirv.empty());
                if (mirror.spirv.empty()) {
                    continue;
                }

                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});

                // Wide and close, so the mirrored lobe lands entirely on it.
                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[0] = 0.0f;
                rect.position[1] = 0.0f;
                rect.position[2] = 2.0f;
                rect.direction[0] = 0.0f;
                rect.direction[1] = 0.0f;
                rect.direction[2] = -1.0f;
                rect.uAxis[0] = 4.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
                rect.vAxis[0] = 0.0f; rect.vAxis[1] = 4.0f; rect.vAxis[2] = 0.0f;
                rect.area = 8.0f * 8.0f;
                rect.radiance[0] = emitted;
                rect.radiance[1] = emitted;
                rect.radiance[2] = emitted;
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {mirror});

                RenderSettings glossy;
                glossy.samplesPerPixel = 512;
                glossy.maxBounces = 2;
                for (int i = 0; i < 3; ++i) {
                    glossy.environmentColor[i] = 0.0f;
                    glossy.sunRadiance[i] = 0.0f;
                }

                // Close in, so the centre is viewed near normal incidence and
                // its mirror direction points into the light.
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(1.0f), glossy);
                measured[which] = Window(image, 0.5f, 0.5f, 4).g;
            }

            const double expected = reflectance * emitted;
            std::printf("  specular under a light: delta %.4f, gloss %.4f, "
                        "delta+T %.4f, gloss+T %.4f (closed form %.4f)\n",
                        measured[0], measured[1], measured[2], measured[3],
                        expected);
            for (int which = 0; which < kLobes; ++which) {
                CHECK_NEAR(measured[which], expected, expected * 0.10);
            }
        }

        // --- Forward scattering goes forward ----------------------------------
        //
        // The sign of the phase function, which no furnace can see: a medium
        // that scatters forward and one that scatters backward are both
        // lossless, so a closed slab of either renders one, and the images
        // differ only in *where* the light ends up.
        //
        // It was wrong. The Henyey-Greenstein sampler is stated in the
        // literature against `wo` -- the direction pointing back along the ray,
        // because a phase function is conventionally written between two
        // directions that both point away from the vertex -- and hdClaude built
        // its basis around the direction of travel. So the cosine's sign was
        // inverted and `g` of 1, which OpenPBR defines as fully forward, sent
        // every ray straight back. The OpenPBR Playground's bottle authors
        // exactly that value.
        //
        // The assertion is a beam through a slab. With a light behind it and
        // blackness everywhere else, a forward-scattering medium barely
        // deviates what passes through and a backward-scattering one turns it
        // around, so the first must read far brighter than the second. Both
        // directions are measured rather than one against a remembered number,
        // because it is their *ratio* that carries the sign.
        {
            const float emitted = 6.0f;
            struct Case { float g; const char* name; };
            const Case cases[] = {{0.9f, "forward"}, {-0.9f, "backward"}};
            double measured[2] = {0.0, 0.0};

            for (int which = 0; which < 2; ++which) {
                const CompiledMaterial medium = MakeScatteringMedium(
                    libraries, compiler, tracer.ShadeKernelSource(),
                    mx::Vector3(0.0f, 0.0f, 0.0f), mx::Vector3(6.0f, 6.0f, 6.0f),
                    cases[which].g, cases[which].name);
                CHECK(!medium.spirv.empty());
                if (medium.spirv.empty()) {
                    continue;
                }

                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.prototypes.push_back(MakeQuadFacingBack());
                Transform3x4 back;
                back.m[11] = -0.25f;
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                scene.instances.push_back({1, back, 0, true});

                // Behind the slab, wide enough that the unscattered beam lands
                // on it whichever way the medium bends what passes through.
                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[2] = -3.0f;
                rect.direction[2] = 1.0f;
                rect.uAxis[0] = 3.0f;
                rect.vAxis[1] = 3.0f;
                rect.area = 6.0f * 6.0f;
                for (int i = 0; i < 3; ++i) {
                    rect.radiance[i] = emitted;
                }
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {medium});

                RenderSettings beam;
                beam.samplesPerPixel = 512;
                beam.maxBounces = 12;
                for (int i = 0; i < 3; ++i) {
                    beam.environmentColor[i] = 0.0f;
                    beam.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(3.0f), beam);
                measured[which] = Window(image, 0.5f, 0.5f, 8).g;
            }

            std::printf("  phase: forward %.4f, backward %.4f (ratio %.2f)\n",
                        measured[0], measured[1],
                        measured[1] > 0.0 ? measured[0] / measured[1] : 0.0);
            // A factor rather than a value. What is being asserted is which way
            // round the two are, and by enough that noise cannot swap them.
            CHECK(measured[0] > measured[1] * 1.5);
        }

        // --- And what it transmits is estimated too ---------------------------
        //
        // The other half of a refracting surface, and until 2026-09-07 the half
        // that had no next-event estimate at all: the closure was only ever
        // asked about a light on the viewer's side, so a light seen *through*
        // glass was found by nothing but a scattered ray that happened to point
        // at it. Glass therefore had one strategy where every opaque surface
        // has two, which is unbiased and about twenty times louder.
        //
        // The assertion is the mirror image of the reflection one. A smooth
        // dielectric transmits `1 - R(0)` of what is directly behind it -- 96
        // per cent at n = 1.5 -- so with a rect light behind the quad, black
        // environment, and the camera in front, the pixel must read
        // `(1 - R(0)) * L` whether the surface is delta, which has no
        // next-event estimate and takes the light by hitting it, or glossy,
        // which now splits it between the two. A transmission response missing
        // its cosine, or an estimate weighed against the wrong density, moves
        // this number; the reflection tests cannot, because they never look
        // through anything.
        {
            const float ior = 1.5f;
            const float emitted = 2.0f;
            const double reflectance =
                ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
            const double expected = (1.0 - reflectance) * emitted;

            const struct { float roughness; const char* name; } lobes[] = {
                {0.0f, "delta"},
                {0.02f, "gloss"},
            };
            double measured[2] = {0.0, 0.0};

            for (int which = 0; which < 2; ++which) {
                const CompiledMaterial glass = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), ior,
                    lobes[which].name, lobes[which].roughness, "RT");
                CHECK(!glass.spirv.empty());
                if (glass.spirv.empty()) {
                    continue;
                }

                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});

                // Behind the quad, emitting forward through it at the camera.
                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[0] = 0.0f;
                rect.position[1] = 0.0f;
                rect.position[2] = -2.0f;
                rect.direction[0] = 0.0f;
                rect.direction[1] = 0.0f;
                rect.direction[2] = 1.0f;
                rect.uAxis[0] = 4.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
                rect.vAxis[0] = 0.0f; rect.vAxis[1] = 4.0f; rect.vAxis[2] = 0.0f;
                rect.area = 8.0f * 8.0f;
                rect.radiance[0] = emitted;
                rect.radiance[1] = emitted;
                rect.radiance[2] = emitted;
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {glass});

                RenderSettings through;
                through.samplesPerPixel = 512;
                through.maxBounces = 3;
                for (int i = 0; i < 3; ++i) {
                    through.environmentColor[i] = 0.0f;
                    through.sunRadiance[i] = 0.0f;
                }

                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(1.0f), through);
                measured[which] = Window(image, 0.5f, 0.5f, 4).g;
            }

            std::printf("  seen through glass: delta %.4f, gloss %.4f "
                        "(closed form %.4f)\n",
                        measured[0], measured[1], expected);
            CHECK_NEAR(measured[0], expected, expected * 0.10);
            CHECK_NEAR(measured[1], expected, expected * 0.10);
        }

        // --- An interior medium absorbs over the distance travelled -----------
        //
        // What makes honey honey rather than clear glass. OpenPBR hands the
        // colour of a transmissive material to its *volume* whenever
        // `transmission_depth` is above zero -- the surface tint is dropped
        // deliberately -- so a renderer that publishes the medium and never
        // transports it renders honey, wine and coloured glass identically, and
        // identically wrong.
        //
        // Beer-Lambert has a closed form and only one variable that matters, so
        // the test is exact: with the light `d` behind the quad and an
        // absorption of `sigma`, the pixel must read
        // `(1 - R(0)) * L * exp(-sigma * d)`. Absorption applied at the surface
        // instead of over the flight would be independent of `d`; applied with
        // the wrong sign or base it would not match at three different
        // coefficients at once.
        {
            const float ior = 1.5f;
            const float emitted = 2.0f;
            const float distance = 2.0f;   // quad at the origin, light at -2
            // OpenPBR's own mapping: an absorption of -log(colour)/depth.
            const mx::Color3 tint(0.8f, 0.6f, 0.4f);
            const float depth = 1.0f;
            const double sigma[3] = {-std::log(0.8) / depth,
                                     -std::log(0.6) / depth,
                                     -std::log(0.4) / depth};
            const double reflectance =
                ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));

            // Measured as a *ratio* against the same surface with a clear
            // interior, which is what makes this an assertion about the medium
            // rather than about OpenPBR. The absolute value carries whatever
            // factor `open_pbr_surface` puts on transmission -- energy
            // compensation among them, which is not (1 - R(0)) and is not
            // something this test has any business predicting. Dividing two
            // renders that differ only in the interior cancels it exactly, and
            // what survives is Beer-Lambert alone.
            const auto renderWith = [&](const mx::Color3& colour,
                                        const char* label) {
                const CompiledMaterial material = MakeAbsorbingMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), ior, colour,
                    depth, label);
                CHECK(!material.spirv.empty());
                if (material.spirv.empty()) {
                    return Pixel{0.0f, 0.0f, 0.0f};
                }

                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});

                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[2] = -distance;
                rect.direction[0] = 0.0f;
                rect.direction[1] = 0.0f;
                rect.direction[2] = 1.0f;
                rect.uAxis[0] = 4.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
                rect.vAxis[0] = 0.0f; rect.vAxis[1] = 4.0f; rect.vAxis[2] = 0.0f;
                rect.area = 8.0f * 8.0f;
                for (int i = 0; i < 3; ++i) {
                    rect.radiance[i] = emitted;
                }
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {material});

                RenderSettings tinted;
                tinted.samplesPerPixel = 512;
                tinted.maxBounces = 3;
                for (int i = 0; i < 3; ++i) {
                    tinted.environmentColor[i] = 0.0f;
                    tinted.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(1.0f), tinted);
                return Window(image, 0.5f, 0.5f, 8);
            };

            const Pixel clear = renderWith(mx::Color3(1.0f, 1.0f, 1.0f), "clear");
            const Pixel absorbing = renderWith(tint, "absorbing");

            const double gotR = absorbing.r / std::max(clear.r, 1.0e-6f);
            const double gotG = absorbing.g / std::max(clear.g, 1.0e-6f);
            const double gotB = absorbing.b / std::max(clear.b, 1.0e-6f);
            const double wantR = std::exp(-sigma[0] * distance);
            const double wantG = std::exp(-sigma[1] * distance);
            const double wantB = std::exp(-sigma[2] * distance);
            std::printf("  absorbing medium: %.4f %.4f %.4f "
                        "(closed form %.4f %.4f %.4f)\n",
                        gotR, gotG, gotB, wantR, wantG, wantB);

            // Wider than the other closed forms: the transmittance is upsampled
            // to the hero wavelengths and integrated back, so it carries the
            // round trip's error as well as the estimator's.
            CHECK_NEAR(gotR, wantR, wantR * 0.08);
            CHECK_NEAR(gotG, wantG, wantG * 0.08);
            CHECK_NEAR(gotB, wantB, wantB * 0.08);
        }

        // --- A lossless slab in a furnace renders the furnace -----------------
        //
        // The only exact assertion available about a transmissive surface, and
        // the one a random walk needs behind it. A closed object that absorbs
        // nothing, in a uniform environment of unit radiance, must render
        // exactly one however the light gets around inside it: every direction
        // sees the same radiance, so bending a path cannot change what it finds.
        //
        // A dielectric reflects and transmits and destroys nothing, so a slab of
        // one is lossless by construction. Anything but one here is energy the
        // closure invented or dropped, and it compounds: a path crosses this
        // slab twice, and a medium inside it many more times.
        {
            const auto furnace = [&](const CompiledMaterial& material,
                                     const char* label) {
                if (material.spirv.empty()) {
                    return Pixel{0.0f, 0.0f, 0.0f};
                }
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.prototypes.push_back(MakeQuadFacingBack());
                Transform3x4 back;
                back.m[11] = -0.2f;
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                scene.instances.push_back({1, back, 0, true});
                tracer.SetScene(scene, {material});

                RenderSettings box;
                box.samplesPerPixel = 512;
                box.maxBounces = 16;
                for (int i = 0; i < 3; ++i) {
                    box.environmentColor[i] = 1.0f;
                    box.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(3.0f), box);
                const Pixel centre = Window(image, 0.5f, 0.5f, 10);
                std::printf("  furnace, %-16s %.4f %.4f %.4f (expected 1.00)\n",
                            label, centre.r, centre.g, centre.b);
                return centre;
            };

            // The same furnace on a *sphere*, which is what makes it a test of
            // angle rather than of one angle.
            //
            // Two flat parallel quads present a delta refraction with exactly
            // one incidence per pixel and never a steep one, so every furnace
            // above is, without meaning to be, a near-normal measurement. A
            // sphere presents every angle at once and sends a large share of
            // its interior paths past the critical angle. It is still closed
            // and still lossless, so it must still read one.
            const auto sphereFurnace = [&](const CompiledMaterial& material,
                                           const char* label, float u,
                                           std::uint32_t samples) {
                if (material.spirv.empty()) {
                    return Pixel{0.0f, 0.0f, 0.0f};
                }
                Scene scene;
                scene.prototypes.push_back(MakeSphere());
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                tracer.SetScene(scene, {material});

                RenderSettings box;
                // Half the slab furnace's samples over a window four times its
                // area: a sphere fills the frame where two quads fill a patch
                // of it, so the same number of paths reaches the measurement.
                box.samplesPerPixel = samples;
                // Deeper than the slab's sixteen. A path inside a sphere of
                // glass is totally reflected at every incidence past 41.8
                // degrees, so it crosses the boundary many more times before it
                // finds an angle steep enough to leave by.
                box.maxBounces = 32;
                for (int i = 0; i < 3; ++i) {
                    box.environmentColor[i] = 1.0f;
                    box.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(3.0f), box);
                const Pixel patch = Window(image, u, 0.5f, 6);
                std::printf("  sphere furnace, %-14s u=%.2f  %.4f %.4f %.4f "
                            "(expected 1.00)\n",
                            label, u, patch.r, patch.g, patch.b);
                return patch;
            };

            const CompiledMaterial bare = MakeDielectricMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                "bare dielectric", 0.0f, "RT");
            CHECK(!bare.spirv.empty());
            const Pixel bareResult = furnace(bare, "dielectric_bsdf");

            const CompiledMaterial layered = MakeLayeredDielectric(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                "layered dielectric");
            CHECK(!layered.spirv.empty());
            const Pixel layeredResult = furnace(layered, "layer(R, T)");

            const CompiledMaterial openPbr = MakeAbsorbingMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                mx::Color3(1.0f, 1.0f, 1.0f), 1.0f, "openpbr",
                mx::Color3(0.0f, 0.0f, 0.0f), 0.0f);
            CHECK(!openPbr.spirv.empty());
            const Pixel openPbrResult = furnace(openPbr, "open_pbr_surface");
            CHECK_NEAR(bareResult.r, 1.0, 0.03);
            CHECK_NEAR(bareResult.g, 1.0, 0.03);
            CHECK_NEAR(bareResult.b, 1.0, 0.03);

            // Two per cent, which is three times the layered slab's measured
            // error and tight enough to catch either defect this furnace has
            // already found: 1.1505, when a layer of zero weight could still
            // claim a twentieth of the mixture density, and 1.0145, when a four
            // per cent loss and a five per cent gain were cancelling. Those two
            // are within three per cent of each other, which is exactly why the
            // earlier and looser tolerance would have passed the second one
            // indefinitely.
            // A subsurface material of unit albedo, whose radius has a zero
            // component -- which is what the bubblegum asset authors, and the
            // case OpenPBR's own text says has to be regularized: "this may
            // need to be regularized in the limit r -> 0 to avoid numerical
            // issues". A unit albedo absorbs nothing whatever the walk does
            // with the light, so this still has to read one now that a walk is
            // carrying it and not only when it was a Lambertian.
            //
            // Measured on the *slab* rather than on the sphere below, and for
            // the reason the zero component creates. The regularization floors
            // that channel at a mean free path 18.42 times shorter than the
            // longest, which here is 0.054 units; crossing a sphere of radius
            // one at that density is some thirteen hundred collisions and the
            // walk is capped at 256, so the sphere would be measuring the cap.
            // The slab is 0.2 thick, which the same channel crosses in about
            // fourteen collisions. Its open sides cost nothing at that density:
            // the walk wanders about 0.2 from where it entered, against a half
            // width of one.
            const CompiledMaterial sss = MakeSubsurfaceMaterial(
                libraries, compiler, tracer.ShadeKernelSource(),
                mx::Color3(1.0f, 0.0f, 0.068f), "subsurface");
            CHECK(!sss.spirv.empty());
            const Pixel sssResult = furnace(sss, "subsurface_bsdf");
            CHECK_NEAR(sssResult.r, 1.0, 0.02);
            CHECK_NEAR(sssResult.g, 1.0, 0.02);
            CHECK_NEAR(sssResult.b, 1.0, 0.02);

            CHECK_NEAR(layeredResult.r, 1.0, 0.02);
            CHECK_NEAR(layeredResult.g, 1.0, 0.02);
            CHECK_NEAR(layeredResult.b, 1.0, 0.02);
            CHECK_NEAR(openPbrResult.r, 1.0, 0.02);
            CHECK_NEAR(openPbrResult.g, 1.0, 0.02);
            CHECK_NEAR(openPbrResult.b, 1.0, 0.02);

            // And the OpenPBR slab reads what the bare layered pair reads.
            //
            // This is the gate on *which* interior a path enters.
            // `open_pbr_surface` instantiates `subsurface_bsdf` and
            // `anisotropic_vdf` whether or not the material uses either, so
            // this surface -- which transmits and has no subsurface at all --
            // describes two interiors, and the material above authors the
            // subsurface one dense and strongly coloured on purpose. A path
            // that entered it instead of the transmissive one would leave this
            // slab opaque and pink. The two readings agreeing is what says the
            // medium follows the lobe the mixture selected rather than
            // whichever closure ran last.
            CHECK_NEAR(openPbrResult.r, layeredResult.r, 0.01);
            CHECK_NEAR(openPbrResult.g, layeredResult.g, 0.01);
            CHECK_NEAR(openPbrResult.b, layeredResult.b, 0.01);

            // Dispersion moves light between wavelengths; it does not create or
            // destroy any. So the same slab, authored dispersive, must still
            // render one -- and still render *neutral*, which is the second
            // half of the claim and the one a tint would break.
            //
            // This is the gate on the packet collapse rather than on the
            // refraction. A dispersive surface can carry only its hero
            // wavelength, so the other three lanes are terminated there, and
            // the film averages four lanes whether or not three of them are
            // empty. Terminating without compensating renders a quarter of the
            // right answer; compensating twice -- which is what happens without
            // the flag that records the collapse, because a ray entering this
            // slab and leaving it shades the same material twice -- renders
            // four times it. Both are far outside any tolerance a furnace has,
            // which is what makes this the right instrument for a change whose
            // visible effect is a faint colour fringe nobody can check by eye.
            CompiledMaterial dispersive = MakeDielectricMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                "dispersive dielectric", 0.0f, "RT");
            CHECK(!dispersive.spirv.empty());
            // Set here rather than authored, so this measures the transport
            // alone: `dielectric_bsdf` has no dispersion input of its own, and
            // the document route is asserted separately below.
            dispersive.dispersionAbbe = 20.0f;
            const Pixel dispersiveResult = furnace(dispersive, "dispersive RT");
            CHECK_NEAR(dispersiveResult.r, 1.0, 0.04);
            CHECK_NEAR(dispersiveResult.g, 1.0, 0.04);
            CHECK_NEAR(dispersiveResult.b, 1.0, 0.04);

            // And the same again through the whole chain: the Abbe number
            // authored on `open_pbr_surface`, read back off the document
            // because MaterialX drops it, and applied by the integrator.
            const CompiledMaterial authored = MakeAbsorbingMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                mx::Color3(1.0f, 1.0f, 1.0f), 1.0f, "openpbr dispersive",
                mx::Color3(0.0f, 0.0f, 0.0f), 0.0f, 1.0f, 20.0f);
            CHECK(!authored.spirv.empty());
            CHECK_NEAR(authored.dispersionAbbe, 20.0, 1.0e-4);
            const Pixel authoredResult = furnace(authored, "open_pbr dispersive");
            CHECK_NEAR(authoredResult.r, 1.0, 0.04);
            CHECK_NEAR(authoredResult.g, 1.0, 0.04);
            CHECK_NEAR(authoredResult.b, 1.0, 0.04);

            // --- The same closures, at angles the flat furnaces never reach ---
            //
            // A solid glass sphere in a uniform environment must render one at
            // every pixel, exactly as a slab must. It is the same claim and the
            // same closures; the only thing that changes is that a curved
            // surface presents every angle of incidence, so an error that
            // vanishes at normal incidence has nowhere to hide.
            //
            // Sampled across the disc rather than at its centre. The centre of
            // a sphere is a normal-incidence measurement and reproduces the
            // slab; the interesting pixels are the ones whose refracted path
            // meets the far side steeply enough to be totally reflected.
            {
                const CompiledMaterial solid = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                    "sphere bare", 0.0f, "RT");
                CHECK(!solid.spirv.empty());

                const CompiledMaterial layeredSphere = MakeLayeredDielectric(
                    libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                    "sphere layered");
                CHECK(!layeredSphere.spirv.empty());

                for (const float u : {0.50f, 0.36f}) {
                    const Pixel bareSphere = sphereFurnace(solid, "RT", u, 256);
                    const Pixel layeredPatch =
                        sphereFurnace(layeredSphere, "layer(R, T)", u, 256);
                    CHECK_NEAR(bareSphere.g, 1.0, 0.03);
                    CHECK_NEAR(layeredPatch.g, 1.0, 0.03);
                }

                // --- The walk's own furnace waits for the defect it found ---
                //
                // A medium that scatters and absorbs nothing loses no light, so
                // a sphere full of one must read what the sphere full of vacuum
                // reads. It does not: at n = 1.5 it reads **1.42**, at n = 1.2
                // it reads 0.94, and with the reflection lobe taken off the top
                // it reads 0.61, while the vacuum control above reads 1.0003 and
                // the same closures read one at every angle probed. The defect
                // is in how `layer(R, layer_vdf(T, vdf))` accounts for its lobes
                // when a path is *inside* and past the critical angle, which is
                // a regime nothing here could reach before: a solid glass
                // sphere's internal rays never exceed the critical angle, and
                // two flat quads never make oblique internal directions at all.
                // It is present in the walk as committed, so it is not the
                // spectral work.
                //
                // The gate is therefore not asserted yet. Committing it at a
                // tolerance that passed 1.42 would record the defect as correct,
                // and this project's rule is that a furnace goes in with the fix
                // rather than ahead of it. `MakeScatteringMedium` and the sphere
                // stay so that it can, and the measurements are in
                // docs/implementation-notes.md.
                const struct {
                    mx::Vector3 scattering;
                    float anisotropy;
                    const char* name;
                } media[] = {
                    // The control: the same graph enclosing nothing, which must
                    // read what the layered dielectric reads. Anything wrong
                    // here belongs to `layer(bsdf, vdf)`, not to the walk.
                    {mx::Vector3(0.0f, 0.0f, 0.0f), 0.0f, "vacuum"},
                    {mx::Vector3(2.0f, 2.0f, 2.0f), 0.0f, "isotropic"},
                    // Forward scattering makes the longest walks, so it is where
                    // a per-step weight that does not cancel compounds furthest
                    // before roulette ends it.
                    {mx::Vector3(2.0f, 2.0f, 2.0f), 0.8f, "forward"},
                    {mx::Vector3(8.0f, 8.0f, 8.0f), 0.0f, "dense"},
                    // Four to one across the channels is far more chromatic than
                    // any real medium, which is the point of a gate.
                    {mx::Vector3(4.0f, 2.0f, 1.0f), 0.0f, "chromatic"},
                    {mx::Vector3(4.0f, 2.0f, 1.0f), 0.8f, "chromatic fwd"},
                };

                for (const auto& probe : media) {
                    const CompiledMaterial medium = MakeScatteringMedium(
                        libraries, compiler, tracer.ShadeKernelSource(),
                        mx::Vector3(0.0f, 0.0f, 0.0f), probe.scattering,
                        probe.anisotropy, probe.name);
                    CHECK(!medium.spirv.empty());
                    if (medium.spirv.empty()) {
                        continue;
                    }
                    // Four times the closure probes' samples. A chromatic
                    // medium's lanes carry different weights, where an
                    // achromatic one's are all exactly one, so the channels
                    // separate at a rate the cheap count cannot resolve against
                    // a gate this tight. The spread halves as it should with
                    // sample count -- 3.2 per cent at 256 and 1.4 at 2048 --
                    // which is what says it is variance and not the compounding
                    // the per-step form had.
                    const Pixel patch =
                        sphereFurnace(medium, probe.name, 0.5f, 1024);
                    CHECK_NEAR(patch.r, 1.0, 0.03);
                    CHECK_NEAR(patch.g, 1.0, 0.03);
                    CHECK_NEAR(patch.b, 1.0, 0.03);
                    // Neutral, asserted separately from unity: a lane weighting
                    // wrong by a common factor moves all three together, and one
                    // wrong per lane pulls them apart. Only the second is a
                    // chromatic defect.
                    CHECK(std::abs(patch.r - patch.b) < 0.025);
                    CHECK(std::abs(patch.r - patch.g) < 0.025);
                }
            }

            // --- Subsurface, on a closed body and against its own colour -----
            //
            // Two claims, and they need different instruments.
            //
            // The first is conservation, and it is the sphere's to make. A
            // subsurface material of unit albedo absorbs nothing, so a closed
            // one in a uniform environment must read one however far the walk
            // wanders and whatever angle it leaves at. The slab furnace above
            // makes the same claim at one incidence; this one makes it at every
            // incidence at once, which is what caught the interface index being
            // wrong when the same pair of tests was written for glass.
            {
                const CompiledMaterial lossless = MakeSubsurfaceMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(),
                    mx::Color3(0.25f, 0.25f, 0.25f), "sss lossless");
                CHECK(!lossless.spirv.empty());
                const Pixel patch =
                    sphereFurnace(lossless, "sss lossless", 0.5f, 512);
                CHECK_NEAR(patch.r, 1.0, 0.02);
                CHECK_NEAR(patch.g, 1.0, 0.02);
                CHECK_NEAR(patch.b, 1.0, 0.02);
            }

            // The second is the one conservation cannot see, and it is the
            // reason the mapping from `color` to a scattering albedo is not the
            // identity.
            //
            // MaterialX documents `color` as the diffuse reflectivity and
            // OpenPBR as "the observed reflection color", so a body of it thick
            // enough to be opaque, under a uniform sky of radiance one, must
            // render that colour -- the same closed form as the Lambertian
            // furnace far above, and the same reason: a surface under uniform
            // illumination returns its own albedo.
            //
            // This is what says van de Hulst's inversion is being applied and
            // applied the right way round. Handing `color` to the walk as its
            // single-scattering albedo instead -- which is what the mapping
            // looks like if nobody checks -- makes a 0.6 material read about
            // 0.19, because a walk whose collisions each survive with
            // probability 0.6 loses light at every one of them. That is a
            // factor of three, and it is invisible to every furnace above.
            //
            // A mean free path of 0.02 in a sphere of radius one puts the
            // diffusion length at about 0.05, so nothing crosses and the
            // measurement is of a half-space, which is what the relation is
            // written for.
            //
            // Measured achromatic first and chromatic second, which separates
            // the relation from the spectral mapping of the coefficients it
            // produces. An achromatic colour makes both coefficients neutral,
            // so the upsampling is exact and only van de Hulst and the walk are
            // under test. A chromatic one adds a scattering and an absorption
            // coefficient that are fitted to spectra independently and whose
            // *ratio* is the albedo the relation was solved for.
            {
                const mx::Color3 colours[] = {
                    mx::Color3(0.6f, 0.6f, 0.6f),
                    mx::Color3(0.2f, 0.2f, 0.2f),
                    mx::Color3(0.6f, 0.2f, 0.4f),
                };
                for (const mx::Color3& authored : colours) {
                    const CompiledMaterial coloured = MakeSubsurfaceMaterial(
                        libraries, compiler, tracer.ShadeKernelSource(),
                        mx::Color3(0.02f, 0.02f, 0.02f), "sss colour", authored);
                    CHECK(!coloured.spirv.empty());
                    if (coloured.spirv.empty()) {
                        continue;
                    }
                    const Pixel patch =
                        sphereFurnace(coloured, "sss colour", 0.5f, 512);
                    std::printf("  subsurface colour: %.4f %.4f %.4f "
                                "(authored %.2f %.2f %.2f)\n",
                                patch.r, patch.g, patch.b, authored[0], authored[1],
                                authored[2]);
                    // Three hundredths, and it discriminates between the
                    // three things it has to. The identity mistake -- handing
                    // `color` to the walk as its scattering albedo -- takes 0.6
                    // to 0.19. An undeviated entry, which is the other
                    // defensible boundary and the one an index-matched
                    // interface physically has, takes it to 0.5577. The cosine
                    // boundary reads 0.6173, and the 0.017 above the authored
                    // value is where van de Hulst's approximation and that
                    // boundary land together.
                    CHECK_NEAR(patch.r, authored[0], 0.03);
                    CHECK_NEAR(patch.g, authored[1], 0.03);
                    CHECK_NEAR(patch.b, authored[2], 0.03);
                    // Neutral where it was authored neutral, asserted
                    // separately: a relation applied to three channels before
                    // they became a spectrum came back 0.5178, 0.1971, 0.4158
                    // for an authored 0.6, 0.2, 0.4 -- a seventh short in red
                    // with the other two moving the other way, which no check
                    // against a single channel would have called a colour
                    // error.
                    CHECK(std::abs((patch.r - authored[0]) -
                                   (patch.b - authored[2])) < 0.02);
                }
            }
        }

        // --- No pixel is ever non-finite, however long the path --------------
        //
        // The gallery renders at eight bounces and has never seen this. Raise
        // the limit on the glass shader ball and the renderer starts producing
        // non-finite samples -- 208 at sixteen, 115 at thirty-two, 78 at
        // sixty-four, none at eight -- and it predates every closure change
        // made this week, since the code as it stood before them gives 103 at
        // sixteen (docs/implementation-notes.md, 2026-09-08).
        //
        // `hdClaudeImageDiff` already treats a non-finite sample as a gate
        // failure, so the gallery gate is capable of catching this and has
        // simply never been pointed at a long enough path. Nothing in the
        // suite was either: every render test above runs at three bounces or
        // fewer except the furnaces, which run in an empty uniform environment
        // with no lights in it at all.
        //
        // So this is the reproduction attempt, built from what the shader ball
        // has and the furnaces do not: glass, an emitter a ray can hit, and a
        // path long enough to find the awkward parts of both.
        {
            const auto anyNonFinite = [](const std::vector<float>& image) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < image.size(); i += 4) {
                    for (int c = 0; c < 3; ++c) {
                        if (!std::isfinite(image[i + c])) {
                            ++count;
                            break;
                        }
                    }
                }
                return count;
            };

            const CompiledMaterial glass = MakeDielectricMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                "deep glass", 0.02f, "RT");
            CHECK(!glass.spirv.empty());

            if (!glass.spirv.empty()) {
                Scene scene;
                scene.prototypes.push_back(MakeSphere());
                scene.instances.push_back({0, Transform3x4{}, 0, true});

                // An emitter a scattered ray can reach, which is what the
                // furnaces deliberately do not have.
                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[1] = 2.5f;
                rect.direction[1] = -1.0f;
                rect.uAxis[0] = 1.0f;
                rect.vAxis[2] = 1.0f;
                rect.area = 4.0f;
                for (int i = 0; i < 3; ++i) {
                    rect.radiance[i] = 12.0f;
                }
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {glass});

                for (const std::uint32_t bounces : {8u, 16u, 32u, 64u}) {
                    RenderSettings deep;
                    deep.samplesPerPixel = 256;
                    deep.maxBounces = bounces;
                    for (int i = 0; i < 3; ++i) {
                        deep.environmentColor[i] = 0.4f;
                        // The stand-in sun, which the shader ball scenes get
                        // because they author no dome, and which every furnace
                        // above deliberately switches off.
                        deep.sunRadiance[i] = 3.0f;
                    }
                    const std::vector<float> image =
                        tracer.Render(kWidth, kHeight, LookDownZ(3.0f), deep);
                    const std::size_t bad = anyNonFinite(image);
                    const Pixel centre = Window(image, 0.5f, 0.5f, 20);
                    std::printf("  depth %2u: %zu non-finite pixels, "
                                "centre %.4f %.4f %.4f\n",
                                bounces, bad, centre.r, centre.g, centre.b);
                    CHECK_EQ(bad, std::size_t(0));
                }
            }
        }

        // --- Dispersion refracts each wavelength by its own index -------------
        //
        // The conservation gate above says the collapse loses nothing. This says
        // the index actually varies, in the right direction and by the right
        // amount, which is the half a furnace can never see: a lossless slab
        // renders one whether the index moves with wavelength or not.
        //
        // The measurement is Fresnel, not the bend. A dispersive surface's
        // reflectance is `R(lambda) = ((n(lambda) - 1) / (n(lambda) + 1))^2`,
        // and with a rect light mirrored back at the camera and nothing else in
        // the scene, that spectrum *is* the pixel: everything transmitted leaves
        // into blackness. So the answer is the colour of a known reflectance
        // spectrum under a white light, which is an integral the host can do
        // exactly rather than a previous render to compare against.
        //
        // At n = 1.5 and an Abbe number of 20 -- a dense flint, more dispersive
        // than SF11 -- the index runs from 1.4888 at 700 nm to 1.5439 at 400 nm,
        // so blue reflects about a fifth more strongly than red. That is a
        // visible tint and a tiny absolute difference, 0.0386 against 0.0457,
        // which is exactly the shape of error an image cannot be checked for.
        //
        // The closed form is asserted against the bare `dielectric_bsdf` in RT
        // mode, which is one closure at one interface with one index. An
        // `open_pbr_surface` is measured beside it but held to a weaker claim,
        // and the difference is a real limit rather than a tolerance: MaterialX
        // builds OpenPBR's specular lobe as a *reflection-only* dielectric
        // layered over a transmission-only one, and dispersion is a property of
        // the transmitted medium, so only the second of the two is given the
        // wavelength's index. The first is indistinguishable at runtime from a
        // coat, which is a different interface and must not be dispersed at all.
        // So an OpenPBR glass refracts a spectrum and reflects a highlight that
        // is only partly tinted -- 1.04 blue over red where the interface itself
        // gives 1.15. It is recorded in docs/implementation-notes.md.
        {
            const float ior = 1.5f;
            // Authored as scale 1 at Abbe 20; the reader resolves the two to one
            // effective number and only that reaches the integrator.
            const float abbe = 20.0f;
            const float emitted = 2.0f;

            // The closed form. `R(lambda)` weighted by the illuminant a white
            // light emits, integrated against the colour matching functions, and
            // normalised by the illuminant's own luminous integral -- which is
            // the normalisation the film applies, and is what makes a perfect
            // reflector come back as the light's own colour rather than as its
            // absolute power.
            //
            // Stepped at 5 nm because that is the grid the film's tables are
            // uploaded on, and comparing two integrals of the same integrand on
            // different grids measures the grids.
            Vec3 xyz{};
            double norm = 0.0;
            for (float lambda = kLambdaMin; lambda <= kLambdaMax; lambda += 5.0f) {
                const float d65 = IlluminantD65(lambda);
                const Vec3 bar = CieXyzBar(lambda);
                const double n = DispersedIor(ior, abbe, lambda);
                const double r = ((n - 1.0) / (n + 1.0)) * ((n - 1.0) / (n + 1.0));
                xyz += bar * float(double(d65) * r);
                norm += double(bar.y) * double(d65);
            }
            const Vec3 expected =
                XyzToLinearSrgb(xyz * float(1.0 / norm)) * emitted;

            const auto mirrorUnderLight = [&](const CompiledMaterial& material) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});

                // Wide and close, so the whole mirrored lobe lands on it.
                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[2] = 2.0f;
                rect.direction[2] = -1.0f;
                rect.uAxis[0] = 4.0f;
                rect.vAxis[1] = 4.0f;
                rect.area = 8.0f * 8.0f;
                for (int i = 0; i < 3; ++i) {
                    rect.radiance[i] = emitted;
                }
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {material});

                RenderSettings settings;
                // Many times the samples the same measurement needs without
                // dispersion, because a collapsed packet carries one lane where
                // it used to carry four, and because sRGB red is a difference of
                // large XYZ terms and so amplifies what noise is left. That cost
                // is the honest price of dispersion, not a defect to tune away:
                // at 4096 the red channel lands 4.3 per cent below the closed
                // form and at 8192 it lands 2.9 per cent below it.
                settings.samplesPerPixel = 8192;
                settings.maxBounces = 2;
                for (int i = 0; i < 3; ++i) {
                    settings.environmentColor[i] = 0.0f;
                    settings.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(1.0f), settings);
                return Window(image, 0.5f, 0.5f, 4);
            };

            // The control: the same surface with no dispersion authored. It must
            // read the achromatic Fresnel value, which is what says the setup
            // measures reflectance and that the tint below comes from the index.
            const CompiledMaterial flat = MakeAbsorbingMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), ior,
                mx::Color3(1.0f, 1.0f, 1.0f), 0.0f, "openpbr flat",
                mx::Color3(0.0f, 0.0f, 0.0f), 0.0f, 0.0f, abbe);
            CHECK(!flat.spirv.empty());
            CHECK_NEAR(flat.dispersionAbbe, 0.0, 1.0e-6);

            const CompiledMaterial spread = MakeAbsorbingMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), ior,
                mx::Color3(1.0f, 1.0f, 1.0f), 0.0f, "openpbr spread",
                mx::Color3(0.0f, 0.0f, 0.0f), 0.0f, 1.0f, abbe);
            CHECK(!spread.spirv.empty());
            CHECK_NEAR(spread.dispersionAbbe, double(abbe), 1.0e-4);

            // The closure itself: one `dielectric_bsdf` in RT mode, whose single
            // index is the one dispersion moves. Set here rather than authored,
            // because `dielectric_bsdf` has no dispersion input of its own --
            // which is the whole reason this arrives from the host.
            CompiledMaterial bare = MakeDielectricMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), ior,
                "bare dispersive", 0.0f, "RT");
            CHECK(!bare.spirv.empty());
            bare.dispersionAbbe = abbe;

            if (!flat.spirv.empty() && !spread.spirv.empty() &&
                !bare.spirv.empty()) {
                const Pixel bareResult = mirrorUnderLight(bare);
                const Pixel flatResult = mirrorUnderLight(flat);
                const Pixel spreadResult = mirrorUnderLight(spread);

                const double achromatic =
                    ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0)) *
                    emitted;
                std::printf("  dispersion off:      %.4f %.4f %.4f "
                            "(closed form %.4f, neutral)\n",
                            flatResult.r, flatResult.g, flatResult.b, achromatic);
                std::printf("  dispersion on:       %.4f %.4f %.4f "
                            "(closed form %.4f %.4f %.4f)\n",
                            bareResult.r, bareResult.g, bareResult.b,
                            expected.x, expected.y, expected.z);
                std::printf("  through open_pbr:    %.4f %.4f %.4f "
                            "(reflection lobe not dispersed)\n",
                            spreadResult.r, spreadResult.g, spreadResult.b);

                // The control has no dispersion and must read the achromatic
                // Fresnel value, which is what says this setup measures
                // reflectance and that the tint below comes from the index.
                CHECK_NEAR(flatResult.r, achromatic, achromatic * 0.08);
                CHECK_NEAR(flatResult.g, achromatic, achromatic * 0.08);
                CHECK_NEAR(flatResult.b, achromatic, achromatic * 0.08);

                // And the dispersive closure must read the reflectance spectrum
                // its own relation implies, channel by channel.
                CHECK_NEAR(bareResult.r, expected.x, expected.x * 0.08);
                CHECK_NEAR(bareResult.g, expected.y, expected.y * 0.08);
                CHECK_NEAR(bareResult.b, expected.z, expected.z * 0.08);

                // The direction, asserted separately from the magnitude. A
                // dispersion relation with its sign inverted still produces a
                // tinted highlight, and still lands near a closed form computed
                // with the same inverted sign; what it cannot do is make blue
                // the strongly reflected end. Blue is the more strongly
                // refracted -- which is why a prism puts it at the bottom -- and
                // so also the more strongly reflected.
                const double bareRatio =
                    bareResult.r > 0.0 ? bareResult.b / bareResult.r : 0.0;
                const double spreadRatio =
                    spreadResult.r > 0.0 ? spreadResult.b / spreadResult.r : 0.0;
                const double flatRatio =
                    flatResult.r > 0.0 ? flatResult.b / flatResult.r : 0.0;
                std::printf("  blue/red: %.4f closure, %.4f open_pbr, "
                            "%.4f flat (closed form %.4f)\n",
                            bareRatio, spreadRatio, flatRatio,
                            expected.z / expected.x);
                CHECK(bareRatio > 1.08);
                CHECK(flatRatio > 0.96 && flatRatio < 1.04);

                // The authored route reaches the image. The number it reaches it
                // by is smaller than the interface's own, for the structural
                // reason in the comment above this block, so what is asserted is
                // that a value read off the document changed the render at all
                // and changed it in the right direction -- not that it produced
                // the full spread, which through this graph it cannot.
                CHECK(spreadRatio > flatRatio + 0.02);
            }
        }

        // --- A rect light is an emitter a ray can hit, and MIS splits it ------
        //
        // The analytic lights used to be absent from traversal entirely: they
        // were sampled by next-event estimation and nothing else, so a mirror
        // reflected no light and a glass ball held no highlight, however many
        // samples were thrown at it. They are intersected in closed form now,
        // which means the same light arrives by two strategies and each has to
        // take the share the balance heuristic gives it.
        //
        // The closed form is what makes that checkable. A Lambertian surface of
        // albedo `a` under a rectangular emitter of radiance L, directly above
        // it, leaves `a * L * F`, where F is the configuration factor from the
        // point to the rectangle -- the standard corner formula, four times over
        // for a rectangle centred on the point. It is the right test for an MIS
        // weight because it is a *total*: counting the light twice overshoots
        // it, weighting a strategy that has no partner undershoots it, and only
        // a partition that sums to one lands on it.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});

            const float halfU = 1.0f;
            const float halfV = 1.0f;
            const float height = 2.0f;
            const float emitted = 3.0f;

            Light rect;
            rect.type = static_cast<std::uint32_t>(LightType::Rect);
            rect.position[0] = 0.0f;
            rect.position[1] = 0.0f;
            rect.position[2] = height;
            // Emitting along -Z, down onto the quad.
            rect.direction[0] = 0.0f;
            rect.direction[1] = 0.0f;
            rect.direction[2] = -1.0f;
            rect.uAxis[0] = halfU; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
            rect.vAxis[0] = 0.0f; rect.vAxis[1] = halfV; rect.vAxis[2] = 0.0f;
            rect.area = (2.0f * halfU) * (2.0f * halfV);
            rect.radiance[0] = emitted;
            rect.radiance[1] = emitted;
            rect.radiance[2] = emitted;
            rect.castsShadows = 1;
            scene.lights.push_back(rect);

            tracer.SetScene(scene, {materials[0]});   // albedo 0.8, grey

            RenderSettings lit;
            lit.samplesPerPixel = 512;
            lit.maxBounces = 2;
            for (int i = 0; i < 3; ++i) {
                lit.environmentColor[i] = 0.0f;
                lit.sunRadiance[i] = 0.0f;
            }

            // The camera is below the light and looks at the quad; the light
            // itself is behind the camera, so what is measured is the surface.
            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(1.2f), lit);
            const Pixel centre = Window(image, 0.5f, 0.5f, 12);

            // Configuration factor from a point to one corner rectangle of
            // sides A and B at distance h, in the normalised form.
            const auto corner = [](double A, double B, double h) {
                const double x = A / h;
                const double y = B / h;
                const double rx = std::sqrt(1.0 + x * x);
                const double ry = std::sqrt(1.0 + y * y);
                return (x / rx * std::atan(y / rx) + y / ry * std::atan(x / ry)) /
                       (2.0 * 3.14159265358979);
            };
            const double factor = 4.0 * corner(halfU, halfV, height);
            const double expected = 0.8 * emitted * factor;
            std::printf("  rect light: %.4f %.4f %.4f (closed form %.4f)\n",
                        centre.r, centre.g, centre.b, expected);

            CHECK_NEAR(centre.r, expected, expected * 0.05);
            CHECK_NEAR(centre.g, expected, expected * 0.05);
            CHECK_NEAR(centre.b, expected, expected * 0.05);
        }

        // --- And the light itself is visible ----------------------------------
        //
        // The other half of the same change, and the one the reflection depends
        // on: a ray that reaches a light returns its radiance. A camera ray
        // carries no scatter density -- next-event estimation could not have
        // produced it -- so the light arrives unweighted and the pixel reads the
        // radiance the light was given. Before the lights were hittable this
        // pixel was the empty environment.
        {
            Scene scene;   // no geometry at all; only the light is in view

            const float emitted = 0.5f;
            Light rect;
            rect.type = static_cast<std::uint32_t>(LightType::Rect);
            rect.position[0] = 0.0f;
            rect.position[1] = 0.0f;
            rect.position[2] = 0.0f;
            // Facing the camera, which looks down -Z from +Z.
            rect.direction[0] = 0.0f;
            rect.direction[1] = 0.0f;
            rect.direction[2] = 1.0f;
            rect.uAxis[0] = 1.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
            rect.vAxis[0] = 0.0f; rect.vAxis[1] = 1.0f; rect.vAxis[2] = 0.0f;
            rect.area = 4.0f;
            rect.radiance[0] = emitted;
            rect.radiance[1] = emitted;
            rect.radiance[2] = emitted;
            scene.lights.push_back(rect);

            tracer.SetScene(scene, {materials[0]});

            RenderSettings lit;
            lit.samplesPerPixel = 64;
            lit.maxBounces = 2;
            for (int i = 0; i < 3; ++i) {
                lit.environmentColor[i] = 0.0f;
                lit.sunRadiance[i] = 0.0f;
            }

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), lit);
            const Pixel centre = Window(image, 0.5f, 0.5f, 8);
            std::printf("  light seen directly: %.4f %.4f %.4f (emitted %.2f)\n",
                        centre.r, centre.g, centre.b, emitted);

            CHECK_NEAR(centre.r, emitted, 0.02);
            CHECK_NEAR(centre.g, emitted, 0.02);
            CHECK_NEAR(centre.b, emitted, 0.02);
        }

        // --- A furnace under a *textured* dome --------------------------------
        //
        // The same closed form, with the environment sampled from a map's
        // luminance rather than uniformly over the sphere. That is a different
        // estimator reaching the same number, and it is the only way to catch
        // the two errors importance sampling can make without looking wrong:
        // a change of measure that is off by the map's Jacobian, and a
        // direction reconstruction that does not invert the lookup.
        //
        // Note the density here is *not* uniform even though the map is: rows
        // near a pole cover little solid angle, so a distribution built over
        // (u, v) has to carry sin(theta) to describe a uniform sky. A missing
        // Jacobian therefore shows up in this test, not only in a peaky map.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.textures.push_back(
                MakeDome(64, 32, [](float, float) { return true; }));
            scene.domeTexture = 0;
            scene.hasDomeLight = true;
            tracer.SetScene(scene, {materials[0]});   // albedo 0.8, grey

            RenderSettings furnace;
            furnace.samplesPerPixel = 256;
            furnace.maxBounces = 2;
            furnace.environmentColor[0] = 1.0f;
            furnace.environmentColor[1] = 1.0f;
            furnace.environmentColor[2] = 1.0f;
            furnace.sunRadiance[0] = 0.0f;
            furnace.sunRadiance[1] = 0.0f;
            furnace.sunRadiance[2] = 0.0f;

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), furnace);
            const Pixel centre = Window(image, 0.5f, 0.5f, 12);
            std::printf("  textured furnace: %.4f %.4f %.4f (expected 0.80)\n",
                        centre.r, centre.g, centre.b);

            CHECK_NEAR(centre.r, 0.8, 0.05);
            CHECK_NEAR(centre.g, 0.8, 0.05);
            CHECK_NEAR(centre.b, 0.8, 0.05);
        }

        // --- Half a sky, and which half -----------------------------------
        //
        // The furnace above pins the measure but not the *orientation*: a
        // reconstruction rotated by a quarter turn integrates a uniform sky to
        // the same number. So this one lights only u < 0.5, which the dome's
        // parameterisation places at x > 0, and asks a quad facing +Z what it
        // receives.
        //
        // The answer is exactly half the furnace. The lit set is the half-space
        // x > 0; the quad integrates over the hemisphere z > 0; and the cosine
        // weight is symmetric in x, so the lit part is exactly half of it. A
        // reconstruction rotated a quarter turn would light z > 0 or z < 0
        // instead, and answer 0.8 or 0.0.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.textures.push_back(
                MakeDome(64, 32, [](float u, float) { return u < 0.5f; }));
            scene.domeTexture = 0;
            scene.hasDomeLight = true;
            tracer.SetScene(scene, {materials[0]});

            RenderSettings half;
            half.samplesPerPixel = 512;
            // Two, not one. Both halves of a multiple-importance estimate
            // have to actually run: at one bounce the scattered ray is
            // retired before it reaches the environment kernel, so only the
            // next-event half is ever added and each contribution keeps just
            // its MIS weight. That is not a small bias -- it renders this
            // scene at 0.11 instead of 0.40 -- and it is a property of the
            // estimator rather than a fault, which is exactly why it is worth
            // stating here.
            half.maxBounces = 2;
            half.environmentColor[0] = 1.0f;
            half.environmentColor[1] = 1.0f;
            half.environmentColor[2] = 1.0f;
            half.sunRadiance[0] = 0.0f;
            half.sunRadiance[1] = 0.0f;
            half.sunRadiance[2] = 0.0f;

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), half);
            const Pixel centre = Window(image, 0.5f, 0.5f, 12);
            std::printf("  half sky: %.4f %.4f %.4f (expected 0.40)\n",
                        centre.r, centre.g, centre.b);

            CHECK_NEAR(centre.r, 0.4, 0.04);
            CHECK_NEAR(centre.g, 0.4, 0.04);
            CHECK_NEAR(centre.b, 0.4, 0.04);
        }

        // --- A colour temperature tints and does not brighten -----------------
        //
        // `enableColorTemperature` is the single input where transporting a
        // spectrum instead of a colour is most obviously the point: a light at
        // 2700 K emits Planck's law, and an RGB renderer can only multiply by
        // the blackbody's *colour*, which is a metamer of it.
        //
        // Two claims, and they pull against each other, which is why both are
        // asserted. The image has to change hue -- warm at 2700 K, cool at
        // 9000 K, measured as the red-to-blue ratio -- and its luminance has to
        // stay where the author put it, because UsdLux's control is a colour
        // control. A blackbody normalised to its *peak* rather than to its
        // luminous integral would pass the first and fail the second badly: a
        // 2700 K blackbody peaks well outside the visible range, so peak
        // normalisation would render this scene dark as well as warm.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});

            Light distant;
            distant.type = static_cast<std::uint32_t>(LightType::Distant);
            // Emitting along -Z, so it lights the quad's +Z face head on.
            distant.direction[0] = 0.0f;
            distant.direction[1] = 0.0f;
            distant.direction[2] = -1.0f;
            distant.angularRadius = 0.01f;
            // A distant light's irradiance is its radiance times the solid
            // angle of its disk, which at 0.01 rad is 3.1e-4. Unit radiance
            // would render this quad at a luminance of 1e-4 -- correct, and
            // far too dark for a ratio between two of them to be worth
            // reading. The reciprocal of that solid angle puts the neutral
            // render near the middle of the range instead, so what the
            // luminance assertion below rules out is a real shift rather than
            // a rounding one.
            const float kDistantRadiance = 1.0f / (3.14159265f * 0.01f * 0.01f);
            distant.radiance[0] = kDistantRadiance;
            distant.radiance[1] = kDistantRadiance;
            distant.radiance[2] = kDistantRadiance;
            distant.castsShadows = 0;

            RenderSettings lit;
            lit.samplesPerPixel = 256;
            lit.maxBounces = 2;
            // Nothing else may light the quad, or it would dilute the shift
            // being measured.
            for (int i = 0; i < 3; ++i) {
                lit.environmentColor[i] = 0.0f;
                lit.sunRadiance[i] = 0.0f;
            }

            const auto renderAt = [&](float kelvin) {
                Light light = distant;
                light.colorTemperature = kelvin;
                light.temperatureScale =
                    kelvin > 0.0f ? BlackbodyLuminousScale(kelvin) : 1.0f;
                Scene withLight = scene;
                withLight.lights.push_back(light);
                tracer.SetScene(withLight, {materials[0]});
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(4.0f), lit);
                return Window(image, 0.5f, 0.5f, 12);
            };

            const Pixel neutral = renderAt(0.0f);
            const Pixel warm = renderAt(2700.0f);
            const Pixel cool = renderAt(9000.0f);

            const auto ratio = [](const Pixel& p) {
                return p.r / std::max(p.b, 1.0e-6f);
            };
            std::printf("  colour temperature r/b: neutral %.3f, 2700 K %.3f, "
                        "9000 K %.3f\n",
                        ratio(neutral), ratio(warm), ratio(cool));
            std::printf("  luminance: neutral %.4f, 2700 K %.4f, 9000 K %.4f\n",
                        Luminance(neutral), Luminance(warm), Luminance(cool));

            // Lit, and lit well enough that the ratios below mean something.
            CHECK(Luminance(neutral) > 0.05f);
            // Warmer means more red than blue, and cooler means less. The
            // margins are wide because what is being asserted is the direction
            // of a large shift, not its size.
            CHECK(ratio(warm) > ratio(neutral) * 1.5f);
            CHECK(ratio(cool) < ratio(neutral));

            // And the luminance is the author's, whatever the temperature.
            CHECK_NEAR(Luminance(warm) / Luminance(neutral), 1.0, 0.05);
            CHECK_NEAR(Luminance(cool) / Luminance(neutral), 1.0, 0.05);
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
            const Pixel bottomLeft = Window(image, 0.375f, 0.375f, 8);
            const Pixel bottomRight = Window(image, 0.625f, 0.375f, 8);
            const Pixel topLeft = Window(image, 0.375f, 0.625f, 8);
            const Pixel topRight = Window(image, 0.625f, 0.625f, 8);
            std::printf("  texture corners: bl %.2f %.2f %.2f, br %.2f %.2f %.2f, "
                        "tl %.2f %.2f %.2f, tr %.2f %.2f %.2f\n",
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

        // --- A UDIM set is one image per tile, chosen per sample --------------
        //
        // `<UDIM>` names a 10x10 grid of images over UV space, tile
        // `1001 + floor(u) + 10 * floor(v)`. Until 2026-09-08 hdClaude expanded
        // the token to the set's first existing tile and shaded every tile with
        // it, which is right for the many assets that ship one tile and wrong
        // for every asset that does not: ALab's `electronics_turntable01` has
        // nineteen of its twenty-two meshes on a tile other than 1001, so 53.5
        // per cent of its surface carried the wrong image.
        //
        // One quad, three units wide in u and two in v, so it spans tiles 1001,
        // 1002 and 1003 along the bottom and 1011, 1012 and 1013 along the top.
        // The set ships only 1001, 1002 and 1012 -- deliberately neither
        // contiguous nor a rectangle, because a lookup that assumed
        // `first + offset` would pass on a contiguous set and fail on ALab's,
        // which jumps from 1007 to 1013.
        //
        // Each tile is a solid primary, so the assertion is which colour lands
        // where and needs no filtering argument. The three squares the set does
        // not ship must read the image node's `default`, which is black here:
        // an absent tile has no image, and reading a neighbour's would be the
        // same class of mistake as reading tile 1001 for everything.
        {
            const CompiledMaterial udimMaterial = MakeUdimMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), "udim");
            CHECK(!udimMaterial.spirv.empty());

            MeshPrototype quad = MakeQuad();
            // The quad is 2x2 in object space; three tiles across and two up.
            quad.uvs = {0, 0, 3, 0, 3, 2, 0, 2};

            Scene scene;
            scene.prototypes.push_back(quad);
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.textures.push_back(MakeSolidTexture(255, 0, 0, "tile.1001"));
            scene.textures.push_back(MakeSolidTexture(0, 255, 0, "tile.1002"));
            scene.textures.push_back(MakeSolidTexture(0, 0, 255, "tile.1012"));
            tracer.SetScene(scene, {udimMaterial});

            const std::vector<float> image =
                tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
            SavePpm(image, "texture-udim");

            // The quad covers the middle half of the frame in both axes, so a
            // point at (fu, fv) in UV space is at 0.25 + 0.5 * f of the image.
            // Sampled at the middle of each tile.
            const auto atTile = [&](float u, float v) {
                return Window(image, 0.25f + 0.5f * ((u + 0.5f) / 3.0f),
                              0.25f + 0.5f * ((v + 0.5f) / 2.0f), 5);
            };

            const Pixel t1001 = atTile(0.0f, 0.0f);
            const Pixel t1002 = atTile(1.0f, 0.0f);
            const Pixel t1003 = atTile(2.0f, 0.0f);
            const Pixel t1011 = atTile(0.0f, 1.0f);
            const Pixel t1012 = atTile(1.0f, 1.0f);
            const Pixel t1013 = atTile(2.0f, 1.0f);

            std::printf("  udim 1001 %.2f %.2f %.2f, 1002 %.2f %.2f %.2f, "
                        "1012 %.2f %.2f %.2f\n",
                        t1001.r, t1001.g, t1001.b, t1002.r, t1002.g, t1002.b,
                        t1012.r, t1012.g, t1012.b);
            std::printf("  udim absent tiles: 1003 %.3f, 1011 %.3f, 1013 %.3f "
                        "(expected 0)\n",
                        Luminance(t1003), Luminance(t1011), Luminance(t1013));

            // Each tile that exists shows its own colour, and no other.
            CHECK(t1001.r > t1001.g && t1001.r > t1001.b);
            CHECK(t1002.g > t1002.r && t1002.g > t1002.b);
            CHECK(t1012.b > t1012.r && t1012.b > t1012.g);

            // The three the set does not ship read the node's default. This is
            // the half of the claim that the old behaviour would fail loudest:
            // with every tile collapsed to 1001 these would be red.
            CHECK(Luminance(t1003) < 0.02f);
            CHECK(Luminance(t1011) < 0.02f);
            CHECK(Luminance(t1013) < 0.02f);
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
            // Compared in proportion, not in absolute radiance: the two renders
            // agree analytically but are separate Monte Carlo estimates, and a
            // fixed tolerance silently becomes a tighter one every time the
            // scene gets brighter. What this has to separate is a few per cent
            // of sampling noise from the defect it was written for, which put
            // these two at 0.20 against 0.55.
            const float tiltedLuminance = Luminance(byTransform);
            const float bakedLuminance = Luminance(byHost);
            CHECK(std::abs(tiltedLuminance - bakedLuminance) <=
                  0.04f * std::max(tiltedLuminance, bakedLuminance));
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
