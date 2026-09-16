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
#include "hdclaude/core/image_metrics.h"
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
#include <string>
#include <utility>
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
    material.thinWalled = AuthoredThinWalled(doc);
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
                                       const std::string& name,
                                       float roughness = 0.0f)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr reflection = AddNode(doc, "dielectric_bsdf", "dr", "BSDF");
    SetValue(reflection, "weight", 1.0f);
    SetValue(reflection, "ior", ior);
    SetValue(reflection, "roughness", mx::Vector2(roughness, roughness));
    reflection->setInputValue("scatter_mode", std::string("R"), "string");

    mx::NodePtr transmission = AddNode(doc, "dielectric_bsdf", "dt", "BSDF");
    SetValue(transmission, "weight", 1.0f);
    SetValue(transmission, "ior", ior);
    SetValue(transmission, "roughness", mx::Vector2(roughness, roughness));
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

/// A thin-walled `open_pbr_surface` glass, with a transmission depth.
///
/// The depth is there to be ignored. MaterialX 1.39.3's graph attaches an
/// interior volume whenever `transmission_depth` is positive, thin-walled or
/// not; OpenPBR says a thin-walled surface has no interior. A tinted colour with
/// a depth would absorb inside a volume, so reading exactly the clear sheet's
/// transmission is what says there is none.
CompiledMaterial MakeThinWalledGlass(mx::DocumentPtr libraries,
                                     const GlslCompiler& compiler,
                                     const std::string& shadeKernel,
                                     float ior, bool thinWalled,
                                     const std::string& name)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr surface = AddNode(doc, "open_pbr_surface", "s", "surfaceshader");
    SetValue(surface, "base_weight", 0.0f);
    SetValue(surface, "specular_roughness", 0.0f);
    SetValue(surface, "specular_ior", ior);
    SetValue(surface, "transmission_weight", 1.0f);
    SetValue(surface, "transmission_color", mx::Color3(0.5f, 0.5f, 0.5f));
    SetValue(surface, "transmission_depth", 0.5f);
    SetValue(surface, "geometry_thin_walled", thinWalled);
    mx::NodePtr material = AddNode(doc, "surfacematerial", "m", "material");
    Connect(material, "surfaceshader", surface);

    return CompileMaterial(doc, compiler, shadeKernel, name);
}

/// A thin-walled `open_pbr_surface` with whatever else the caller sets.
///
/// The OpenPBR Playground's thin-walled materials are not bare sheets: its
/// paint spill is thin-walled *with subsurface*, its MeetMat thin-walled with an
/// anisotropic coat, its bottle thin-walled with a thin film and an anisotropic
/// specular. A sheet on its own was the only thing measured when thin-walled
/// mode was built, and a sheet on its own is exactly what stayed correct.
CompiledMaterial MakeThinWalledSurface(
    mx::DocumentPtr libraries, const GlslCompiler& compiler,
    const std::string& shadeKernel, const std::string& name,
    const std::vector<std::pair<std::string, float>>& inputs,
    bool thinWalled = true)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr surface = AddNode(doc, "open_pbr_surface", "s", "surfaceshader");
    SetValue(surface, "geometry_thin_walled", thinWalled);
    // White everywhere a colour would otherwise absorb: OpenPBR's own defaults
    // include a base colour of 0.8, and a furnace measures what the *transport*
    // keeps, not what the asset's palette does.
    for (const char* colour : {"base_color", "specular_color", "coat_color",
                               "subsurface_color", "transmission_color",
                               "fuzz_color"}) {
        SetValue(surface, colour, mx::Color3(1.0f, 1.0f, 1.0f));
    }
    for (const auto& [input, value] : inputs) {
        SetValue(surface, input, value);
    }
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
                                      float ior = 1.5f,
                                      float roughness = 0.0f)
{
    mx::DocumentPtr doc = mx::createDocument();
    doc->importLibrary(libraries);

    mx::NodePtr reflection = AddNode(doc, "dielectric_bsdf", "dr", "BSDF");
    SetValue(reflection, "weight", 1.0f);
    SetValue(reflection, "ior", ior);
    SetValue(reflection, "roughness", mx::Vector2(roughness, roughness));
    reflection->setInputValue("scatter_mode", std::string("R"), "string");

    mx::NodePtr transmission = AddNode(doc, "dielectric_bsdf", "dt", "BSDF");
    SetValue(transmission, "weight", 1.0f);
    SetValue(transmission, "ior", ior);
    SetValue(transmission, "roughness", mx::Vector2(roughness, roughness));
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

/// One colour, with its first texel column black.
///
/// Solid would be the obvious thing and would test nothing at a seam: a filter
/// that wraps off the right edge of a solid tile comes back with the same
/// colour it left. The dark column is what makes a wrap visible -- a fetch near
/// the *right* edge that reaches past it returns a blend with this column
/// instead of the tile's colour, and the difference is half the brightness.
TextureImage MakeTileTexture(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                             const char* name)
{
    // Four texels across, deliberately coarse. The band a wrap corrupts is one
    // texel wide, and at this frame's resolution a tile is about twenty pixels,
    // so an eight-texel tile puts the whole artefact inside two pixels and a
    // finer one hides it completely. Four texels makes it measurable.
    constexpr std::uint32_t kSize = 4;
    TextureImage image;
    image.width = kSize;
    image.height = kSize;
    image.debugName = name;
    image.texels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            const bool dark = (x == 0);
            image.texels[i + 0] = dark ? 0 : r;
            image.texels[i + 1] = dark ? 0 : g;
            image.texels[i + 2] = dark ? 0 : b;
        }
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

/// A camera at `distance` down +Z, with a projection the depth guide can be
/// checked against in closed form.
///
/// `worldToClip` is what the depth AOV is defined in terms of, so a test that
/// leaves it at the identity is not testing depth at all. This builds the same
/// composition the Hydra layer does -- the view matrix times a standard
/// perspective projection with a [-1, 1] clip range -- so the depth of a plane
/// at a known distance can be predicted rather than merely eyeballed.
RenderCamera LookDownZWithClip(float distance, float nearPlane, float farPlane)
{
    RenderCamera camera = LookDownZ(distance);

    // Column-major, as GLSL reads it. The view matrix is the inverse of a pure
    // translation down +Z, so it translates by -distance; the projection is the
    // textbook one, negating and scaling z into [-1, 1].
    const float f = 1.0f / camera.tanHalfFov;
    const float range = nearPlane - farPlane;
    float clip[16] = {0};
    clip[0] = f / camera.aspect;             // column 0, row 0
    clip[5] = f;                             // column 1, row 1
    clip[10] = (farPlane + nearPlane) / range;
    clip[11] = -1.0f;                        // column 2, row 3: w = -z_view
    clip[14] = 2.0f * farPlane * nearPlane / range;
    // The view translation folded in: z_view = z_world - distance.
    clip[14] += clip[10] * -distance;
    clip[15] = distance;                     // w picks up -(z - distance)
    std::memcpy(camera.worldToClip, clip, sizeof(clip));
    return camera;
}

/// The depth the guide should report for a plane at `z` in world space.
double ExpectedNdcDepth(float z, float distance, float nearPlane, float farPlane)
{
    const double viewZ = static_cast<double>(z) - distance;   // negative, ahead
    const double range = nearPlane - farPlane;
    const double clipZ =
        (farPlane + nearPlane) / range * viewZ + 2.0 * farPlane * nearPlane / range;
    const double clipW = -viewZ;
    return (clipZ / clipW + 1.0) * 0.5;
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

    // NGX names instance and device extensions it will not initialise without,
    // and they have to be enabled when the instance and the device are created
    // -- which is why this is a construction-time thing and cannot be arranged
    // later by whatever decides to reconstruct a frame. Without it the backend
    // reports that the *device* cannot run DLSS, which is both untrue and
    // unfixable from where it is read.
    //
    // Safe to install unconditionally: with no SDK it names nothing, and the
    // context only enables extensions a device actually has, so a machine that
    // will never run DLSS is unaffected.
    const NgxRequirementProvider ngxProvider;

    std::unique_ptr<VulkanContext> context;
    try {
        VulkanContextOptions options;
        options.enableValidation = true;
        options.requirementProviders.push_back(&ngxProvider);
        context = std::make_unique<VulkanContext>(options);
    } catch (const VulkanError& error) {
        std::printf("SKIP: no usable Vulkan device (%s)\n", error.what());
        return 0;
    }
    if (!context->ValidationEnabled()) {
        std::fprintf(stderr, "FAIL: validation layer unavailable\n");
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
        // Lights are probed through their geometry here, and read through a
        // mirror in several of these; the render default is off.
        settings.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            single.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            timed.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            furnace.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                mirror.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                glossy.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                beam.lightGeometry = true;
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

        // --- A light inside a medium, seen through a rough interface ---------
        //
        // The one arrangement in which a shadow ray crosses an interior without
        // meeting a surface, and the one in which a scattered path reaches a
        // light after scattering while a rough surface's density is still on
        // record. Both were wrong, and neither can be seen from outside a
        // medium or through a smooth interface: a smooth interface has no
        // next-event estimate and stores no density, so it takes the whole
        // light by the scattered path, which was always right.
        //
        // The slab is two quads a unit apart with a rect light halfway, all
        // inside the medium, and the camera looks through the front quad.
        {
            const auto slabWithLight = [&](const CompiledMaterial& medium,
                                           float lightZ, float facing,
                                           float halfExtent, float radiance,
                                           float depth,
                                           std::uint32_t samples = 1024) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.prototypes.push_back(MakeQuadFacingBack());
                Transform3x4 back;
                back.m[11] = -depth;
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                scene.instances.push_back({1, back, 0, true});

                Light rect;
                rect.type = static_cast<std::uint32_t>(LightType::Rect);
                rect.position[2] = lightZ;
                rect.direction[2] = facing;
                rect.uAxis[0] = halfExtent;
                rect.vAxis[1] = halfExtent;
                rect.area = 4.0f * halfExtent * halfExtent;
                for (int i = 0; i < 3; ++i) {
                    rect.radiance[i] = radiance;
                }
                scene.lights.push_back(rect);
                tracer.SetScene(scene, {medium});

                RenderSettings settings;
                settings.lightGeometry = true;
                settings.samplesPerPixel = samples;
                settings.maxBounces = 8;
                for (int i = 0; i < 3; ++i) {
                    settings.environmentColor[i] = 0.0f;
                    settings.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(3.0f), settings);
                return Luminance(Window(image, 0.5f, 0.5f, 6));
            };

            // Absorption on the shadow ray. A light one unit into a medium that
            // absorbs three quarters of what crosses a unit must arrive at a
            // quarter of what it does through the same slab left clear, and a
            // rough interface splits it between next-event estimation and the
            // scattered path, so each strategy has to attenuate it. The flight
            // through a rough refraction is a little longer than the unit, and
            // the ratio is correspondingly a little below a quarter.
            {
                const float absorption = float(std::log(4.0));
                const struct { float roughness; const char* name; } cases[] = {
                    {0.0f, "smooth"}, {0.3f, "rough"}};
                double ratio[2] = {0.0, 0.0};
                for (int which = 0; which < 2; ++which) {
                    const CompiledMaterial clear = MakeScatteringMedium(
                        libraries, compiler, tracer.ShadeKernelSource(),
                        mx::Vector3(0.0f, 0.0f, 0.0f), mx::Vector3(0.0f, 0.0f, 0.0f),
                        0.0f, std::string("clear ") + cases[which].name, 1.5f,
                        cases[which].roughness);
                    const CompiledMaterial absorbing = MakeScatteringMedium(
                        libraries, compiler, tracer.ShadeKernelSource(),
                        mx::Vector3(absorption, absorption, absorption),
                        mx::Vector3(0.0f, 0.0f, 0.0f), 0.0f,
                        std::string("absorbing ") + cases[which].name, 1.5f,
                        cases[which].roughness);
                    CHECK(!clear.spirv.empty() && !absorbing.spirv.empty());
                    if (clear.spirv.empty() || absorbing.spirv.empty()) {
                        continue;
                    }
                    const double lit =
                        slabWithLight(clear, -1.0f, 1.0f, 0.9f, 2.0f, 2.0f);
                    const double dimmed =
                        slabWithLight(absorbing, -1.0f, 1.0f, 0.9f, 2.0f, 2.0f);
                    ratio[which] = lit > 0.0 ? dimmed / lit : 0.0;
                    std::printf("  light in a medium, %s: clear %.4f, absorbing "
                                "%.4f, ratio %.4f (closed form 0.25 at normal "
                                "incidence)\n",
                                cases[which].name, lit, dimmed, ratio[which]);
                }
                CHECK_NEAR(ratio[0], 0.25, 0.0125);
                CHECK_NEAR(ratio[1], 0.25, 0.0125);
            }

            // The density after a walk. A light facing away from the front
            // quad cannot be reached from it along a straight line, so only a
            // path that scatters behind the light finds its emitting side, and
            // next-event estimation from the front quad has nothing to share
            // with it. Weighing such a hit against the rough interface's
            // density anyway took the share `p_bsdf / (p_bsdf + p_light)` from
            // it, and a light's density grows as its area shrinks.
            //
            // So the instrument is the light's size at constant power. Both
            // lights are far smaller than a free path in the medium, which
            // scatters every eighth of a unit, so the light that reaches the
            // camera has forgotten how large its source was; the weight that
            // was wrong has not, and quartering the area quartered it. A
            // smooth interface would be no reference at all: the slab is open
            // at its sides, and roughness changes how much light total internal
            // reflection carries out of them.
            {
                const CompiledMaterial rough = MakeScatteringMedium(
                    libraries, compiler, tracer.ShadeKernelSource(),
                    mx::Vector3(0.0f, 0.0f, 0.0f), mx::Vector3(8.0f, 8.0f, 8.0f),
                    0.0f, "behind rough", 1.5f, 0.3f);
                CHECK(!rough.spirv.empty());
                if (!rough.spirv.empty()) {
                    // Four thousand samples, because a ratio of two
                    // small-light renders carries about three per cent of noise
                    // at one thousand: 1.038 there, where 4096 read 0.990, and
                    // a third light of an eighth the area read 1.026 -- no trend
                    // with size, which is the claim. The old weight read 0.809.
                    const double large = slabWithLight(
                        rough, -0.25f, -1.0f, 0.05f, 400.0f, 0.5f, 4096);
                    const double small = slabWithLight(
                        rough, -0.25f, -1.0f, 0.025f, 1600.0f, 0.5f, 4096);
                    const double ratio = large > 0.0 ? small / large : 0.0;
                    std::printf("  light facing away inside a scattering medium: "
                                "half-extent 0.05 %.4f, 0.025 at the same power "
                                "%.4f, ratio %.4f\n",
                                large, small, ratio);
                    CHECK(large > 0.0);
                    CHECK_NEAR(ratio, 1.0, 0.05);
                }
            }
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                through.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                tinted.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                box.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                box.lightGeometry = true;
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

                // The same two, rough.
                //
                // Every transmissive furnace in this file authors roughness
                // zero, and OpenPBR's `specular_roughness` defaults to 0.3, so
                // a rough interface crossed many times was measured nowhere: an
                // `open_pbr_surface` sphere of pure transmission reads 1.0425
                // at thirty-two bounces against 0.9974 for the same material
                // smooth, and the excess grows with the path limit -- 0.9968 at
                // eight and 1.0215 at sixteen -- which is a per-crossing gain
                // compounding rather than a truncation. A lossless interface is
                // lossless at any roughness, so these must read one too. The
                // pair separates the lobe from the layering: RT weighs its own
                // two lobes by Fresnel at the sampled microfacet, while
                // `layer(R, T)` has `mx_layer_bsdf` split the energy from the
                // reflection lobe's declared directional albedo.
                const CompiledMaterial roughSolid = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                    "sphere bare rough", 0.3f, "RT");
                CHECK(!roughSolid.spirv.empty());

                const CompiledMaterial roughLayered = MakeLayeredDielectric(
                    libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                    "sphere layered rough", 0.3f);
                CHECK(!roughLayered.spirv.empty());

                for (const float u : {0.50f, 0.36f}) {
                    const Pixel bareSphere = sphereFurnace(solid, "RT", u, 256);
                    const Pixel layeredPatch =
                        sphereFurnace(layeredSphere, "layer(R, T)", u, 256);
                    CHECK_NEAR(bareSphere.g, 1.0, 0.03);
                    CHECK_NEAR(layeredPatch.g, 1.0, 0.03);
                    // The absolute value is question 10 and is not asserted:
                    // both of these sit below one because nothing compensates
                    // the transmission lobe for what masking takes. What *is*
                    // asserted is that the two agree, because they are two
                    // encodings of one interface -- the same microfacet
                    // distribution, the same index -- and a renderer that makes
                    // them differ is sampling something other than what it
                    // reports. They differed by a factor of two before the
                    // density defects of 2026-09-17: 0.8840 against 1.1747 at
                    // the centre, 0.8538 against 1.9173 off it.
                    //
                    // Eight per cent, which is wide enough for two 256-sample
                    // measurements of a configuration this scattering and far
                    // inside the factor of two it is there to catch. It
                    // tightens when question 10 is answered.
                    const Pixel roughBare =
                        sphereFurnace(roughSolid, "RT rough", u, 256);
                    const Pixel roughLayer =
                        sphereFurnace(roughLayered, "layer(R, T) rough", u, 256);
                    CHECK_NEAR(roughLayer.g, roughBare.g, roughBare.g * 0.08);
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
                    // Lights are probed through their geometry here, and read through a
                    // mirror in several of these; the render default is off.
                    deep.lightGeometry = true;
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
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                settings.lightGeometry = true;
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
                // The centre, where the closed form is stated, and a window
                // twenty times its area for a ratio that has to resolve a per
                // cent: nine by nine pixels carry about three per cent of noise
                // in blue over red, sRGB red being a difference of large XYZ
                // terms.
                return std::make_pair(Window(image, 0.5f, 0.5f, 4),
                                      Window(image, 0.5f, 0.5f, 20));
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
                const Pixel bareResult = mirrorUnderLight(bare).first;
                const auto flatPair = mirrorUnderLight(flat);
                const auto spreadPair = mirrorUnderLight(spread);
                const Pixel flatResult = flatPair.first;
                const Pixel spreadResult = spreadPair.first;

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
                    spreadPair.second.r > 0.0
                        ? spreadPair.second.b / spreadPair.second.r
                        : 0.0;
                const double flatRatio =
                    flatPair.second.r > 0.0 ? flatPair.second.b / flatPair.second.r
                                            : 0.0;
                std::printf("  blue/red: %.4f closure, %.4f open_pbr, "
                            "%.4f flat (closed form %.4f)\n",
                            bareRatio, spreadRatio, flatRatio,
                            expected.z / expected.x);
                CHECK(bareRatio > 1.08);
                CHECK(flatRatio > 0.96 && flatRatio < 1.04);

                // And through `open_pbr_surface` the highlight is *neutral*.
                // Its specular lobe is `layer(reflection-only, transmission-
                // only)`, dispersion belongs to the lobe that transmits, and
                // everything reaching this pixel was reflected by the lobe that
                // does not -- so the expected ratio is the flat control's. This
                // used to assert a partial tint of about 1.037, which was one
                // nine-pixel window's noise: eight independent windows averaged
                // 0.9990 on the same code, and the centre read 0.9730 once lobe
                // selection drew a different sequence. The document's Abbe
                // number reaching the program is asserted above; that it
                // refracts is the closure's own measurement.
                CHECK_NEAR(spreadRatio, flatRatio, 0.02);
            }
        }

        // --- A thin-walled sheet transmits without refracting ----------------
        //
        // OpenPBR's thin-walled surface is "a 2d sheet with no interior" whose
        // translucent base "reduces to a thin sheet of dielectric". Two parallel
        // faces bend nothing, and the bounces between them give a lossless
        // sheet of single-face reflectance R the closed forms
        //
        //     reflected  2R / (1 + R)        transmitted  (1 - R) / (1 + R)
        //
        // -- 0.0769 and 0.9231 at n = 1.5, against a single interface's 0.04 and
        // 0.96. Roughness is ignored on the transmitted side by decision, so a
        // rough sheet transmits exactly what a smooth one does. Each is
        // measured at normal incidence, where R is the closed form's.
        {
            const float ior = 1.5f;
            const float emitted = 2.0f;
            const double R = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
            const double sheetReflect = 2.0 * R / (1.0 + R);
            const double sheetTransmit = (1.0 - R) / (1.0 + R);

            const auto sheetUnderLightAt = [&](const CompiledMaterial& material,
                                               float lightZ, float facing,
                                               float sky, std::uint32_t bounces) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                if (emitted > 0.0f && facing != 0.0f) {
                    Light rect;
                    rect.type = static_cast<std::uint32_t>(LightType::Rect);
                    rect.position[2] = lightZ;
                    rect.direction[2] = facing;
                    rect.uAxis[0] = 20.0f;
                    rect.vAxis[1] = 20.0f;
                    rect.area = 40.0f * 40.0f;
                    for (int i = 0; i < 3; ++i) {
                        rect.radiance[i] = emitted;
                    }
                    scene.lights.push_back(rect);
                }
                tracer.SetScene(scene, {material});

                RenderSettings settings;
                settings.lightGeometry = true;
                settings.samplesPerPixel = 256;
                settings.maxBounces = bounces;
                for (int i = 0; i < 3; ++i) {
                    settings.environmentColor[i] = sky;
                    settings.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(6.0f), settings);
                return Window(image, 0.5f, 0.5f, 4).g;
            };
            const auto sheetUnderLight = [&](const CompiledMaterial& material,
                                             float lightZ, float facing,
                                             float sky) {
                return sheetUnderLightAt(material, lightZ, facing, sky, 3);
            };

            const struct { float roughness; const char* name; } sheets[] = {
                {0.0f, "smooth"}, {0.3f, "rough"}};
            for (const auto& sheet : sheets) {
                CompiledMaterial thin = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), ior,
                    std::string("thin sheet ") + sheet.name, sheet.roughness, "RT");
                CHECK(!thin.spirv.empty());
                if (thin.spirv.empty()) {
                    continue;
                }
                thin.thinWalled = true;

                // Behind the sheet, facing the camera through it.
                const double through = sheetUnderLight(thin, -2.0f, 1.0f, 0.0f);
                // Behind the camera, facing the sheet, seen in its reflection.
                const double mirrored = sheetUnderLight(thin, 8.0f, -1.0f, 0.0f);
                // A white sky on both sides and no light: a lossless sheet.
                const double furnace = sheetUnderLight(thin, 0.0f, 0.0f, 1.0f);
                std::printf("  thin sheet %s: transmits %.4f (closed form %.4f), "
                            "reflects %.4f (closed form %.4f), furnace %.4f\n",
                            sheet.name, through, sheetTransmit * emitted, mirrored,
                            sheetReflect * emitted, furnace);
                CHECK_NEAR(through, sheetTransmit * emitted, sheetTransmit * emitted * 0.01);
                CHECK_NEAR(furnace, 1.0, 0.02);
                if (sheet.roughness == 0.0f) {
                    CHECK_NEAR(mirrored, sheetReflect * emitted, sheetReflect * emitted * 0.05);
                }
            }

            // Authored through OpenPBR, the flag is read off the document and a
            // depth with a grey colour -- which would absorb inside a volume --
            // changes nothing: the sheet has no interior. MaterialX's graph
            // layers the sheet's transmission under its reflection lobe, so
            // this also measures the internal bounces carried by the base.
            const CompiledMaterial glass = MakeThinWalledGlass(
                libraries, compiler, tracer.ShadeKernelSource(), ior, true,
                "openpbr thin glass");
            CHECK(!glass.spirv.empty());
            CHECK(glass.thinWalled);
            if (!glass.spirv.empty()) {
                const double through = sheetUnderLight(glass, -2.0f, 1.0f, 0.0f);
                const double furnace = sheetUnderLight(glass, 0.0f, 0.0f, 1.0f);
                std::printf("  thin open_pbr glass with a depth: transmits %.4f "
                            "(closed form %.4f), furnace %.4f\n",
                            through, sheetTransmit * emitted, furnace);
                CHECK_NEAR(through, sheetTransmit * emitted, sheetTransmit * emitted * 0.02);
                CHECK_NEAR(furnace, 1.0, 0.02);
            }

            // --- The furnace holds however long the paths are -----------------
            //
            // A furnace at three bounces cannot see a lobe that gains a little
            // on every crossing: three crossings of a one per cent gain is one
            // per cent, and thirty-two is a third again. A sheet is the shape
            // that makes this worth asserting, because a path can cross it
            // repeatedly -- it is transparent and has two faces -- so any
            // mismatch between a delta's response and its density compounds.
            //
            // It is the assertion the OpenPBR Playground needed: that scene
            // renders at a mean of 0.166 at eight bounces, 0.181 at sixteen and
            // 1.53 at thirty-two, which is not a scene converging.
            for (const auto& sheet : sheets) {
                CompiledMaterial thin = MakeDielectricMaterial(
                    libraries, compiler, tracer.ShadeKernelSource(), ior,
                    std::string("thin sheet bounces ") + sheet.name,
                    sheet.roughness, "RT");
                if (thin.spirv.empty()) {
                    continue;
                }
                thin.thinWalled = true;
                for (const std::uint32_t bounces : {3u, 8u, 16u, 32u}) {
                    const double furnace =
                        sheetUnderLightAt(thin, 0.0f, 0.0f, 1.0f, bounces);
                    std::printf("  thin sheet %s furnace at %u bounces: %.4f\n",
                                sheet.name, bounces, furnace);
                    CHECK_NEAR(furnace, 1.0, 0.02);
                }
            }

            // --- Thin-walled with the lobes real assets put beside it ---------
            //
            // Every thin-walled material in the OpenPBR Playground carries
            // something else: subsurface on the paint spill and the yellow
            // paint, an anisotropic coat on MeetMat, a thin film and an
            // anisotropic specular on the bottle. Each is a furnace here, and a
            // furnace is the same measurement whatever the lobes are: a
            // lossless surface in a uniform environment renders it exactly.
            {
                const struct {
                    const char* name;
                    std::vector<std::pair<std::string, float>> inputs;
                    /// Whether the furnace is asserted. Two of these do not
                    /// read one yet and are recorded as open questions rather
                    /// than asserted away (docs/roadmap.md): a thin-walled
                    /// transmissive sheet loses all but its own reflection on a
                    /// closed shape, and subsurface loses a tenth whether the
                    /// surface is a sheet or a solid.
                    bool asserted;
                } materials[] = {
                    {"subsurface",
                     {{"subsurface_weight", 0.2f}, {"subsurface_radius", 0.1f}},
                     false},
                    {"anisotropic coat",
                     {{"coat_weight", 1.0f},
                      {"coat_roughness", 0.1f},
                      {"coat_roughness_anisotropy", 1.0f}},
                     true},
                    // The OpenPBR Playground's `paper`, which is thin-walled
                    // with a thin film a thousandth thick and nothing else.
                    //
                    // The weight is authored beside the thickness because
                    // OpenPBR's `thin_film_weight` defaults to *zero* and the
                    // graph mixes the filmed reflection against the unfilmed
                    // one on it. A thickness on its own therefore reaches a
                    // lobe of weight nothing, and these two entries measured an
                    // ordinary opaque surface while naming a film: they read
                    // exactly what the transmission-only entry beside them
                    // read, to the last digit.
                    {"thin film only",
                     {{"thin_film_weight", 1.0f},
                      {"thin_film_thickness", 1.0e-3f}},
                     true},
                    // Transmission on its own, which is what separates the film
                    // from the transmission when the pair of them does not read
                    // one. Without this entry a failure of the pair has two
                    // suspects and no way to tell them apart.
                    {"transmission only", {{"transmission_weight", 1.0f}}, false},
                    // The same transmission with a smooth interface. OpenPBR's
                    // `specular_roughness` defaults to 0.3, so every entry here
                    // that does not say otherwise is a *rough* dielectric,
                    // while every transmissive furnace that passes elsewhere in
                    // this file authors roughness zero. That is the difference
                    // the pair of them isolates.
                    {"transmission only, smooth",
                     {{"transmission_weight", 1.0f}, {"specular_roughness", 0.0f}},
                     false},
                    {"thin film and transmission",
                     {{"thin_film_weight", 1.0f},
                      {"thin_film_thickness", 1.0e-3f},
                      {"transmission_weight", 1.0f}},
                     false},
                };
                // A closed sphere, not a quad. A path meets a quad once and
                // leaves; the scene these materials come from is full of
                // objects a path crosses, re-enters and crosses again, and a
                // per-crossing error is only visible once it compounds. The
                // sphere also presents every angle of incidence at once.
                const auto sphereFurnace = [&](const CompiledMaterial& material,
                                               std::uint32_t bounces) {
                    Scene scene;
                    scene.prototypes.push_back(MakeSphere());
                    scene.instances.push_back({0, Transform3x4{}, 0, true});
                    tracer.SetScene(scene, {material});

                    RenderSettings settings;
                    settings.samplesPerPixel = 256;
                    settings.maxBounces = bounces;
                    for (int i = 0; i < 3; ++i) {
                        settings.environmentColor[i] = 1.0f;
                        settings.sunRadiance[i] = 0.0f;
                    }
                    const std::vector<float> image =
                        tracer.Render(kWidth, kHeight, LookDownZ(4.0f), settings);
                    return static_cast<double>(Window(image, 0.5f, 0.5f, 8).g);
                };

                // Each is measured thin-walled and solid. The solid render is
                // the control: where both lose the same energy the loss belongs
                // to the lobes, and where only the thin-walled one loses it the
                // sheet is what put it there.
                for (const auto& entry : materials) {
                    for (const bool thinWalled : {false, true}) {
                        CompiledMaterial material = MakeThinWalledSurface(
                            libraries, compiler, tracer.ShadeKernelSource(),
                            std::string(thinWalled ? "thin " : "solid ") +
                                entry.name,
                            entry.inputs, thinWalled);
                        CHECK(!material.spirv.empty());
                        if (material.spirv.empty()) {
                            continue;
                        }
                        material.thinWalled = thinWalled;
                        // A ladder rather than two ends, because the two ways a
                        // furnace misses one look the same at a single depth: a
                        // path limit truncates transport and loses energy that
                        // more bounces restore, while a per-crossing gain
                        // compounds and grows with them. Only the shape of the
                        // sequence separates them.
                        for (const std::uint32_t bounces : {3u, 8u, 16u, 32u}) {
                            const double furnace =
                                sphereFurnace(material, bounces);
                            std::printf("  %s %s sphere furnace at %u bounces: "
                                        "%.4f%s\n",
                                        thinWalled ? "thin-walled" : "solid",
                                        entry.name, bounces, furnace,
                                        entry.asserted ? "" : " (recorded, not "
                                                              "asserted)");
                            if (entry.asserted) {
                                CHECK_NEAR(furnace, 1.0, 0.03);
                            }
                        }
                    }
                }
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            lit.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            lit.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            furnace.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            half.lightGeometry = true;
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
            // Lights are probed through their geometry here, and read through a
            // mirror in several of these; the render default is off.
            lit.lightGeometry = true;
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

        // --- Light linking and shadow linking ----------------------------------
        //
        // UsdLux's `collection:lightLink` says which geometry a light
        // illuminates and `collection:shadowLink` which geometry blocks it. The
        // closed forms are the unlinked scenes themselves: a surface that a
        // shadow link lets see past an occluder must render exactly as it does
        // with no occluder at all, and a surface a light link leaves out must
        // receive nothing. Each is paired with the render the link changes, so
        // a link that did nothing could not pass.
        //
        // The occluders are black, so the only thing they can do to the floor
        // is block it. A white one would reflect light back and the "no
        // occluder" render would stop being the right reference.
        //
        // Both kinds of emitter a scattered ray can reach are covered: a distant
        // light, found in the environment kernel's cone test, and the
        // environment itself. A shadow-linked emitter is estimated by next-event
        // estimation alone (shade.comp.glsl), so these renders are also the
        // check that doing so neither drops nor doubles the light.
        {
            const CompiledMaterial black = MakeDiffuseMaterial(
                libraries, compiler, tracer.ShadeKernelSource(),
                mx::Color3(0.0f, 0.0f, 0.0f), "black");
            CHECK(!black.spirv.empty());
            const std::vector<CompiledMaterial> linkMaterials = {materials[0],
                                                                 black};

            constexpr int kLink = 0;
            const auto instance = [](const Transform3x4& transform,
                                     std::uint32_t material,
                                     std::vector<std::uint32_t> categories) {
                MeshInstance placed{0, transform, material, true};
                placed.linkCategories = std::move(categories);
                return placed;
            };

            // A distant light 45 degrees off the floor's normal, emitting
            // towards -x and -z. A 1x1 occluder two units up at x = 2 casts
            // its shadow on the centre of the floor while lying outside the
            // camera's view, which at that height spans |x| < 1.
            Light distant;
            distant.type = static_cast<std::uint32_t>(LightType::Distant);
            distant.direction[0] = -0.70710678f;
            distant.direction[1] = 0.0f;
            distant.direction[2] = -0.70710678f;
            distant.angularRadius = 0.01f;
            const float kDistantRadiance = 1.0f / (3.14159265f * 0.01f * 0.01f);
            for (int i = 0; i < 3; ++i) {
                distant.radiance[i] = kDistantRadiance;
            }

            RenderSettings linkSettings;
            linkSettings.lightGeometry = true;
            linkSettings.samplesPerPixel = 256;
            linkSettings.maxBounces = 3;
            for (int i = 0; i < 3; ++i) {
                linkSettings.environmentColor[i] = 0.0f;
                linkSettings.sunRadiance[i] = 0.0f;
            }

            const Transform3x4 floorPlace = Transform(4, 4, 1, 0, 0, 0);
            const Transform3x4 occluderPlace = Transform(0.5f, 0.5f, 1, 2, 0, 2);

            const auto renderDistant = [&](bool occluder, std::int32_t lightLink,
                                           std::int32_t shadowLink,
                                           std::vector<std::uint32_t> floorIn,
                                           std::vector<std::uint32_t> occluderIn) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back(instance(floorPlace, 0, floorIn));
                if (occluder) {
                    scene.instances.push_back(
                        instance(occluderPlace, 1, occluderIn));
                }
                Light light = distant;
                light.lightLink = lightLink;
                light.shadowLink = shadowLink;
                scene.lights.push_back(light);
                scene.linkCategoryCount = 1;
                tracer.SetScene(scene, linkMaterials);
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(4.0f), linkSettings);
                return Luminance(Window(image, 0.5f, 0.5f, 8));
            };

            const float open = renderDistant(false, -1, -1, {}, {});
            const float shadowed = renderDistant(true, -1, -1, {}, {});
            const float pastExcluded =
                renderDistant(true, -1, kLink, {kLink}, {});
            const float blockedIncluded =
                renderDistant(true, -1, kLink, {kLink}, {kLink});
            const float unlinkedFloor =
                renderDistant(false, kLink, -1, {}, {kLink});
            const float linkedFloor = renderDistant(false, kLink, -1, {kLink}, {});
            std::printf("  distant light links: open %.4f, shadowed %.4f, "
                        "occluder outside shadowLink %.4f, inside %.4f; floor "
                        "outside lightLink %.4f, inside %.4f\n",
                        open, shadowed, pastExcluded, blockedIncluded,
                        unlinkedFloor, linkedFloor);

            // White at 0.8 under an irradiance of cos 45: 0.8 / pi * 0.7071.
            CHECK_NEAR(open, 0.18006, 0.01);
            CHECK(shadowed < open * 0.02f);
            CHECK_NEAR(pastExcluded / open, 1.0, 0.02);
            CHECK(blockedIncluded < open * 0.02f);
            CHECK(unlinkedFloor < open * 0.001f);
            CHECK_NEAR(linkedFloor / open, 1.0, 0.02);

            // The environment, under a black roof one unit above the floor and
            // wider than anything it could let past at the sides. The camera
            // sits under the roof, looking down, so the roof is never seen.
            RenderSettings skySettings = linkSettings;
            skySettings.samplesPerPixel = 512;
            for (int i = 0; i < 3; ++i) {
                skySettings.environmentColor[i] = 1.0f;
            }
            const Transform3x4 skyFloor = Transform(200, 200, 1, 0, 0, 0);
            const Transform3x4 roof = Transform(200, 200, 1, 0, 0, 1);

            const auto renderSky = [&](bool roofed, std::int32_t lightLink,
                                       std::int32_t shadowLink,
                                       std::vector<std::uint32_t> floorIn,
                                       std::vector<std::uint32_t> roofIn) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back(instance(skyFloor, 0, floorIn));
                if (roofed) {
                    scene.instances.push_back(instance(roof, 1, roofIn));
                }
                scene.hasDomeLight = true;
                scene.domeLightLink = lightLink;
                scene.domeShadowLink = shadowLink;
                scene.linkCategoryCount = 1;
                tracer.SetScene(scene, linkMaterials);
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(0.5f), skySettings);
                return Luminance(Window(image, 0.5f, 0.5f, 12));
            };

            const float skyOpen = renderSky(false, -1, -1, {}, {});
            const float skyRoofed = renderSky(true, -1, -1, {}, {});
            const float skyPastRoof = renderSky(true, -1, kLink, {kLink}, {});
            const float skyUnlinked = renderSky(false, kLink, -1, {}, {kLink});
            const float skyLinked = renderSky(false, kLink, -1, {kLink}, {});
            std::printf("  environment links: open %.4f, roofed %.4f, roof "
                        "outside shadowLink %.4f; floor outside lightLink %.4f, "
                        "inside %.4f\n",
                        skyOpen, skyRoofed, skyPastRoof, skyUnlinked, skyLinked);

            // A white furnace for the upper hemisphere: the albedo.
            CHECK_NEAR(skyOpen, 0.8, 0.02);
            CHECK(skyRoofed < skyOpen * 0.02f);
            CHECK_NEAR(skyPastRoof / skyOpen, 1.0, 0.02);
            CHECK(skyUnlinked < skyOpen * 0.001f);
            CHECK_NEAR(skyLinked / skyOpen, 1.0, 0.02);
        }

        // --- A wide distant light, with its geometry out of the frame ----------
        //
        // A distant light's irradiance on a surface facing it is its radiance
        // times the cone's projected solid angle, `pi sin^2 theta`, so a white
        // floor under one reads `0.8 sin^2 theta` times the radiance. The
        // `lightGeometry` setting is off by default, and with it off no ray can
        // reach a light: next-event estimation takes the whole contribution.
        // The environment kernel added a scattered ray's share of the disc all
        // the same, which went unseen for as long as every test rendered with
        // geometry on and every sun was half a degree wide. At 40 degrees it
        // was 19 per cent.
        {
            constexpr float kHalfAngle = 20.0f * 3.14159265f / 180.0f;
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform(4, 4, 1, 0, 0, 0), 0, true});
            Light distant;
            distant.type = static_cast<std::uint32_t>(LightType::Distant);
            distant.direction[0] = 0.0f;
            distant.direction[1] = 0.0f;
            distant.direction[2] = -1.0f;
            distant.angularRadius = kHalfAngle;
            for (int i = 0; i < 3; ++i) {
                distant.radiance[i] = 1.0f;
            }
            scene.lights.push_back(distant);
            tracer.SetScene(scene, materials);

            const double expected =
                0.8 * std::sin(kHalfAngle) * std::sin(kHalfAngle);
            for (const bool geometry : {false, true}) {
                RenderSettings wide;
                wide.lightGeometry = geometry;
                wide.samplesPerPixel = 256;
                wide.maxBounces = 3;
                for (int i = 0; i < 3; ++i) {
                    wide.environmentColor[i] = 0.0f;
                    wide.sunRadiance[i] = 0.0f;
                }
                const std::vector<float> image =
                    tracer.Render(kWidth, kHeight, LookDownZ(4.0f), wide);
                const float measured = Luminance(Window(image, 0.5f, 0.5f, 12));
                std::printf("  distant light of 40 degrees, geometry %s: %.4f "
                            "(closed form %.4f)\n",
                            geometry ? "on" : "off", measured, expected);
                CHECK_NEAR(measured, expected, expected * 0.02);
            }
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
            scene.textures.push_back(MakeTileTexture(255, 0, 0, "tile.1001"));
            scene.textures.push_back(MakeTileTexture(0, 255, 0, "tile.1002"));
            scene.textures.push_back(MakeTileTexture(0, 0, 255, "tile.1012"));
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

            // --- The seam ----------------------------------------------
            //
            // A tile is a whole image and the shared sampler addresses REPEAT,
            // so a bilinear fetch just inside a tile's right edge reaches past
            // it and comes back with the tile's *left* edge -- a one-texel band
            // of the wrong thing down every boundary where two tiles meet. It
            // is invisible on a solid tile, which is why these tiles have a
            // dark first column, and invisible at a tile's centre, which is why
            // every assertion above would have passed with it there.
            //
            // Sampled at 0.97 of the way across tile 1001, between the last
            // texel's centre at 0.9375 and the edge, which is where a wrap
            // blends and a clamp does not. Correct is the tile's full colour;
            // wrapped is that blended with the dark column, about half of it.
            // Single pixels, not a window. The quad puts about twenty pixels
            // across a tile, so any window wide enough to average noise is also
            // wide enough to reach into the next tile -- which is a different
            // effect, and one that would fail this whether the seam were fixed
            // or not.
            const auto pixelAtU = [&](float u) {
                return At(image, 0.25f + 0.5f * (u / 3.0f), 0.5f);
            };
            // Two points a texel apart, which a clamp resolves to the same
            // texel and a wrap does not: 0.875 is the last texel's centre,
            // and 0.96 is past it, in the only band where the two differ.
            //
            // Compared against each other rather than against the tile's
            // middle, because they are close enough that the quad's own
            // illumination falloff between them is negligible where between
            // here and the middle it is not. Against the middle the failing
            // margin was 0.89 to a 0.9 threshold, which is a coin toss
            // rather than a gate.
            const Pixel seam = pixelAtU(0.96f);
            const Pixel edge = pixelAtU(0.875f);
            const Pixel middle = pixelAtU(0.5f);
            std::printf("  udim seam: %.3f past the last texel against %.3f "
                        "at it (%.3f at the tile's middle)\n",
                        seam.r, edge.r, middle.r);
            CHECK(edge.r > 0.05f);
            // Wrapping blends the dark first column into the outer sample
            // and costs about a third of it; clamping to the edge texel
            // costs nothing, so the two read the same.
            CHECK(seam.r > edge.r * 0.85f);

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

        // --- A frame decides its own invalidation, and says what it is of ----
        //
        // The single frame-scoped entry point exists because independent
        // setters do not work: hdCodex's SetScene / SetShadingMode /
        // implicit-resize-inside-Trace triad each had to guess what the others
        // implied, each guessed wrong differently, and that triad is the direct
        // cause of four shipped defects (docs/lessons-from-hdcodex.md D3).
        //
        // So the claim under test is not that BeginFrame renders -- every test
        // above already proves that -- but that a caller which *forgets* to ask
        // for a reset still gets one whenever the renderer knows the
        // accumulation cannot continue. Each case below asks for
        // `resetAccumulation = false` and is right to be overruled.
        {
            Scene scene;
            scene.prototypes.push_back(MakeQuad());
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(scene, {materials[0]});

            RenderSettings frameSettings = settings;
            frameSettings.samplesPerPixel = 4;
            frameSettings.maxBounces = 1;

            hdclaude::FrameDescription description;
            description.width = kWidth;
            description.height = kHeight;
            description.camera = LookDownZ(4.0f);
            description.settings = frameSettings;
            description.sceneRevision = 1;

            // The first frame of all restarts, because there is nothing to
            // continue.
            description.settings.resetAccumulation = false;
            hdclaude::FrameResult first =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(first.Valid());
            CHECK(first.accumulationReset);

            // A second frame that changes nothing may continue, and the
            // renderer must not invent a reason to restart it.
            description.settings.firstSample = 4;
            hdclaude::FrameResult second =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(second.Valid());
            CHECK(!second.accumulationReset);

            // Its identity is its own, and increasing.
            CHECK(second.index > first.index);

            // What it is a frame *of* travels with it. Re-deriving this from
            // the renderer's current extents is what hdCodex did, and it is why
            // a host that resized mid-frame got an image of the wrong size with
            // nothing to say so.
            CHECK_EQ(second.width, kWidth);
            CHECK_EQ(second.height, kHeight);
            CHECK_EQ(second.image.size(),
                     std::size_t(kWidth) * kHeight * 4);
            CHECK_EQ(second.firstSample, 4u);
            CHECK_EQ(second.sampleCount, frameSettings.samplesPerPixel);

            // A different resolution cannot continue an accumulation of the
            // old one, whatever the caller believes.
            description.width = kWidth / 2;
            description.height = kHeight / 2;
            hdclaude::FrameResult resized =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(resized.Valid());
            CHECK(resized.accumulationReset);
            CHECK_EQ(resized.width, kWidth / 2);
            CHECK_EQ(resized.image.size(),
                     std::size_t(kWidth / 2) * (kHeight / 2) * 4);

            // Nor can a different scene.
            description.sceneRevision = 2;
            hdclaude::FrameResult revised =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(revised.accumulationReset);

            // Nor a switch between the two accumulation contracts, which never
            // share storage or history.
            description.mode = hdclaude::RenderMode::Interactive;
            hdclaude::FrameResult switched =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(switched.accumulationReset);

            // --- Motion vectors ---------------------------------------------
            //
            // A static scene seen from a camera that has not moved has moved
            // by nothing, and that is worth asserting rather than assuming:
            // the previous placement of an instance defaults to the identity,
            // and a renderer that believed it would report every surface as
            // having flown in from the origin.
            {
                hdclaude::FrameDescription still = description;
                // Its own extents: an earlier case above resized this
                // description, and a test that inherits a size it did not
                // choose is measuring something it did not mean to.
                still.width = kWidth;
                still.height = kHeight;
                still.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                still.settings.resetAccumulation = true;
                (void)tracer.EndFrame(tracer.BeginFrame(still));
                const hdclaude::FrameResult second =
                    tracer.EndFrame(tracer.BeginFrame(still));
                CHECK_EQ(second.motion.size(),
                         std::size_t(kWidth) * kHeight * 2);
                double worstStill = 0.0;
                for (const float value : second.motion) {
                    worstStill = std::max(worstStill, std::abs(double(value)));
                }
                CHECK(worstStill < 1e-3);

                // A camera that steps sideways moves a static surface across
                // the film by an amount trigonometry gives without rendering
                // anything: the quad sits at z = 0, four units ahead, and the
                // half-width of the frame there is 4 * tanHalfFov.
                const float step = 0.1f;
                hdclaude::FrameDescription moved = still;
                moved.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                moved.camera.cameraToWorld[12] = step;
                // The view translation enters the clip matrix's last column.
                moved.camera.worldToClip[12] -=
                    step * moved.camera.worldToClip[0];
                const hdclaude::FrameResult after =
                    tracer.EndFrame(tracer.BeginFrame(moved));

                const std::size_t centre =
                    (static_cast<std::size_t>(kHeight / 2) * kWidth) +
                    kWidth / 2;
                const double halfWidth = 4.0 * 0.5;  // distance * tanHalfFov
                const double expectedX =
                    step / halfWidth * (kWidth * 0.5);
                const double measuredX = after.motion[centre * 2 + 0];
                const double measuredY = after.motion[centre * 2 + 1];
                // The surface moves *right* on the film when the camera steps
                // left, so the history of this pixel lies to its right.
                CHECK(std::abs(measuredX - expectedX) < 0.05 * expectedX + 0.05);
                CHECK(std::abs(measuredY) < 1e-2);
                std::printf("  motion: still %.2e, stepped %.3f px, expected "
                            "%.3f\n",
                            worstStill, measuredX, expectedX);
            }

            // --- The frame's jitter -----------------------------------------
            //
            // A reconstruction backend is handed one sample and must be told
            // where in the pixel it landed, so an interactive frame reports
            // the offset it actually used. A reference frame has no single
            // offset -- it jitters each of its samples independently -- and
            // reports none rather than an average that describes nothing.
            {
                hdclaude::FrameDescription jitterFrame = description;
                jitterFrame.mode = hdclaude::RenderMode::Reference;
                jitterFrame.settings.resetAccumulation = true;
                const hdclaude::FrameResult reference =
                    tracer.EndFrame(tracer.BeginFrame(jitterFrame));
                CHECK_EQ(reference.jitter[0], 0.0f);
                CHECK_EQ(reference.jitter[1], 0.0f);

                // Interactive frames each report their own offset, inside the
                // pixel and not all the same.
                jitterFrame.mode = hdclaude::RenderMode::Interactive;
                std::vector<std::pair<float, float>> offsets;
                for (int i = 0; i < 8; ++i) {
                    const hdclaude::FrameResult interactive =
                        tracer.EndFrame(tracer.BeginFrame(jitterFrame));
                    CHECK(interactive.jitter[0] >= -0.5f &&
                          interactive.jitter[0] <= 0.5f);
                    CHECK(interactive.jitter[1] >= -0.5f &&
                          interactive.jitter[1] <= 0.5f);
                    offsets.emplace_back(interactive.jitter[0],
                                         interactive.jitter[1]);
                }
                // No two of eight repeat. A sequence that repeated would put
                // a standing pattern into whatever reconstructs it.
                for (std::size_t i = 0; i < offsets.size(); ++i) {
                    for (std::size_t j = i + 1; j < offsets.size(); ++j) {
                        CHECK(offsets[i] != offsets[j]);
                    }
                }
                std::printf("  jitter: reference none, interactive "
                            "(%.3f %.3f) (%.3f %.3f) ...\n",
                            offsets[0].first, offsets[0].second,
                            offsets[1].first, offsets[1].second);
            }

            // --- The depth guide -------------------------------------------
            //
            // Depth is checkable in closed form, which is the only kind of
            // check worth having for it: a plane at a known distance, seen
            // through a known projection, has one normalised device depth and
            // it can be computed rather than compared against a previous run.
            {
                hdclaude::FrameDescription depthFrame = description;
                depthFrame.width = kWidth;
                depthFrame.height = kHeight;
                depthFrame.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                depthFrame.settings.resetAccumulation = true;
                depthFrame.settings.firstSample = 0;
                const hdclaude::FrameResult depthResult =
                    tracer.EndFrame(tracer.BeginFrame(depthFrame));
                CHECK(depthResult.Valid());
                CHECK_EQ(depthResult.depth.size(),
                         std::size_t(kWidth) * kHeight);

                // The quad sits at z = 0 and fills the middle of the frame.
                const std::size_t centre =
                    (static_cast<std::size_t>(kHeight / 2) * kWidth) + kWidth / 2;
                const double expected = ExpectedNdcDepth(0.0f, 4.0f, 0.1f, 100.0f);
                const double measured = depthResult.depth[centre];
                CHECK(std::abs(measured - expected) < 1e-4);

                // A corner sees nothing, and a ray that hit nothing is at the
                // far plane rather than at zero: reporting zero there would put
                // the background in front of everything.
                CHECK_EQ(depthResult.depth[0], 1.0f);

                // Every value is inside the range the AOV is defined over.
                double lowest = 1.0;
                double highest = 0.0;
                for (const float value : depthResult.depth) {
                    CHECK(value >= 0.0f && value <= 1.0f);
                    lowest = std::min(lowest, double(value));
                    highest = std::max(highest, double(value));
                }
                CHECK(lowest < 1.0);   // something was hit
                CHECK_EQ(highest, 1.0);  // and something was not
                std::printf("  depth: centre %.6f, expected %.6f, range %.3f..%.3f\n",
                            measured, expected, lowest, highest);
            }

            // --- Reconstruction ---------------------------------------------
            //
            // Phase 13's gate, which has two halves.
            //
            // The half that holds on every machine: a reference render is
            // bit-identical whether or not a backend exists
            // (docs/dlss-integration.md 6). It is checked here by rendering one
            // before anything has created a backend, creating one by asking for
            // an interactive frame, and rendering the same reference frame
            // again -- which is the strongest form of the claim available
            // inside one process, because the second render happens with NGX
            // initialised and the backend alive rather than merely compiled in.
            // The comparison is bit-for-bit over the film, not by tolerance:
            // "untouched" has no epsilon.
            //
            // The half that needs the hardware: an interactive frame asking to
            // be reconstructed comes back at the extent it asked for, from an
            // estimator that traced a smaller one. Skipped, loudly, on a build
            // or a machine without DLSS -- and even then the extents and the
            // honesty of `reconstructed` are still checked, because a frame
            // that cannot be reconstructed must still be a frame.
            {
                hdclaude::FrameDescription referenceFrame = description;
                referenceFrame.mode = hdclaude::RenderMode::Reference;
                referenceFrame.width = kWidth;
                referenceFrame.height = kHeight;
                referenceFrame.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                referenceFrame.settings.resetAccumulation = true;
                referenceFrame.settings.firstSample = 0;
                // Asked for, and ignored, because this is a reference frame.
                // A caller that renders both modes from one settings struct
                // should not have to remember to clear it.
                referenceFrame.settings.reconstruct = true;
                referenceFrame.settings.reconstructionQuality =
                    hdclaude::ReconstructionQuality::Performance;

                const hdclaude::FrameResult before =
                    tracer.EndFrame(tracer.BeginFrame(referenceFrame));
                CHECK(before.Valid());
                CHECK(!before.reconstructed);
                CHECK(before.reconstructionBackend.empty());
                CHECK_EQ(before.width, kWidth);
                CHECK_EQ(before.renderWidth, kWidth);
                // Nothing has been created yet, which is the point: a gallery
                // render must not initialise NGX.
                CHECK_EQ(std::string(tracer.ReconstructionBackendName()),
                         std::string());

                constexpr std::uint32_t kOutputWidth = 256;
                constexpr std::uint32_t kOutputHeight = 256;

                hdclaude::FrameDescription interactive = referenceFrame;
                interactive.mode = hdclaude::RenderMode::Interactive;
                interactive.width = kOutputWidth;
                interactive.height = kOutputHeight;
                interactive.settings.samplesPerPixel = 1;
                interactive.settings.maxBounces = 2;

                const hdclaude::FrameResult upscaled =
                    tracer.EndFrame(tracer.BeginFrame(interactive));
                CHECK(upscaled.Valid());
                // Whatever happened, the result describes itself: the image is
                // exactly as large as the extents it reports, and the guides
                // are exactly as large as the extents that were traced.
                CHECK_EQ(upscaled.image.size(),
                         std::size_t(upscaled.width) * upscaled.height * 4);
                CHECK_EQ(upscaled.depth.size(),
                         std::size_t(upscaled.renderWidth) * upscaled.renderHeight);
                CHECK_EQ(upscaled.motion.size(),
                         std::size_t(upscaled.renderWidth) * upscaled.renderHeight * 2);

                if (!upscaled.reconstructed) {
                    // No backend: the frame is the one that would have been
                    // rendered without one, at the size it was asked for, and
                    // the renderer says why rather than leaving it to be
                    // guessed at.
                    CHECK_EQ(upscaled.width, kOutputWidth);
                    CHECK_EQ(upscaled.renderWidth, kOutputWidth);
                    CHECK(upscaled.reconstructionBackend.empty());
                    CHECK(!tracer.ReconstructionUnavailable().empty());
                    std::printf("  reconstruction . skipped: %s\n",
                                tracer.ReconstructionUnavailable().c_str());
                } else {
                    CHECK_EQ(upscaled.width, kOutputWidth);
                    CHECK_EQ(upscaled.height, kOutputHeight);
                    // Performance upscales, so the estimator traced fewer
                    // pixels than were returned. This is the assertion that
                    // separates reconstruction from a copy.
                    CHECK(upscaled.renderWidth < kOutputWidth);
                    CHECK(upscaled.renderHeight < kOutputHeight);
                    CHECK(!upscaled.reconstructionBackend.empty());

                    // The image is finite everywhere and is an image of
                    // something: a backend that produced a black frame, or one
                    // that produced NaNs, passes every extent check above.
                    double brightest = 0.0;
                    bool finite = true;
                    for (std::size_t i = 0; i < upscaled.image.size(); i += 4) {
                        for (int c = 0; c < 3; ++c) {
                            const float value = upscaled.image[i + std::size_t(c)];
                            finite = finite && std::isfinite(value);
                            brightest = std::max(brightest, double(value));
                        }
                        CHECK_EQ(upscaled.image[i + 3], 1.0f);
                    }
                    CHECK(finite);
                    CHECK(brightest > 0.0);

                    std::printf("  reconstruction . %s, %ux%u -> %ux%u, "
                                "brightest %.4f\n",
                                upscaled.reconstructionBackend.c_str(),
                                upscaled.renderWidth, upscaled.renderHeight,
                                upscaled.width, upscaled.height, brightest);

                    // DLAA is the same backend asked for no upscaling at all,
                    // and it answers its own output extent as its render
                    // extent. It also rebuilds the feature and the images,
                    // which is the path a quality-mode change takes.
                    hdclaude::FrameDescription native = interactive;
                    native.settings.reconstructionQuality =
                        hdclaude::ReconstructionQuality::NativeResolution;
                    const hdclaude::FrameResult dlaa =
                        tracer.EndFrame(tracer.BeginFrame(native));
                    CHECK(dlaa.reconstructed);
                    CHECK_EQ(dlaa.renderWidth, kOutputWidth);
                    CHECK_EQ(dlaa.renderHeight, kOutputHeight);
                    CHECK_EQ(dlaa.image.size(),
                             std::size_t(kOutputWidth) * kOutputHeight * 4);
                    // Changing what the extents mean is a history reset even
                    // though the camera never moved.
                    CHECK(dlaa.historyReset);

                    // Ray Reconstruction, the same backend building its other
                    // feature -- fed the reconstruction guides, which exist
                    // only because MaterialX closures report them. Asserted
                    // the way the upscale above is, and then the reference
                    // below is compared after it too, which is phase 14's
                    // gate: a reference render is untouched by it.
                    hdclaude::FrameDescription denoised = native;
                    denoised.settings.reconstructionModel =
                        hdclaude::ReconstructionModel::RayReconstruction;
                    const hdclaude::FrameResult rr =
                        tracer.EndFrame(tracer.BeginFrame(denoised));
                    if (!rr.reconstructed) {
                        std::printf("  ray reconstruction . not run: %s\n",
                                    tracer.ReconstructionUnavailable().c_str());
                    }
                    CHECK(rr.reconstructed);
                    CHECK(rr.historyReset);
                    CHECK_EQ(rr.image.size(),
                             std::size_t(kOutputWidth) * kOutputHeight * 4);
                    double rrBrightest = 0.0;
                    bool rrFinite = true;
                    for (std::size_t i = 0; i < rr.image.size(); i += 4) {
                        for (int c = 0; c < 3; ++c) {
                            const float value = rr.image[i + std::size_t(c)];
                            rrFinite = rrFinite && std::isfinite(value);
                            rrBrightest = std::max(rrBrightest, double(value));
                        }
                    }
                    CHECK(rrFinite);
                    CHECK(rrBrightest > 0.0);
                    std::printf("  ray reconstruction . %s, %ux%u -> %ux%u, "
                                "brightest %.4f\n",
                                rr.reconstructionBackend.c_str(), rr.renderWidth,
                                rr.renderHeight, rr.width, rr.height,
                                rrBrightest);
                }

                // And the reference frame again, now that a backend either
                // exists or has been proven not to.
                const hdclaude::FrameResult after =
                    tracer.EndFrame(tracer.BeginFrame(referenceFrame));
                CHECK(after.Valid());
                CHECK(!after.reconstructed);
                CHECK_EQ(after.image.size(), before.image.size());
                const bool identical =
                    after.image.size() == before.image.size() &&
                    std::memcmp(after.image.data(), before.image.data(),
                                before.image.size() * sizeof(float)) == 0;
                CHECK(identical);

                // The other half of "present and absent" is across two builds,
                // which no single process can compare. So the film's bytes are
                // hashed and printed: the build with the SDK and the build
                // without it print the same number, or the claim is false. FNV
                // over the raw bytes, because the claim is about bytes.
                std::uint64_t filmHash = 1469598103934665603ull;
                const auto* bytes =
                    reinterpret_cast<const unsigned char*>(after.image.data());
                for (std::size_t i = 0; i < after.image.size() * sizeof(float);
                     ++i) {
                    filmHash = (filmHash ^ bytes[i]) * 1099511628211ull;
                }
                std::printf("  reference is bit-identical with a backend "
                            "%s: %s (film hash %016llx)\n",
                            tracer.ReconstructionBackendName()[0] != '\0'
                                ? "alive"
                                : "absent",
                            identical ? "yes" : "NO",
                            static_cast<unsigned long long>(filmHash));
            }

            // Nor a camera that moved, which this decision did not previously
            // include. An accumulated film is an average of one integral and a
            // moved camera makes it an average of two, so a caller that moves
            // the camera and forgets to say so must still be overruled.
            description.mode = hdclaude::RenderMode::Reference;
            (void)tracer.EndFrame(tracer.BeginFrame(description));
            description.camera = LookDownZ(5.0f);
            hdclaude::FrameResult moved =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(moved.accumulationReset);

            // And a camera that then stays put may continue again, so the test
            // above is not passing merely because everything resets.
            hdclaude::FrameResult still =
                tracer.EndFrame(tracer.BeginFrame(description));
            CHECK(!still.accumulationReset);

            // History reset is the *subset* a reconstructor must discard on,
            // and the camera is exactly what separates the two. Moving it
            // restarts the reference accumulation and does not invalidate a
            // history that motion vectors can carry forward; a resize, a scene
            // change or a mode switch leaves nothing to carry.
            CHECK(!moved.historyReset);
            CHECK(!still.historyReset);
            CHECK(resized.historyReset);
            CHECK(revised.historyReset);
            CHECK(switched.historyReset);

            std::printf("  frames: %llu..%llu, resets on resize %s, scene %s, "
                        "mode %s\n",
                        static_cast<unsigned long long>(first.index),
                        static_cast<unsigned long long>(switched.index),
                        resized.accumulationReset ? "yes" : "no",
                        revised.accumulationReset ? "yes" : "no",
                        switched.accumulationReset ? "yes" : "no");
            std::printf("  camera move resets accumulation %s, history %s\n",
                        moved.accumulationReset ? "yes" : "no",
                        moved.historyReset ? "yes" : "no");

            // A handle whose frame has already been taken yields nothing, not
            // the frame before it. Handing back a stale image is the failure
            // this whole shape exists to make impossible, so it must not be
            // reachable by asking twice either.
            hdclaude::FrameHandle handle = tracer.BeginFrame(description);
            CHECK(tracer.EndFrame(handle).Valid());
            CHECK(!tracer.EndFrame(handle).Valid());
            CHECK(!tracer.EndFrame(hdclaude::FrameHandle{}).Valid());
        }

        // --- A chain of segments is the capsule it describes -----------------
        //
        // Four segments laid end to end along one line, with one radius
        // throughout, describe exactly the same solid as a single segment
        // spanning the whole of it. So the two must render the same surface,
        // and the depth AOV is where to ask: it is geometry with no lighting in
        // it, so a difference here cannot be blamed on a normal or a shadow.
        //
        // This exists because the implicit curve path drew a crescent at every
        // joint (roadmap open question 5), and the argument about whether that
        // was geometry or shading needed an instrument rather than a picture.
        {
            constexpr std::uint32_t kSize = 192;
            constexpr float kRadius = 0.25f;

            const auto depthOf = [&](const std::vector<float>& segments,
                                     std::uint64_t revision) {
                Scene scene;
                MeshPrototype strand;
                strand.debugName = "chain";
                strand.segments = segments;
                scene.prototypes.push_back(strand);
                scene.instances.push_back({0, Transform3x4{}, 0, true});
                tracer.SetScene(scene, {materials[1]});

                hdclaude::FrameDescription frame;
                frame.width = kSize;
                frame.height = kSize;
                frame.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                frame.mode = hdclaude::RenderMode::Reference;
                frame.sceneRevision = revision;
                frame.settings.samplesPerPixel = 1;
                frame.settings.maxBounces = 1;
                return tracer.EndFrame(tracer.BeginFrame(frame)).depth;
            };

            // One segment from y = -1 to y = 1.
            const std::vector<float> single = {
                0.0f, -1.0f, 0.0f, kRadius, 0.0f,
                0.0f,  1.0f, 0.0f, kRadius, 1.0f,
            };
            // The same span in four, joined end to end.
            std::vector<float> chain;
            for (int i = 0; i < 4; ++i) {
                const float y0 = -1.0f + 0.5f * float(i);
                const float y1 = y0 + 0.5f;
                const float v0 = 0.25f * float(i);
                chain.insert(chain.end(),
                             {0.0f, y0, 0.0f, kRadius, v0,
                              0.0f, y1, 0.0f, kRadius, v0 + 0.25f});
            }

            const std::vector<float> one = depthOf(single, 51);
            const std::vector<float> four = depthOf(chain, 52);
            CHECK_EQ(one.size(), std::size_t(kSize) * kSize);
            CHECK_EQ(four.size(), one.size());

            std::size_t differing = 0;
            double worst = 0.0;
            for (std::size_t i = 0; i < one.size(); ++i) {
                const double delta = std::abs(double(one[i]) - double(four[i]));
                if (delta > 1.0e-5) {
                    ++differing;
                    worst = std::max(worst, delta);
                }
            }
            std::printf("  curve chain: %zu of %zu depths differ, worst %.6f\n",
                        differing, one.size(), worst);
            // Exactly nothing, because the two descriptions are of one solid.
            //
            // This was 656 of 36,864, worst 0.00163, from the day curves were
            // intersected, and was the crescent at every joint (roadmap open
            // question 5). The cause was that `extend` generated a hit without
            // asking whether it was nearer than the one already committed. At a
            // joint a ray meets the neighbouring segment's end sphere *behind*
            // the surface it already found, and the driver committed that
            // farther hit whenever its box was visited later. Vulkan's closest
            // hit determination says a confirmed hit with t > t_max is dropped;
            // this driver did not, and the kernel now makes the comparison
            // itself. Reverting that check brings back exactly 656.
            CHECK_EQ(differing, std::size_t(0));
        }

        // --- The primary surface's reconstruction guides ----------------------
        //
        // What the integrator writes for Ray Reconstruction, checked against
        // what is known without rendering: a quad turned 0.6 rad about Y has
        // the normal (sin 0.6, 0, cos 0.6); white diffuse reports its colour as
        // diffuse albedo, no specular albedo and a roughness of one; a
        // dielectric of alpha 0.3 reports no diffuse albedo, some specular
        // albedo and a linear roughness of sqrt(0.3); and a pixel that misses
        // holds NVIDIA's sky defaults. One bounce, deliberately: that is the
        // render in which the scattering pass never runs, so a guide read from
        // it would be missing.
        {
            constexpr std::uint32_t kSize = 64;
            const float angle = 0.6f;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            Transform3x4 rotation;
            rotation.m[0] = c;  rotation.m[2] = s;
            rotation.m[8] = -s; rotation.m[10] = c;

            const CompiledMaterial glossy = MakeDielectricMaterial(
                libraries, compiler, tracer.ShadeKernelSource(), 1.5f,
                "guide.glossy", 0.3f);
            CHECK(!glossy.spirv.empty());

            const auto guidesOf = [&](const CompiledMaterial& material,
                                      std::uint64_t revision) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, rotation, 0, true});
                tracer.SetScene(scene, {material});

                hdclaude::FrameDescription frame;
                frame.width = kSize;
                frame.height = kSize;
                frame.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                frame.mode = hdclaude::RenderMode::Reference;
                frame.sceneRevision = revision;
                frame.settings.samplesPerPixel = 1;
                frame.settings.maxBounces = 1;
                return tracer.EndFrame(tracer.BeginFrame(frame));
            };

            const std::size_t centre = (kSize / 2) * kSize + kSize / 2;
            const std::size_t corner = 0;

            const hdclaude::FrameResult diffuse = guidesOf(materials[0], 61);
            CHECK_EQ(diffuse.normalRoughness.size(), std::size_t(kSize) * kSize * 4);
            CHECK_EQ(diffuse.diffuseAlbedo.size(), std::size_t(kSize) * kSize * 3);
            CHECK_EQ(diffuse.specularAlbedo.size(), std::size_t(kSize) * kSize * 3);
            if (diffuse.normalRoughness.size() == std::size_t(kSize) * kSize * 4) {
                const float* n = &diffuse.normalRoughness[centre * 4];
                std::printf("  guides, diffuse: normal (%.5f, %.5f, %.5f) against"
                            " (%.5f, 0, %.5f), roughness %.3f, albedo %.3f / %.3f\n",
                            n[0], n[1], n[2], s, c, n[3],
                            diffuse.diffuseAlbedo[centre * 3],
                            diffuse.specularAlbedo[centre * 3]);
                CHECK_NEAR(n[0], s, 1.0e-4);
                CHECK_NEAR(n[1], 0.0f, 1.0e-4);
                CHECK_NEAR(n[2], c, 1.0e-4);
                CHECK_NEAR(n[3], 1.0f, 1.0e-6);
                for (int k = 0; k < 3; ++k) {
                    CHECK_NEAR(diffuse.diffuseAlbedo[centre * 3 + k], 0.8f, 1.0e-5);
                    CHECK_EQ(diffuse.specularAlbedo[centre * 3 + k], 0.0f);
                }

                const float* sky = &diffuse.normalRoughness[corner * 4];
                CHECK_EQ(sky[0], 0.0f);
                CHECK_EQ(sky[1], 0.0f);
                CHECK_EQ(sky[2], 0.0f);
                CHECK_EQ(sky[3], 0.0f);
                for (int k = 0; k < 3; ++k) {
                    CHECK_EQ(diffuse.diffuseAlbedo[corner * 3 + k], 0.5f);
                    CHECK_EQ(diffuse.specularAlbedo[corner * 3 + k], 0.0f);
                }
            }

            const hdclaude::FrameResult shiny = guidesOf(glossy, 62);
            if (shiny.normalRoughness.size() == std::size_t(kSize) * kSize * 4) {
                const float* n = &shiny.normalRoughness[centre * 4];
                std::printf("  guides, dielectric: roughness %.4f against %.4f,"
                            " albedo %.4f / %.4f\n",
                            n[3], std::sqrt(0.3), shiny.diffuseAlbedo[centre * 3],
                            shiny.specularAlbedo[centre * 3]);
                CHECK_NEAR(n[0], s, 1.0e-4);
                CHECK_NEAR(n[2], c, 1.0e-4);
                CHECK_NEAR(n[3], std::sqrt(0.3f), 1.0e-5);
                CHECK_EQ(shiny.diffuseAlbedo[centre * 3], 0.0f);
                CHECK(shiny.specularAlbedo[centre * 3] > 0.0f);
                CHECK(shiny.specularAlbedo[centre * 3] < 1.0f);
            }
        }

        // --- The specular hit distance guide -----------------------------------
        //
        // A mirror turned 45 degrees about Y, seen straight down -Z, reflects
        // the view to exactly +X; a wall facing it at x = 1.5 is then 1.5 away
        // along the specular probe from the mirror's centre, which is where the
        // centre pixel's primary surface is. The wall is edge-on to the camera,
        // so it cannot be the primary surface of that pixel. A pixel that
        // misses everything holds FP16_MAX, and a surface whose reflection
        // meets nothing does too.
        {
            constexpr std::uint32_t kSize = 64;
            constexpr float kWall = 1.5f;
            const float quarter = 0.25f * 3.14159265f;
            Transform3x4 mirror;
            mirror.m[0] = std::cos(quarter);   mirror.m[2] = std::sin(quarter);
            mirror.m[8] = -std::sin(quarter);  mirror.m[10] = std::cos(quarter);
            // A quad turned -90 degrees about Y faces -X, and moved to x = 1.5.
            Transform3x4 wall;
            wall.m[0] = 0.0f;  wall.m[2] = -1.0f;  wall.m[3] = kWall;
            wall.m[8] = 1.0f;  wall.m[10] = 0.0f;

            const auto hitDistanceOf = [&](bool withWall, std::uint64_t revision) {
                Scene scene;
                scene.prototypes.push_back(MakeQuad());
                scene.instances.push_back({0, mirror, 0, true});
                if (withWall) {
                    scene.instances.push_back({0, wall, 0, true});
                }
                tracer.SetScene(scene, {materials[0]});
                hdclaude::FrameDescription frame;
                frame.width = kSize;
                frame.height = kSize;
                frame.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                frame.mode = hdclaude::RenderMode::Reference;
                frame.sceneRevision = revision;
                frame.settings.samplesPerPixel = 1;
                frame.settings.maxBounces = 1;
                return tracer.EndFrame(tracer.BeginFrame(frame)).specularHitDistance;
            };

            const std::size_t centre = (kSize / 2) * kSize + kSize / 2;
            const std::vector<float> walled = hitDistanceOf(true, 71);
            const std::vector<float> open = hitDistanceOf(false, 72);
            CHECK_EQ(walled.size(), std::size_t(kSize) * kSize);
            CHECK_EQ(open.size(), std::size_t(kSize) * kSize);
            if (walled.size() == std::size_t(kSize) * kSize &&
                open.size() == walled.size()) {
                std::printf("  specular hit distance: %.5f against %.5f, open "
                            "%.1f, miss %.1f\n",
                            walled[centre], kWall, open[centre], walled[0]);
                // Pixel 32 spans x in [0, 4/64] on the mirror, which is where
                // its probe starts, and a probe starting at x reaches the wall
                // after 1.5 - x. So the distance lies in [1.5 - 1/16, 1.5],
                // plus the ray offset and the few thousandths by which a
                // perspective ray is not exactly -Z.
                CHECK(walled[centre] > kWall - 0.0625f - 0.005f);
                CHECK(walled[centre] < kWall + 0.005f);
                CHECK_EQ(open[centre], 65504.0f);
                CHECK_EQ(walled[0], 65504.0f);
            }
        }

        // --- A shadow ray is not stopped by a curve beyond its light ----------
        //
        // The other half of the same fault. A shadow ray runs from a surface to
        // a point on the light and ends there, so a curve beyond the light
        // cannot shade the surface -- but its box can begin before the light,
        // and then the traversal hands it over as a candidate whose real hit
        // lies past the end of the ray. Generating that hit is outside the
        // ray's interval, which the driver shown above does not reliably drop.
        //
        // The geometry is chosen so that the claim is exact rather than
        // approximate. A diffuse quad at z = 0 is lit by a rect light off to
        // its side at z = 1.5, and every shadow ray therefore ends on the plane
        // z = 1.5, inside the bundle x in [-1 + 5z/3, 1 + 5z/3]. The comb's
        // teeth descend from (2, y, 2.8) to (4, y, 1.2): below z = 1.5 they are
        // at x > 3.6, where no shadow ray is, so no ray can truly meet them --
        // yet each tooth's box reaches down to z = 1.185, across the rays'
        // path. With the ray's interval honoured the comb changes nothing.
        //
        // The control is what stops this passing vacuously: a grate of the same
        // teeth laid flat across the bundle at z = 1.2 must darken the quad,
        // so a renderer whose curves cast no shadow at all fails here. Both
        // are outside the camera's view, which at z >= 1.2 is under a unit
        // wide, so only the quad's lighting is measured.
        {
            constexpr std::uint32_t kSize = 64;
            constexpr float kTooth = 0.015f;

            Light aside;
            aside.type = static_cast<std::uint32_t>(LightType::Rect);
            aside.position[0] = 2.5f;
            aside.position[1] = 0.0f;
            aside.position[2] = 1.5f;
            aside.direction[0] = 0.0f;
            aside.direction[1] = 0.0f;
            aside.direction[2] = -1.0f;
            aside.uAxis[0] = 1.0f; aside.uAxis[1] = 0.0f; aside.uAxis[2] = 0.0f;
            aside.vAxis[0] = 0.0f; aside.vAxis[1] = 1.0f; aside.vAxis[2] = 0.0f;
            aside.area = 4.0f;
            aside.radiance[0] = 6.0f;
            aside.radiance[1] = 6.0f;
            aside.radiance[2] = 6.0f;
            aside.castsShadows = 1;

            const auto meanLit = [&](const std::vector<float>& segments) {
                Scene lit;
                lit.prototypes.push_back(MakeQuad());
                lit.instances.push_back({0, Transform3x4{}, 0, true});
                if (!segments.empty()) {
                    MeshPrototype teeth;
                    teeth.debugName = "comb";
                    teeth.segments = segments;
                    lit.prototypes.push_back(teeth);
                    lit.instances.push_back({1, Transform3x4{}, 0, true});
                }
                lit.lights.push_back(aside);
                lit.environmentColor[0] = 0.0f;
                lit.environmentColor[1] = 0.0f;
                lit.environmentColor[2] = 0.0f;
                tracer.SetScene(lit, {materials[1]});

                RenderSettings settings;
                settings.samplesPerPixel = 64;
                settings.maxBounces = 1;
                settings.environmentColor[0] = 0.0f;
                settings.environmentColor[1] = 0.0f;
                settings.environmentColor[2] = 0.0f;
                settings.lightGeometry = false;
                const std::vector<float> image =
                    tracer.Render(kSize, kSize, LookDownZ(3.0f), settings);
                double total = 0.0;
                const std::size_t pixels = image.size() / 4;
                for (std::size_t i = 0; i < pixels; ++i) {
                    total += 0.2126 * image[i * 4 + 0] +
                             0.7152 * image[i * 4 + 1] +
                             0.0722 * image[i * 4 + 2];
                }
                return pixels > 0 ? total / double(pixels) : 0.0;
            };

            std::vector<float> beyond;
            std::vector<float> across;
            for (int i = 0; i <= 60; ++i) {
                const float y = -1.2f + 0.04f * float(i);
                beyond.insert(beyond.end(), {2.0f, y, 2.8f, kTooth, 0.0f,
                                             4.0f, y, 1.2f, kTooth, 1.0f});
            }
            for (int i = 0; i <= 50; ++i) {
                const float x = 1.0f + 0.04f * float(i);
                across.insert(across.end(), {x, -1.5f, 1.2f, kTooth, 0.0f,
                                             x,  1.5f, 1.2f, kTooth, 1.0f});
            }

            const double open = meanLit({});
            const double behind = meanLit(beyond);
            const double blocked = meanLit(across);
            std::printf("  curve beyond a light: open %.4f, comb beyond %.4f "
                        "(ratio %.4f), grate across %.4f (ratio %.4f)\n",
                        open, behind, open > 0.0 ? behind / open : 0.0, blocked,
                        open > 0.0 ? blocked / open : 0.0);
            CHECK(open > 0.0);
            // Two per cent is the estimator's own disagreement between two
            // renders at this sample count; without the guard this read 0.9105.
            CHECK_NEAR(behind / open, 1.0, 0.02);
            CHECK(blocked / open < 0.5);
        }

        // --- A curve is intersected, not tessellated -------------------------
        //
        // The whole implicit path end to end: an acceleration structure of
        // boxes, a candidate handled in the traversal kernel, a round cone
        // intersected there, and a surface rebuilt from the segment in `shade`.
        // Every one of those can fail by simply producing no hit, and the scene
        // then renders as though the curve were not there -- which is exactly
        // what it did until the structure's move constructor was taught to
        // carry the segment buffer.
        //
        // Measured on the depth AOV rather than on the image, because the claim
        // is about where the geometry *is*: a shading difference could hide a
        // silhouette that is the wrong width, and a depth of less than one is a
        // ray that hit something whatever it was shaded as.
        //
        // The closed form is the silhouette: a cylinder of radius r seen
        // side-on covers exactly 2r of a view 2 * distance * tanHalfFov wide.
        // An intersector with the radius squared, halved or ignored lands
        // nowhere near it.
        {
            constexpr std::uint32_t kSize = 128;
            constexpr float kRadius = 0.25f;
            constexpr float kDistance = 4.0f;

            Scene curves;
            MeshPrototype strand;
            strand.debugName = "capsule";
            // One vertical segment through the origin with equal radii, so the
            // round cone is a cylinder and the closed form is exact.
            strand.segments = {
                0.0f, -1.0f, 0.0f, kRadius, 0.0f,
                0.0f,  1.0f, 0.0f, kRadius, 1.0f,
            };
            curves.prototypes.push_back(strand);
            curves.instances.push_back({0, Transform3x4{}, 0, true});
            tracer.SetScene(curves, {materials[1]});

            hdclaude::FrameDescription frame;
            frame.width = kSize;
            frame.height = kSize;
            frame.camera = LookDownZWithClip(kDistance, 0.1f, 100.0f);
            frame.mode = hdclaude::RenderMode::Reference;
            frame.sceneRevision = 41;
            frame.settings.samplesPerPixel = 1;
            frame.settings.maxBounces = 1;
            const hdclaude::FrameResult result =
                tracer.EndFrame(tracer.BeginFrame(frame));
            CHECK(result.Valid());
            CHECK_EQ(result.depth.size(), std::size_t(kSize) * kSize);

            const std::size_t row = kSize / 2;
            std::size_t covered = 0;
            for (std::size_t x = 0; x < kSize; ++x) {
                if (result.depth[row * kSize + x] < 1.0f) {
                    ++covered;
                }
            }

            const double halfExtent = kDistance * 0.5;  // LookDownZ's tanHalfFov
            const double expected =
                double(kSize) * (2.0 * kRadius) / (2.0 * halfExtent);
            std::printf("  implicit curve: %zu of %u pixels across, closed "
                        "form %.1f\n",
                        covered, kSize, expected);
            CHECK(covered > 0);
            CHECK(double(covered) > expected - 2.0);
            CHECK(double(covered) < expected + 2.0);

            // And nothing at the edges: a box reported as a hit without the
            // cone being tested would still be only its own width, so this is
            // about the structure holding what it should rather than the
            // intersector.
            CHECK_EQ(result.depth[row * kSize + 2], 1.0f);
            CHECK_EQ(result.depth[row * kSize + kSize - 3], 1.0f);
        }

        // --- Light geometry, and what turning it off may not cost -------------
        //
        // Two claims, because the setting makes two promises and only one of
        // them is about the picture.
        //
        // The shape goes: with a light facing the camera and nothing else in
        // the scene, the frame is exactly black when its geometry is off and is
        // not when it is on. Exactly black, not nearly: there is no other
        // emitter, the environment is zero, and a single sample that found the
        // light would show.
        //
        // The light does not go: a surface lit by that light reads the same
        // whether or not the shape is rendered. This is the claim that would
        // fail if the mixture density had been left alone. A hidden light
        // cannot be found by a scattered ray, so next-event estimation is the
        // only strategy left and must take the contribution whole; weighting it
        // against a ray that can no longer arrive would make every hidden light
        // too dark by exactly the share it gave away.
        {
            constexpr std::uint32_t kSize = 96;

            // A rect light square to the camera, four units ahead of it.
            Light facing;
            facing.type = static_cast<std::uint32_t>(LightType::Rect);
            facing.position[0] = 0.0f;
            facing.position[1] = 0.0f;
            facing.position[2] = 0.0f;
            facing.direction[0] = 0.0f;
            facing.direction[1] = 0.0f;
            facing.direction[2] = 1.0f;
            facing.uAxis[0] = 1.0f; facing.uAxis[1] = 0.0f; facing.uAxis[2] = 0.0f;
            facing.vAxis[0] = 0.0f; facing.vAxis[1] = 1.0f; facing.vAxis[2] = 0.0f;
            facing.area = 4.0f;
            facing.radiance[0] = 3.0f;
            facing.radiance[1] = 3.0f;
            facing.radiance[2] = 3.0f;
            facing.castsShadows = 1;

            const auto meanOfImage = [](const std::vector<float>& image) {
                double total = 0.0;
                const std::size_t pixels = image.size() / 4;
                for (std::size_t i = 0; i < pixels; ++i) {
                    total += 0.2126 * image[i * 4 + 0] +
                             0.7152 * image[i * 4 + 1] +
                             0.0722 * image[i * 4 + 2];
                }
                return pixels > 0 ? total / double(pixels) : 0.0;
            };

            // Nothing but the light, and a black sky, so the only thing a ray
            // can find is the shape under test.
            {
                Scene empty;
                empty.lights.push_back(facing);
                empty.environmentColor[0] = 0.0f;
                empty.environmentColor[1] = 0.0f;
                empty.environmentColor[2] = 0.0f;
                tracer.SetScene(empty, {});

                RenderSettings shown;
                shown.samplesPerPixel = 8;
                shown.maxBounces = 1;
                shown.environmentColor[0] = 0.0f;
                shown.environmentColor[1] = 0.0f;
                shown.environmentColor[2] = 0.0f;
                shown.lightGeometry = true;
                const std::vector<float> visible =
                    tracer.Render(kSize, kSize, LookDownZ(4.0f), shown);

                RenderSettings hidden = shown;
                hidden.lightGeometry = false;
                const std::vector<float> gone =
                    tracer.Render(kSize, kSize, LookDownZ(4.0f), hidden);

                const double withShape = meanOfImage(visible);
                const double withoutShape = meanOfImage(gone);
                std::printf("  light geometry: on %.4f, off %.6f\n", withShape,
                            withoutShape);
                CHECK(withShape > 0.0);
                CHECK_EQ(withoutShape, 0.0);
            }

            // And now the half that is about transport rather than the picture.
            // The light is turned to face a diffuse quad and moved out of the
            // frame, so the only thing either render can show is the surface it
            // lights.
            {
                Scene lit;
                lit.prototypes.push_back(MakeQuad());
                lit.instances.push_back({0, Transform3x4{}, 0, true});
                // Off to the side and out of the frame, still square to the
                // quad so it lights it well. The camera is at z = 3 with a
                // half-angle whose tangent is 0.414, so the frustum is 0.62
                // wide at z = 1.5 and this light -- spanning x from 1.5 to 3.5
                // -- is entirely outside it. That matters: a light *in* frame
                // would put its own pixels into the comparison and the
                // measurement would be about the shape rather than about the
                // light, which is exactly the mistake this comment exists to
                // stop being made again.
                Light aside = facing;
                aside.position[0] = 2.5f;
                aside.position[1] = 0.0f;
                aside.position[2] = 1.5f;
                aside.direction[0] = 0.0f;
                aside.direction[1] = 0.0f;
                aside.direction[2] = -1.0f;
                aside.radiance[0] = 6.0f;
                aside.radiance[1] = 6.0f;
                aside.radiance[2] = 6.0f;
                lit.lights.push_back(aside);
                lit.environmentColor[0] = 0.0f;
                lit.environmentColor[1] = 0.0f;
                lit.environmentColor[2] = 0.0f;
                tracer.SetScene(lit, {materials[1]});

                RenderSettings shown;
                shown.samplesPerPixel = 256;
                shown.maxBounces = 2;
                shown.environmentColor[0] = 0.0f;
                shown.environmentColor[1] = 0.0f;
                shown.environmentColor[2] = 0.0f;
                shown.lightGeometry = true;
                const std::vector<float> visible =
                    tracer.Render(kSize, kSize, LookDownZ(3.0f), shown);

                RenderSettings hidden = shown;
                hidden.lightGeometry = false;
                const std::vector<float> gone =
                    tracer.Render(kSize, kSize, LookDownZ(3.0f), hidden);

                const double withShape = meanOfImage(visible);
                const double withoutShape = meanOfImage(gone);
                const double ratio =
                    withShape > 0.0 ? withoutShape / withShape : 0.0;
                std::printf("  light geometry, lit surface: on %.4f, off %.4f "
                            "(ratio %.4f)\n",
                            withShape, withoutShape, ratio);
                CHECK(withShape > 0.0);
                // Two per cent, which is the two estimators' own disagreement
                // at this sample count and nothing like the share the balance
                // heuristic would have taken.
                CHECK_NEAR(ratio, 1.0, 0.02);
            }
        }

        // --- DLAA against the converged reference ----------------------------
        //
        // The other half of phase 13's gate, and the half that says whether the
        // reconstruction is any *good* rather than merely plumbed in. Two
        // measurements, because either alone can be satisfied by a failure.
        //
        // SSIM against the converged reference says the reconstructed frame is
        // a better picture of the truth than the noisy frame it was made from.
        // RMS could not ask this: a reconstructor moves every pixel a little
        // and is meant to, and RMS cannot tell that apart from a picture that
        // fell apart.
        //
        // Temporal instability says the sequence does not boil. A reconstructor
        // could score well on every still frame and still flicker, because
        // being close to the reference on each frame says nothing about being
        // close to the frame before it -- and flicker is the artefact a
        // temporal method actually produces when it is wrong.
        //
        // The comparison is against the *same estimator at one sample*, not
        // against a fixed number, so the claim is "reconstruction improved
        // this" rather than "reconstruction reached a threshold somebody chose".
        // A threshold would be a tolerance to tune; this cannot be tuned
        // without making the renderer worse.
        //
        // Skipped, loudly, on a build or a machine without DLSS.
        {
            Scene scene;
            scene.prototypes.push_back(MakeSphere());
            scene.prototypes.push_back(MakeQuad());
            Transform3x4 behind;
            behind.m[3] = 0.0f;
            behind.m[7] = 0.0f;
            behind.m[11] = -1.5f;
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.instances.push_back({1, behind, 1, true});

            // A rect light, so there is a shadow edge and a specular highlight
            // -- structure for the metric to be about. A furnace would give it
            // nothing to measure.
            Light rect;
            rect.type = static_cast<std::uint32_t>(LightType::Rect);
            rect.position[0] = 0.0f;
            rect.position[1] = 2.5f;
            rect.position[2] = 1.5f;
            rect.direction[0] = 0.0f;
            rect.direction[1] = -1.0f;
            rect.direction[2] = 0.0f;
            rect.uAxis[0] = 1.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
            rect.vAxis[0] = 0.0f; rect.vAxis[1] = 0.0f; rect.vAxis[2] = 1.0f;
            rect.area = 4.0f;
            rect.radiance[0] = 4.0f;
            rect.radiance[1] = 4.0f;
            rect.radiance[2] = 4.0f;
            rect.castsShadows = 1;
            scene.lights.push_back(rect);

            tracer.SetScene(scene, {materials[0], materials[1]});

            constexpr std::uint32_t kDlaaWidth = 256;
            constexpr std::uint32_t kDlaaHeight = 256;
            constexpr int kSequence = 24;

            hdclaude::FrameDescription converged;
            converged.width = kDlaaWidth;
            converged.height = kDlaaHeight;
            converged.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
            converged.mode = hdclaude::RenderMode::Reference;
            converged.sceneRevision = 7;
            converged.settings.samplesPerPixel = 512;
            converged.settings.maxBounces = 4;
            converged.settings.resetAccumulation = true;

            const hdclaude::FrameResult reference =
                tracer.EndFrame(tracer.BeginFrame(converged));
            CHECK(reference.Valid());

            // One sample per frame, the same estimator and the same path
            // length as the reference: an interactive preview that changed the
            // estimator would be measuring two different integrals against each
            // other (docs/architecture.md 5).
            bool reconstructed = false;
            const auto sequence = [&](bool reconstruct,
                                      hdclaude::ReconstructionModel model =
                                          hdclaude::ReconstructionModel::
                                              SuperResolution) {
                hdclaude::FrameDescription frame = converged;
                frame.mode = hdclaude::RenderMode::Interactive;
                frame.settings.samplesPerPixel = 1;
                frame.settings.reconstruct = reconstruct;
                frame.settings.reconstructionModel = model;
                frame.settings.reconstructionQuality =
                    hdclaude::ReconstructionQuality::NativeResolution;

                std::vector<std::vector<float>> frames;
                frames.reserve(kSequence);
                for (int i = 0; i < kSequence; ++i) {
                    // Each frame is its own single sample, decorrelated from
                    // the last by the sample index -- which is what an
                    // interactive host does, and what leaves the reconstructor
                    // rather than the film to do the averaging.
                    frame.settings.firstSample = static_cast<std::uint32_t>(i);
                    frame.settings.resetAccumulation = true;
                    const hdclaude::FrameResult result =
                        tracer.EndFrame(tracer.BeginFrame(frame));
                    CHECK_EQ(result.width, kDlaaWidth);
                    if (reconstruct) {
                        reconstructed = result.reconstructed;
                    }
                    frames.push_back(std::move(result.image));
                }
                return frames;
            };

            const std::vector<std::vector<float>> noisy = sequence(false);
            const std::vector<std::vector<float>> clean = sequence(true);

            if (!reconstructed) {
                std::printf("  DLAA quality ... skipped: %s\n",
                            tracer.ReconstructionUnavailable().c_str());
            } else {
                const double ssimNoisy =
                    hdclaude::Ssim(noisy.back().data(), reference.image.data(),
                                   kDlaaWidth, kDlaaHeight);
                const double ssimClean =
                    hdclaude::Ssim(clean.back().data(), reference.image.data(),
                                   kDlaaWidth, kDlaaHeight);

                // The last eight frames of each, by which point DLSS has a
                // history to be unstable with. Measuring from the first frame
                // would mostly measure the reset.
                const std::vector<std::vector<float>> tailNoisy(
                    noisy.end() - 8, noisy.end());
                const std::vector<std::vector<float>> tailClean(
                    clean.end() - 8, clean.end());
                const double flickerNoisy = hdclaude::TemporalInstability(
                    tailNoisy, kDlaaWidth, kDlaaHeight);
                const double flickerClean = hdclaude::TemporalInstability(
                    tailClean, kDlaaWidth, kDlaaHeight);

                // What fraction of the light survived. A reconstructor is not
                // an estimator and owes no unbiasedness, but the number is
                // worth carrying: DLSS rejects outliers against a
                // neighbourhood, a one-sample path trace is largely outliers,
                // and this is where the cost of feeding a raster-trained model
                // path-traced noise actually shows up. Reported here, and
                // *asserted* further down where the input is nearly converged
                // and there is no longer anything legitimate to reject.
                const auto meanLuminance = [](const std::vector<float>& image) {
                    double total = 0.0;
                    const std::size_t pixels = image.size() / 4;
                    for (std::size_t i = 0; i < pixels; ++i) {
                        total += 0.2126 * image[i * 4 + 0] +
                                 0.7152 * image[i * 4 + 1] +
                                 0.0722 * image[i * 4 + 2];
                    }
                    return pixels > 0 ? total / double(pixels) : 0.0;
                };
                const double convergedMean = meanLuminance(reference.image);
                const double keptAtOneSample =
                    convergedMean > 0.0
                        ? meanLuminance(clean.back()) / convergedMean
                        : 0.0;

                std::printf("  DLAA quality ... ssim %.4f against %.4f "
                            "unreconstructed; flicker %.4f against %.4f; "
                            "energy kept %.1f%% at one sample a frame\n",
                            ssimClean, ssimNoisy, flickerClean, flickerNoisy,
                            100.0 * keptAtOneSample);

                // Both claims, and neither is a threshold: the reconstructed
                // frame is a better picture of the converged reference than the
                // frame it was made from, and the reconstructed sequence is
                // steadier than the sequence it was made from.
                CHECK(ssimClean > ssimNoisy);
                CHECK(flickerClean < flickerNoisy);

                // And it is a picture of *something*: a backend that returned
                // the reference itself would pass both tests above and be
                // wrong, so the reconstruction must still be short of the
                // converged image it is estimating.
                CHECK(ssimClean < 1.0);

                // Ray Reconstruction over the same one-sample sequence, held
                // to the same two claims against the same unreconstructed
                // frames -- still not thresholds. It is the model built for
                // this input, so how it compares with Super Resolution is
                // printed beside it: that is a statement about NVIDIA's
                // models, which this suite measures and does not assert.
                const std::vector<std::vector<float>> denoised = sequence(
                    true, hdclaude::ReconstructionModel::RayReconstruction);
                const double ssimDenoised =
                    hdclaude::Ssim(denoised.back().data(), reference.image.data(),
                                   kDlaaWidth, kDlaaHeight);
                const std::vector<std::vector<float>> tailDenoised(
                    denoised.end() - 8, denoised.end());
                const double flickerDenoised = hdclaude::TemporalInstability(
                    tailDenoised, kDlaaWidth, kDlaaHeight);
                const double keptDenoised =
                    convergedMean > 0.0
                        ? meanLuminance(denoised.back()) / convergedMean
                        : 0.0;
                std::printf("  RR quality ..... ssim %.4f against %.4f "
                            "unreconstructed and %.4f super resolution; "
                            "flicker %.4f; energy kept %.1f%% at one sample a "
                            "frame\n",
                            ssimDenoised, ssimNoisy, ssimClean, flickerDenoised,
                            100.0 * keptDenoised);
                CHECK(ssimDenoised > ssimNoisy);
                CHECK(flickerDenoised < flickerNoisy);
                CHECK(ssimDenoised < 1.0);
            }

            // --- Which way the sub-pixel offset points ----------------------
            //
            // hdClaude's jitter says where the sample landed; DLSS's says how
            // the projection was offset, and the two are the same displacement
            // seen from opposite ends (docs/dlss-integration.md 5). Getting it
            // backwards does not break the image, which is why it survived this
            // long: over a Halton sequence the errors are symmetric about the
            // pixel centre and show up as softening, which nothing here can
            // tell from the softening a reconstructor legitimately produces.
            //
            // Hold the offset still and it stops being a blur and becomes a
            // *displacement*, of twice the offset: DLSS resolves its history at
            // the pixel centre by shifting it by the jitter it was told, and a
            // reversed sign shifts it the wrong way by exactly as much as the
            // samples were already displaced. At an offset of half a pixel that
            // is a whole pixel.
            //
            // The frames are traced at many samples each, so the input is
            // nearly converged and the only thing left for the measurement to
            // be about is where the picture sits. A one-sample sequence would
            // bury the shift under its own noise: at one sample a frame the
            // reversal moves the DLAA SSIM above by 0.004, which is nothing.
            if (reconstructed) {
                constexpr std::uint32_t kSignWidth = 256;
                constexpr std::uint32_t kSignHeight = 256;
                constexpr std::uint32_t kSignFrames = 8;

                hdclaude::FrameDescription converged;
                converged.width = kSignWidth;
                converged.height = kSignHeight;
                converged.camera = LookDownZWithClip(4.0f, 0.1f, 100.0f);
                converged.mode = hdclaude::RenderMode::Reference;
                converged.sceneRevision = 9;
                converged.settings.samplesPerPixel = 512;
                converged.settings.maxBounces = 4;

                const hdclaude::FrameResult centred =
                    tracer.EndFrame(tracer.BeginFrame(converged));
                CHECK(centred.Valid());

                hdclaude::FrameDescription frame = converged;
                frame.mode = hdclaude::RenderMode::Interactive;
                frame.settings.samplesPerPixel = 128;
                frame.settings.reconstruct = true;
                frame.settings.reconstructionQuality =
                    hdclaude::ReconstructionQuality::NativeResolution;
                // Half a pixel on each axis, so a reversed sign displaces the
                // picture by a whole pixel on each axis -- as far from correct
                // as a sub-pixel offset can put it.
                frame.settings.fixedJitter = true;
                frame.settings.jitter[0] = 0.5f;
                frame.settings.jitter[1] = 0.5f;

                hdclaude::FrameResult held;
                for (std::uint32_t i = 0; i < kSignFrames; ++i) {
                    frame.settings.firstSample = i;
                    held = tracer.EndFrame(tracer.BeginFrame(frame));
                }
                CHECK(held.reconstructed);
                // The offset was the caller's and is reported back as given,
                // rather than being replaced by the sequence.
                CHECK_EQ(held.jitter[0], 0.5f);
                CHECK_EQ(held.jitter[1], 0.5f);

                // Where the reconstruction sits relative to the converged
                // render, in pixels. Not SSIM: a sphere and a backdrop lit by
                // one light have almost no detail for a shift to disturb, and
                // SSIM over this scene reads 0.6675 aligned against 0.6655 a
                // whole pixel out -- a difference of two parts in a thousand,
                // which would have "passed" whichever sign was handed over.
                // A displacement is what is in question, so a displacement is
                // what is measured (docs/dlss-integration.md 5).
                const hdclaude::ImageShift placement = hdclaude::EstimateShift(
                    centred.image.data(), held.image.data(), kSignWidth,
                    kSignHeight);
                CHECK(placement.valid);

                std::printf("  jitter sign ... reconstruction sits %.4f px "
                            "from the converged render (%+.4f %+.4f); "
                            "reversing the sign puts it at 1.39\n",
                            placement.Magnitude(), placement.dx, placement.dy);

                // A quarter of a pixel, which is nowhere near either answer:
                // the offset was half a pixel on each axis, so reversing it
                // displaces the picture by a whole pixel on each axis, and
                // there is no tolerance between the two that could be tuned to
                // make a wrong sign pass. Measured: 0.1302 px as it stands,
                // and 1.3934 px with the sign handed to NGX reversed -- close
                // to the 1.4142 a whole pixel on each axis would be, short of
                // it because DLSS's resolve is not a pure translation.
                CHECK(placement.Magnitude() < 0.25);

                // And the light is still there.
                //
                // These frames are traced at many samples each, so there is
                // almost no variance left for an outlier rejector to reject,
                // and a reconstruction of an image that is already converged
                // must not change how much light is in it. This is the
                // assertion that catches the plumbing failing -- a pre-exposure
                // dropped, a format that cannot carry the range, a packing
                // kernel scaling what it copies -- none of which the
                // displacement above or the structure before it would notice.
                //
                // The bound is loose against what it measures on purpose. About
                // 3% goes here and on the gallery's gold shader ball, which is
                // DLSS still rejecting the little variance that remains; a
                // tenth is well clear of that and far below the 30% a
                // one-sample sequence loses, so it separates "reconstructing"
                // from "losing the image" without being a number tuned to pass.
                const auto meanOf = [](const std::vector<float>& image) {
                    double total = 0.0;
                    const std::size_t pixels = image.size() / 4;
                    for (std::size_t i = 0; i < pixels; ++i) {
                        total += 0.2126 * image[i * 4 + 0] +
                                 0.7152 * image[i * 4 + 1] +
                                 0.0722 * image[i * 4 + 2];
                    }
                    return pixels > 0 ? total / double(pixels) : 0.0;
                };
                const double truth = meanOf(centred.image);
                const double retained =
                    truth > 0.0 ? meanOf(held.image) / truth : 0.0;
                std::printf("  reconstruction energy ... %.1f%% of the "
                            "converged mean at %u samples a frame\n",
                            100.0 * retained, frame.settings.samplesPerPixel);
                CHECK(retained > 0.90);
                CHECK(retained < 1.10);
            }
        }

        // --- The renderer reproduces its own output --------------------------
        //
        // Every "byte identical" claim in this project's record rests on this,
        // and until 2026-09-08 nothing checked it. What prompted the check was
        // the New Zealand height map -- one textured quad -- moving against its
        // committed baseline by an RMS of 3.7e-5 in a gallery run, and then
        // matching that same baseline exactly on the two runs after it, with
        // the same binary throughout.
        //
        // The gate's RMS limit is 0.01 and that noise is two orders below it,
        // so the gallery has never failed on it and never will. What it costs
        // is worse than a failure: a genuine change of that size becomes
        // indistinguishable from nothing having happened, which is exactly the
        // argument every remaining step of phase 9 depends on.
        //
        // Three things this is careful about.
        //
        // It compares the *films*, not display images. The gallery's own gate
        // compares display-transformed JPEGs, and anything the transform
        // normalises never reaches it -- which is the same blindness that hid
        // the OpenPBR Playground's 2.18e25 for as long as that image existed.
        //
        // It renders *progressively*, in chunks that accumulate, because that
        // is what the gallery does and what an interactive host will do. A
        // single one-shot render exercises neither the continuation path nor
        // the repeated compaction across it.
        //
        // And it repeats, because one identical pair proves nothing about
        // something that is usually identical. The height map compared clean
        // twice in a row while being demonstrably not reproducible.
        {
            Scene scene;
            scene.prototypes.push_back(MakeSphere());
            scene.prototypes.push_back(MakeQuad());
            // Two prototypes on two materials, so the per-material sort has
            // more than one group to scatter into -- the compaction and the
            // sort are where the order paths take through a frame stops being
            // fixed, and a single-material scene would not exercise either.
            Transform3x4 behind;
            behind.m[3] = 0.0f;
            behind.m[7] = 0.0f;
            behind.m[11] = -1.5f;
            scene.instances.push_back({0, Transform3x4{}, 0, true});
            scene.instances.push_back({1, behind, 1, true});

            Light rect;
            rect.type = static_cast<std::uint32_t>(LightType::Rect);
            rect.position[0] = 0.0f;
            rect.position[1] = 2.5f;
            rect.position[2] = 1.5f;
            rect.direction[0] = 0.0f;
            rect.direction[1] = -1.0f;
            rect.direction[2] = 0.0f;
            rect.uAxis[0] = 1.0f; rect.uAxis[1] = 0.0f; rect.uAxis[2] = 0.0f;
            rect.vAxis[0] = 0.0f; rect.vAxis[1] = 0.0f; rect.vAxis[2] = 1.0f;
            rect.area = 4.0f;
            rect.radiance[0] = 4.0f;
            rect.radiance[1] = 4.0f;
            rect.radiance[2] = 4.0f;
            // Shadow rays are a second queue filled by an atomic, so a scene
            // without them would leave half the ordering untested.
            rect.castsShadows = 1;
            scene.lights.push_back(rect);

            tracer.SetScene(scene, {materials[0], materials[1]});

            constexpr std::uint32_t kChunks = 4;
            constexpr std::uint32_t kChunkSamples = 8;
            constexpr int kRepeats = 6;

            const auto renderProgressively = [&]() {
                RenderSettings chunk;
                // Lights are probed through their geometry here, and read through a
                // mirror in several of these; the render default is off.
                chunk.lightGeometry = true;
                chunk.maxBounces = 6;
                chunk.samplesPerPixel = kChunkSamples;
                std::vector<float> image;
                for (std::uint32_t c = 0; c < kChunks; ++c) {
                    chunk.firstSample = c * kChunkSamples;
                    chunk.resetAccumulation = (c == 0);
                    image = tracer.Render(kWidth, kHeight, LookDownZ(4.0f), chunk);
                }
                return image;
            };

            const std::vector<float> reference = renderProgressively();
            CHECK(!reference.empty());

            int differingRuns = 0;
            double worst = 0.0;
            std::size_t worstIndex = 0;
            for (int repeat = 0; repeat < kRepeats; ++repeat) {
                const std::vector<float> again = renderProgressively();
                CHECK_EQ(again.size(), reference.size());
                bool differs = false;
                for (std::size_t i = 0; i < again.size(); ++i) {
                    // Exact, not near. Two runs of the same arithmetic on the
                    // same inputs have no tolerance to be within: any
                    // difference at all is an ordering that was not fixed, and
                    // a tolerance here would re-create the blindness this
                    // exists to remove.
                    const double delta =
                        std::abs(double(again[i]) - double(reference[i]));
                    if (delta > 0.0) {
                        differs = true;
                        if (delta > worst) {
                            worst = delta;
                            worstIndex = i;
                        }
                    }
                }
                if (differs) {
                    ++differingRuns;
                }
            }

            std::printf("  determinism: %d of %d repeats differ from the first"
                        "; worst %.3e at component %zu\n",
                        differingRuns, kRepeats, worst, worstIndex);

            CHECK_EQ(differingRuns, 0);

            // The same again, with the scene *republished* between renders.
            //
            // Publishing rebuilds the acceleration structures, and a GPU
            // builder is under no obligation to produce the same tree
            // twice. A different tree should still give the same closest
            // hit -- the nearest intersection along a ray is unique,
            // whatever order a traversal finds it in -- so this asks
            // whether that holds in practice.
            //
            // It is the discriminator the loop above cannot be: that one
            // never rebuilds anything, so it cannot tell "the renderer is
            // not reproducible" from "the structure it traverses is not".
            // Across processes a real scene *is* irreproducible -- three
            // runs of the chess set give three different films, worst
            // pixel 1.22 -- and the scene store's ordering and the
            // generated MaterialX are both already ruled out, so what is
            // rebuilt per process is what is left to suspect.
            int rebuildDiffering = 0;
            double rebuildWorst = 0.0;
            for (int repeat = 0; repeat < kRepeats; ++repeat) {
                tracer.SetScene(scene, {materials[0], materials[1]});
                const std::vector<float> again = renderProgressively();
                CHECK_EQ(again.size(), reference.size());
                bool differs = false;
                for (std::size_t i = 0; i < again.size(); ++i) {
                    const double delta =
                        std::abs(double(again[i]) - double(reference[i]));
                    if (delta > 0.0) {
                        differs = true;
                        rebuildWorst = std::max(rebuildWorst, delta);
                    }
                }
                if (differs) {
                    ++rebuildDiffering;
                }
            }

            std::printf("  determinism across a scene rebuild: %d of %d "
                        "differ; worst %.3e\n",
                        rebuildDiffering, kRepeats, rebuildWorst);

            CHECK_EQ(rebuildDiffering, 0);
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
        // Findings inside NVIDIA's runtime, which this cannot fix, said every
        // run so that they do not become a thing nobody knows about.
        const std::uint64_t ngxErrors = context->ThirdPartyValidationErrorCount();
        std::printf("  validation ..... %llu errors, %llu inside NGX%s%s\n",
                    static_cast<unsigned long long>(errors),
                    static_cast<unsigned long long>(ngxErrors),
                    ngxErrors != 0 ? " -- last: " : "",
                    ngxErrors != 0
                        ? context->LastThirdPartyValidationError().c_str()
                        : "");
    } catch (const std::exception& error) {
        // Reported rather than left to terminate: a kernel that fails to
        // compile should name itself, not abort with a status code.
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }

    return hdclaude_test::Summarize("hdClaudeRenderTests");
}
