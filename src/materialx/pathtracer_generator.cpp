#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/Shader.h>
// MaterialX 1.39.5 moved the hardware nodes into MaterialXGenHw; Houdini 22.0
// ships that layout, a standalone OpenUSD 26.03 the earlier one.
#if __has_include(<MaterialXGenHw/Nodes/HwImageNode.h>)
#include <MaterialXGenHw/Nodes/HwImageNode.h>
#else
#include <MaterialXGenShader/Nodes/HwImageNode.h>
#endif
#include <MaterialXGenShader/ShaderGraph.h>
#include <MaterialXGenShader/ShaderStage.h>

#include <MaterialXFormat/XmlIo.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>

namespace hdclaude {
namespace {

using namespace MaterialX;

/// The generated code's globals. These are the ABI between a compiled
/// MaterialX program and the wavefront `shade` kernel: the kernel fills
/// `hdclaude_hit`, calls the entry point, and reads the three results.
///
/// Globals rather than out-parameters because MaterialX's node implementations
/// write into variables by name from several places in the emitted body, and
/// threading out-parameters through them would mean overriding more node
/// implementations than the design needs. In a compute shader a global is
/// per-invocation, so this is a naming convention, not shared state.
constexpr const char* kBsdfGlobal = "hdclaude_bsdf";
constexpr const char* kDisplacementGlobal = "hdclaude_displacement";

/// The thin-film Airy series length, on a MaterialX that has the option.
///
/// From 1.39.5 the stock `mx_microfacet_specular.glsl` loops to
/// `AIRY_FRESNEL_ITERATIONS`, which GlslShaderGenerator::emitPixelStage
/// defines from `GenOptions::hwAiryFresnelIterations`. This target owns the
/// pixel stage, so it defines it too -- but only where the option exists, as
/// older MaterialX (OpenUSD 26.03's) has neither the field nor the loop. A
/// template, so a MaterialX without the member is a discarded branch rather
/// than a compile error.
template <typename Options>
std::optional<unsigned int> AiryFresnelIterations(const Options& options)
{
    if constexpr (requires { options.hwAiryFresnelIterations; }) {
        return static_cast<unsigned int>(options.hwAiryFresnelIterations);
    } else {
        return std::nullopt;
    }
}

/// Turns off the BSDF graph rewrites, on a MaterialX that has them.
///
/// From 1.39.5 HwShaderGenerator::applyDefaultOptions enables
/// `premultipliedBsdfAdd`, which replaces every `mix` of BSDFs whose weight is
/// connected with `add(multiply(fg, w), multiply(bg, 1 - w))`, so that a
/// rasteriser can branch over a zero-weight lobe. The response is the same, but
/// the throughput a `layer` reads from its top is not: hdClaude's `add` sums
/// it, and a leaf folded to weight zero reports one, so open_pbr's specular
/// layer passed its base twice the light (implementation-notes.md,
/// 2026-10-23). `mix` also selects a lobe by its authored weight, where `add`
/// knows no weights and selects evenly. `distributeLayerOverBsdfMix`, off by
/// default, rewrites the graph too. The closures are written against the
/// graph as authored, as 1.39.3 emits it. A template, so a MaterialX without
/// the members is a discarded branch.
template <typename Options>
void KeepAuthoredBsdfGraph(Options& options)
{
    if constexpr (requires { options.premultipliedBsdfAdd; }) {
        options.premultipliedBsdfAdd = false;
    }
    if constexpr (requires { options.distributeLayerOverBsdfMix; }) {
        options.distributeLayerOverBsdfMix = false;
    }
}

/// Whether this graph produces a displacement rather than a surface.
///
/// Asked of the output socket's type rather than of the node's classification:
/// MaterialX classifies the `displacement` constructor as a shader node, the
/// same as `surface`, and the two are told apart by what they output.
bool IsDisplacementGraph(const mx::ShaderGraph& graph)
{
    for (mx::ShaderGraphOutputSocket* socket : graph.getOutputSockets()) {
        if (socket->getType() == mx::Type::DISPLACEMENTSHADER) {
            return true;
        }
    }
    return false;
}

/// Whether this graph is the opacity library rather than a material.
bool IsOpacityLibraryGraph(const mx::ShaderGraph& graph)
{
    for (mx::ShaderGraphOutputSocket* socket : graph.getOutputSockets()) {
        if (socket->getName() == kOpacityLibraryOutput) {
            return true;
        }
    }
    return false;
}

/// The node in the opacity library whose output is material `m`'s opacity is
/// named this prefix followed by `m`. See OpacityLibraryDocument.
constexpr const char* kOpacityRootPrefix = "hdclaude_opacity_m";
constexpr const char* kEmissionGlobal = "hdclaude_emission";
constexpr const char* kOpacityGlobal = "hdclaude_opacity";

/// True when a vertex-data port is a `geompropvalue` node reading the UV set.
///
/// The primvar name is compared *exactly*, after the `geomprop_` prefix, and
/// the port must be a two-component one. A substring test looks equivalent and
/// is not: `geomprop_strand_u` contains `geomprop_st`, so a curve's float
/// parameter was assigned a `vec2` and the material stopped compiling.
bool IsUvGeomProp(const string& loweredVariable, const ShaderPort* port)
{
    if (port == nullptr || port->getType() != Type::VECTOR2) {
        return false;
    }
    const string marker = "geomprop_";
    const std::size_t at = loweredVariable.find(marker);
    if (at == string::npos) {
        return false;
    }
    const string primvar = loweredVariable.substr(at + marker.size());
    return primvar == "st" || primvar == "uv";
}

/// True when a vertex-data port is a `geompropvalue` node reading displayColor.
///
/// Matched exactly, for the reason IsUvGeomProp gives, and as a three-component
/// port: `displayColor` is a `color3f[]` primvar, and a graph may read it as a
/// `color3` or a `vector3`.
bool IsDisplayColorGeomProp(const string& loweredVariable, const ShaderPort* port)
{
    if (port == nullptr ||
        (port->getType() != Type::COLOR3 && port->getType() != Type::VECTOR3)) {
        return false;
    }
    const string marker = "geomprop_";
    const std::size_t at = loweredVariable.find(marker);
    return at != string::npos &&
           loweredVariable.substr(at + marker.size()) == "displaycolor";
}

}  // namespace

// ---------------------------------------------------------------------------
// PathTracerSyntax
// ---------------------------------------------------------------------------

PathTracerSyntax::PathTracerSyntax(TypeSystemPtr typeSystem) : VkSyntax(typeSystem)
{
    // The extended struct and the literal that default-constructs it. They are
    // written once and used by both registrations below, because a type and its
    // default value drifting apart is a miscompile rather than an error.
    //
    // `isDelta` is a float so this stays a plain aggregate literal. Field
    // meanings are documented in
    // mtlx/pbrlib/genglsl_pt/lib/mx_closure_type.glsl, and the two files must
    // be changed together.
    static const string kBsdfDefault =
        "BSDF(vec3(0.0),vec3(1.0),vec4(0.0),vec3(0.0),0.0,0.0,vec3(0.0),"
        "vec3(0.0),vec3(0.0),0.0,vec3(0.0),vec3(0.0),0.0,0.0)";
    static const string kBsdfDefinition =
        "struct BSDF {\n"
        "    vec3  response;\n"
        "    vec3  throughput;\n"
        "    vec4  spectrum;\n"
        "    vec3  sampledL;\n"
        "    float pdf;\n"
        "    float isDelta;\n"
        "    vec3  guideDiffuse;\n"
        "    vec3  guideSpecular;\n"
        "    vec3  guideNormal;\n"
        "    float guideRoughness;\n"
        "    vec3  mediumExtinction;\n"
        "    vec3  mediumAlbedo    ;\n"
        "    float mediumAnisotropy;\n"
        "    float mediumKind   ;\n"
        "};";

    registerTypeSyntax(Type::BSDF,
                       std::make_shared<AggregateTypeSyntax>(
                           this, "BSDF", kBsdfDefault, EMPTY_STRING,
                           EMPTY_STRING, kBsdfDefinition));

    // A filename is a handle, not a sampler.
    //
    // MaterialX's GLSL syntax makes `filename` a `sampler2D`, which can carry
    // exactly one image. A UDIM set is many images behind one `<image>` node,
    // and which of them a sample reads is decided from that sample's own
    // texture coordinate -- so the node needs the whole set, and a sampler
    // cannot express it. The handle carries the set's first index into the
    // shared texture array and where its tile numbers live in the table the
    // material declares; `tileCount` is one for an ordinary image and zero for
    // an `<image>` with no file, and the hdClaude `mx_image_*` overrides read
    // it.
    //
    // Registered here rather than worked around at the call site because the
    // type has to be consistent everywhere MaterialX might emit it -- a
    // nodegraph that exposes a filename on its interface declares a parameter
    // of this type, and a `sampler2D` there would not match.
    static const string kTextureDefault = "HdclaudeTexture(0,0,1)";
    static const string kTextureDefinition =
        "struct HdclaudeTexture {" "\n"
        "    int slot;        // first index into hdclaude_textures" "\n"
        "    int tileOffset;  // into hdclaude_udim_tiles" "\n"
        "    int tileCount;   // 1 when the image is not a UDIM set, 0 when" "\n"
        "                     // there is no image and the node's default" "\n"
        "                     // is the answer" "\n"
        "};";
    // A scalar syntax, not an aggregate one, and the distinction matters. An
    // aggregate type is one MaterialX will *construct*: it emits
    // `HdclaudeTexture(foo_file)` where the stock sampler type emits
    // `foo_file`, and a one-argument constructor for a three-field struct does
    // not compile. A filename is passed along, never built, which is exactly
    // what the stock `sampler2D` registration is -- so this differs from it
    // only in the name and in carrying a type definition.
    registerTypeSyntax(Type::FILENAME,
                       std::make_shared<ScalarTypeSyntax>(
                           this, "HdclaudeTexture", kTextureDefault,
                           EMPTY_STRING, EMPTY_STRING, kTextureDefinition));

    // VDF must be re-registered too, and it is easy to miss.
    //
    // MaterialX 1.39.3 registers VDF as an *alias* of the BSDF struct -- same
    // type name, no definition of its own -- but with a separate copy of the
    // default-value literal. Extending BSDF alone therefore leaves every VDF
    // variable initialised with a two-field literal for an eight-field struct,
    // which surfaces only when a material actually uses a VDF. OpenPBR does;
    // Standard Surface does not, so the first three test materials passed.
    registerTypeSyntax(Type::VDF,
                       std::make_shared<AggregateTypeSyntax>(
                           this, "BSDF", kBsdfDefault, EMPTY_STRING));
}

// ---------------------------------------------------------------------------
// PathTracerSurfaceNode
// ---------------------------------------------------------------------------

ShaderNodeImplPtr PathTracerSurfaceNode::create()
{
    return std::make_shared<PathTracerSurfaceNode>();
}

void PathTracerSurfaceNode::createVariables(const ShaderNode&, GenContext&,
                                            Shader& shader) const
{
    ShaderStage& vs = shader.getStage(Stage::VERTEX);
    ShaderStage& ps = shader.getStage(Stage::PIXEL);

    // Geometry the shade kernel interpolates from the hit record. These become
    // the members of the generated SurfaceHit struct, so registering exactly
    // these and no more is what keeps that struct minimal.
    addStageConnector(HW::VERTEX_DATA, Type::VECTOR3, HW::T_POSITION_WORLD, vs, ps);
    addStageConnector(HW::VERTEX_DATA, Type::VECTOR3, HW::T_NORMAL_WORLD, vs, ps);

    // Deliberately absent, versus the stock implementation:
    //   u_viewPosition        the view direction arrives in closureData.V
    //   lighting uniforms     the light sample arrives in closureData.L
    //   vertex stage inputs   this target emits no vertex stage
}

void PathTracerSurfaceNode::emitFunctionCall(const ShaderNode& node,
                                             GenContext& context,
                                             ShaderStage& stage) const
{
    const HwShaderGenerator& shadergen =
        static_cast<const HwShaderGenerator&>(context.getShaderGenerator());

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        // Declare the node's own surfaceshader output so downstream code that
        // expects it still compiles. The path tracer reads the ABI globals
        // instead; `color` and `transparency` are filled in as a courtesy for
        // anything that inspects the stock fields.
        const ShaderOutput* output = node.getOutput();
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(output, true, true, context, stage);
        shadergen.emitLineEnd(stage);

        shadergen.emitScopeBegin(stage);

        // Opacity, for anything that inspects the stock fields. Nothing shades
        // with it: it is the surface's *cutout* opacity, which decides whether
        // a ray meets the surface at all, and that is answered during traversal
        // by the opacity library (AuthoredCutoutOpacity). A path that reaches
        // this program has already met the surface.
        //
        // emitInput writes the authored value when nothing is connected. This
        // used to write a literal one instead, which dropped a constant opacity
        // set directly on a `surface` node.
        const ShaderInput* opacityInput = node.getInput("opacity");
        if (opacityInput)
        {
            shadergen.emitLineBegin(stage);
            shadergen.emitString(string(kOpacityGlobal) + " = ", stage);
            shadergen.emitInput(opacityInput, context, stage);
            shadergen.emitLineEnd(stage);
        }

        // The BSDF. Evaluated against the caller's `closureData`, which is a
        // parameter of the generated entry point and therefore already in
        // scope. That single substitution is what turns a rasterisation graph
        // into a path-tracing one: the closure type, the direction, and the
        // light sample all come from the integrator.
        const ShaderInput* bsdfInput = node.getInput("bsdf");
        if (bsdfInput)
        {
            if (const ShaderNode* bsdf = bsdfInput->getConnectedSibling())
            {
                shadergen.emitComment("hdClaude: evaluate the BSDF against the caller's closure data", stage);
                shadergen.emitFunctionCall(*bsdf, context, stage);
                shadergen.emitLine(string(kBsdfGlobal) + " = " +
                                       bsdf->getOutput()->getVariable(), stage);
                shadergen.emitLineBreak(stage);
            }
        }

        // The EDF. Emission needs no direction sampling, so it is simply
        // evaluated; the caller decides whether to ask for it by the closure
        // type it passes.
        const ShaderInput* edfInput = node.getInput("edf");
        if (edfInput)
        {
            if (const ShaderNode* edf = edfInput->getConnectedSibling())
            {
                shadergen.emitComment("hdClaude: evaluate the EDF", stage);
                shadergen.emitFunctionCall(*edf, context, stage);
                shadergen.emitLine(string(kEmissionGlobal) + " = " +
                                       edf->getOutput()->getVariable(), stage);
                shadergen.emitLineBreak(stage);
            }
        }

        // Stock surfaceshader fields, for compatibility only.
        shadergen.emitLine(output->getVariable() + ".color = " + kBsdfGlobal +
                               ".response + " + kEmissionGlobal, stage);
        shadergen.emitLine(output->getVariable() + ".transparency = vec3(1.0 - " +
                               kOpacityGlobal + ")", stage);

        shadergen.emitScopeEnd(stage);
        shadergen.emitLineBreak(stage);
    }
}

// ---------------------------------------------------------------------------
// PathTracerShaderGenerator
// ---------------------------------------------------------------------------

const string PathTracerShaderGenerator::TARGET = "genglsl_pt";

PathTracerShaderGenerator::PathTracerShaderGenerator(TypeSystemPtr typeSystem)
    : VkShaderGenerator(typeSystem)
{
    // Replace the syntax with ours, so BSDF carries the path-tracing fields.
    _syntax = PathTracerSyntax::create(typeSystem);

    // Replace the surface node's calling convention. Every other node
    // implementation is inherited: the geometric nodes, the whole of stdlib,
    // and the pbrlib closures resolved from mtlx/pbrlib/genglsl_pt.
    //
    // Registered under the genglsl_pt name because that is what our
    // implementation declaration in hdclaude_pbrlib_impl.mtlx names it.
    registerImplementation("IM_surface_" + TARGET, PathTracerSurfaceNode::create);

    // The image nodes keep MaterialX's own node implementation and change only
    // their GLSL body.
    //
    // `HwImageNode` is not a formality. It *adds* the `uv_scale` and
    // `uv_offset` inputs that the `image` nodedef does not declare and the
    // stock GLSL body takes, so a source-code node registered in its place
    // emits an eleven-argument call against a thirteen-parameter function and
    // fails to compile. Registering it under the genglsl_pt implementation
    // names is what keeps the override to the body alone.
    for (const std::string& type : {"float", "color3", "color4", "vector2",
                                    "vector3", "vector4"})
    {
        registerImplementation("IM_image_" + type + "_" + TARGET,
                               mx::HwImageNode::create);
    }

    // ClosureData is deliberately left byte-compatible with upstream. MaterialX
    // 1.39.3 constructs it inline with a fixed six-argument list and offers no
    // substitution token, so adding fields would break every construction site
    // this generator does not itself replace. The hero wavelengths and the
    // stratified sample travel in per-invocation globals instead; see
    // mtlx/pbrlib/genglsl_pt/lib/mx_closure_type.glsl.
}

ShaderPtr PathTracerShaderGenerator::generate(const string& name,
                                              ElementPtr element,
                                              GenContext& context) const
{
    GenOptions& options = context.getOptions();

    // No prefiltered environment: the indirect closure branches that would use
    // one are dropped, and leaving this enabled adds u_envMatrix,
    // u_envRadiance, u_envIrradiance and their companions to the private
    // uniform block for nothing to read.
    options.hwSpecularEnvironmentMethod = SPECULAR_ENVIRONMENT_NONE;

    // No light loop: a path tracer supplies its own light sample through
    // closureData. Non-zero here also makes MaterialX emit light node
    // implementations that construct their own ClosureData.
    options.hwMaxActiveLightSources = 0;

    // No rasteriser shadow or occlusion maps; visibility comes from shadow rays.
    options.hwShadowMap = false;
    options.hwAmbientOcclusion = false;

    // Analytic directional albedo. The table and Monte Carlo variants both
    // assume a rasteriser's environment, and the table adds two more uniforms.
    options.hwDirectionalAlbedoMethod = DIRECTIONAL_ALBEDO_ANALYTIC;

    // These generate whole alternative shaders, not materials.
    options.hwWriteAlbedoTable = false;
    options.hwWriteEnvPrefilter = false;
    options.hwWriteDepthMoments = false;

    // The graph as authored; see KeepAuthoredBsdfGraph.
    KeepAuthoredBsdfGraph(options);

    return VkShaderGenerator::generate(name, element, context);
}

bool PathTracerShaderGenerator::nodeNeedsClosureData(const ShaderNode& node) const
{
    // Everything upstream threads it through, plus shader and surface nodes.
    return VkShaderGenerator::nodeNeedsClosureData(node) ||
           node.hasClassification(ShaderNode::Classification::SHADER) ||
           node.hasClassification(ShaderNode::Classification::SURFACE);
}

void PathTracerShaderGenerator::emitUniforms(GenContext& context,
                                             ShaderStage& stage) const
{
    _textureOrder.clear();

    // The array is declared here, in the material itself, rather than in
    // path_state.glsl. The shade kernel is *appended* to the generated
    // material, so anything path_state.glsl declares comes after the code that
    // uses it -- the material body samples its textures hundreds of lines
    // before the include would land. No other kernel samples a texture, and a
    // shader need not declare every binding its layout contains.
    size_t textureCount = 0;
    for (const auto& entry : stage.getUniformBlocks())
    {
        const VariableBlock& uniforms = *entry.second;
        if (uniforms.getName() == HW::LIGHT_DATA)
        {
            continue;
        }
        for (ShaderPort* uniform : uniforms.getVariableOrder())
        {
            if (uniform->getType() == Type::FILENAME)
            {
                ++textureCount;
            }
        }
    }

    // The UDIM tile numbers, flattened across every set this material samples.
    //
    // One array for the whole material rather than one per image, because a
    // handle can carry an offset into a shared table and cannot carry an array.
    // Built before the handles are emitted so the offsets are known.
    std::vector<int> udimTable;
    std::map<std::string, std::pair<size_t, size_t>> udimRanges;
    for (const auto& entry : stage.getUniformBlocks())
    {
        const VariableBlock& uniforms = *entry.second;
        if (uniforms.getName() == HW::LIGHT_DATA)
        {
            continue;
        }
        for (ShaderPort* uniform : uniforms.getVariableOrder())
        {
            if (uniform->getType() != Type::FILENAME)
            {
                continue;
            }
            const auto found = _udimTiles.find(uniform->getVariable());
            if (found == _udimTiles.end() || found->second.size() < 2)
            {
                continue;
            }
            udimRanges[uniform->getVariable()] = {udimTable.size(),
                                                  found->second.size()};
            udimTable.insert(udimTable.end(), found->second.begin(),
                             found->second.end());
        }
    }

    if (textureCount > 0)
    {
        emitComment("Shared texture array; see shaders/path_state.glsl", stage);
        emitLine("layout(set = 0, binding = 16) uniform sampler2D "
                 "hdclaude_textures[" + std::to_string(kTextureCapacity) + "]",
                 stage);

        // Always declared, even when nothing here is a UDIM set: the image
        // implementations reference it unconditionally, and a GLSL array
        // cannot have zero elements. One dead entry costs nothing.
        std::string tiles;
        for (const int tile : udimTable)
        {
            tiles += (tiles.empty() ? "" : ", ") + std::to_string(tile);
        }
        if (udimTable.empty())
        {
            tiles = "0";
        }
        emitComment("UDIM tile numbers, indexed by a texture handle's "
                    "tileOffset", stage);
        emitLine("const int hdclaude_udim_tiles[" +
                     std::to_string(udimTable.empty() ? 1 : udimTable.size()) +
                     "] = int[](" + tiles + ")",
                 stage);
        emitLineBreak(stage);
    }

    for (const auto& entry : stage.getUniformBlocks())
    {
        const VariableBlock& uniforms = *entry.second;
        if (uniforms.empty() || uniforms.getName() == HW::LIGHT_DATA)
        {
            continue;
        }

        emitComment("Uniform block " + uniforms.getName() +
                        ", declared without descriptors",
                    stage);

        for (ShaderPort* uniform : uniforms.getVariableOrder())
        {
            if (uniform->getType() == Type::FILENAME)
            {
                // A handle into the shared array, not a descriptor of its own.
                // A UDIM set takes one slot per tile, contiguously and in
                // ascending tile order, and the handle names the first of them
                // together with the range of the tile table that says which
                // tile each slot holds.
                const size_t index = _textureOrder.size();
                const std::string file =
                    uniform->getValue() ? uniform->getValue()->getValueString()
                                        : EMPTY_STRING;

                // An `<image>` with no file. MaterialX defines one as returning
                // its `default`, and assets rely on that: a material that
                // exposes every parameter as "a value, or a map" reads each
                // through an image whose file is usually empty and whose
                // default is the value. The stock genglsl body says so by
                // checking for a 1x1 sampler; a handle says it outright, takes
                // no slot, and leaves a 1x1 *failure* placeholder free to stay
                // magenta. The value is the one MaterialX baked, after
                // following any `dot` in between, so it is empty exactly when
                // MaterialX would sample nothing.
                if (file.empty())
                {
                    emitLine("#define " + uniform->getVariable() +
                                 " HdclaudeTexture(0, 0, 0)",
                             stage, false);
                    continue;
                }

                const auto range = udimRanges.find(uniform->getVariable());
                if (range == udimRanges.end())
                {
                    _textureOrder.push_back({uniform->getVariable(), 0, file});
                    emitLine("#define " + uniform->getVariable() +
                                 " HdclaudeTexture(" + std::to_string(index) +
                                 ", 0, 1)",
                             stage, false);
                    continue;
                }

                const std::vector<int>& tiles =
                    _udimTiles.at(uniform->getVariable());
                for (const int tile : tiles)
                {
                    _textureOrder.push_back({uniform->getVariable(), tile, file});
                }
                emitLine("#define " + uniform->getVariable() +
                             " HdclaudeTexture(" + std::to_string(index) + ", " +
                             std::to_string(range->second.first) + ", " +
                             std::to_string(range->second.second) + ")",
                         stage, false);
                continue;
            }

            // A plain global at its default value. Nothing reads these once
            // SHADER_INTERFACE_REDUCED has baked the values a node uses, but
            // the declarations must exist for the generated code to compile.
            emitLineBegin(stage);
            emitVariableDeclaration(uniform, EMPTY_STRING, context, stage, true);
            emitString(Syntax::SEMICOLON, stage);
            emitLineEnd(stage, false);
        }
        emitLineBreak(stage);
    }

    if (_textureOrder.size() > kTextureCapacity)
    {
        throw mx::ExceptionShaderGenError(
            "This material needs " + std::to_string(_textureOrder.size()) +
            " texture slots; hdClaude's shared array holds " +
            std::to_string(kTextureCapacity) +
            ". A UDIM set takes one slot per tile, so a material with several "
            "sets of many tiles reaches this long before it has that many "
            "images. Raise kTextureCapacity in pathtracer_generator.h and "
            "kHdClaudeTextureCapacity in shaders/path_state.glsl together.");
    }
}

void PathTracerShaderGenerator::emitInputs(GenContext& context,
                                           ShaderStage& stage) const
{
    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        // MaterialX has already worked out exactly which geometric quantities
        // this material reads. Emitting that block as a struct means the
        // generated code declares precisely the geometry it uses, and the
        // `shade` kernel fills precisely that much.
        const VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);

        // The setter's signature is the ABI and does not depend on the
        // document. Only its *body* does.
        const string setter = "void " + string(kSurfaceHitSetter) +
                              "(vec3 P, vec3 N, vec3 T, vec3 B, vec3 Pobj, "
                              "vec3 Nobj, vec3 Tobj, vec3 Bobj, vec2 uv, "
                              "vec3 displayColor)";

        if (vertexData.empty())
        {
            // A material that reads no geometry at all. A surface graph never
            // is one -- the surface node registers the position and the normal
            // itself -- but a *displacement* graph can be: a constant height
            // asks the mesh nothing. The kernel calls the setter regardless of
            // which material it was linked against, so an empty one is emitted
            // rather than none, and the ABI stops depending on what the
            // document happens to read.
            emitComment("This material reads no geometry; the setter is the "
                        "ABI and is kept", stage);
            emitLine(setter, stage, false);
            emitScopeBegin(stage);
            emitScopeEnd(stage);
            emitLineBreak(stage);
            return;
        }

        {
            emitComment("Interpolated geometry supplied by the shade kernel", stage);
            emitLine("struct " + string(kSurfaceHitStruct), stage, false);
            emitScopeBegin(stage);
            emitVariableDeclarations(vertexData, EMPTY_STRING, Syntax::SEMICOLON,
                                     context, stage, false);
            emitScopeEnd(stage, true, false);
            emitLineBreak(stage);

            // Declared under the instance name the generated expressions use,
            // so `vd.normalWorld` resolves without rewriting any node output.
            const string& instance = vertexData.getInstance();
            emitLine(string(kSurfaceHitStruct) + " " + instance, stage);
            emitLineBreak(stage);

            // A setter with exactly the assignments this material needs.
            //
            // The struct's members depend on which geometry the material reads,
            // so a kernel cannot assign them by name without knowing the
            // material -- a diffuse-only material has no tangent, and writing
            // one is a compile error. Emitting the setter here moves that
            // knowledge to the only place that has it.
            emitComment("Filled by the shade kernel; members vary by material", stage);
            emitLine(setter, stage, false);
            emitScopeBegin(stage);
            // Names are matched case-insensitively. A vertex-data variable can
            // reach here either substituted (`i_geomprop_st`) or as the token
            // MaterialX stores it under (`$inGeomprop_st`), and the two differ
            // in case as well as in prefix; matching one spelling silently
            // dropped the other, which is how a `geompropvalue` node reading
            // `st` ended up with a zero coordinate.
            auto lowered = [](const string& text) {
                string result = text;
                std::transform(result.begin(), result.end(), result.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                return result;
            };

            for (std::size_t i = 0; i < vertexData.size(); ++i) {
                const ShaderPort* port = vertexData[i];
                const string& variable = port->getVariable();
                const string key = lowered(variable);
                if (variable == HW::T_POSITION_WORLD ||
                    key.find("positionworld") != string::npos) {
                    emitLine(instance + "." + variable + " = P", stage);
                } else if (variable == HW::T_NORMAL_WORLD ||
                           key.find("normalworld") != string::npos) {
                    emitLine(instance + "." + variable + " = N", stage);
                } else if (key.find("bitangentworld") != string::npos) {
                    // Before the tangent test, not after it: "bitangentworld"
                    // *contains* "tangentworld", so the looser match wins if it
                    // is asked first and the bitangent silently becomes the
                    // tangent. Every anisotropic closure and every normal map
                    // then works from a degenerate frame.
                    //
                    // Taken from the kernel rather than rebuilt as cross(N, T):
                    // the sign is a property of the *parameterisation*, and a
                    // mirrored UV island runs v the other way round. Deriving
                    // it here would silently invert every normal-mapped detail
                    // on the mirrored half of a symmetric asset.
                    emitLine(instance + "." + variable + " = B", stage);
                } else if (key.find("tangentworld") != string::npos) {
                    emitLine(instance + "." + variable + " = T", stage);
                } else if (key.find("positionobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Pobj", stage);
                } else if (key.find("normalobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Nobj", stage);
                } else if (key.find("bitangentobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Bobj", stage);
                } else if (key.find("tangentobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Tobj", stage);
                } else if (key.find("texcoord") != string::npos) {
                    // One UV set. A material that reads a second one gets the
                    // first rather than an undefined value; carrying more than
                    // one is a mesh-adapter change, recorded in
                    // docs/roadmap.md phase 7.
                    emitLine(instance + "." + variable + " = uv", stage);
                } else if (IsUvGeomProp(key, port)) {
                    // A `geompropvalue` node reading the UV primvar by name.
                    // That is how an asset asks for texture coordinates when it
                    // does not use the `texcoord` node -- MaterialX has both,
                    // they mean the same thing here, and a generator that
                    // recognised only the first left the coordinate at zero.
                    // Every pixel then sampled the same texel, which reads as a
                    // material with no texture at all rather than as a bug.
                    emitLine(instance + "." + variable + " = uv", stage);
                } else if (IsDisplayColorGeomProp(key, port)) {
                    // The placement's displayColor, which the kernel reads from
                    // the instance table. A `color3` port takes it as it is;
                    // MaterialX declares both as `vec3`, so a `vector3` one
                    // does too.
                    emitLine(instance + "." + variable + " = displayColor", stage);
                } else {
                    // A geomprop the kernel has no value for: a primvar the
                    // mesh adapter does not carry yet, or a colour set.
                    //
                    // It is assigned a defined zero rather than left alone. The
                    // struct is a global, so leaving a member unassigned is not
                    // "the kernel fills it later" -- nothing does, and the
                    // material then reads an undefined value. That produced a
                    // black surface for every material with a 3D procedural
                    // node, because the undefined object position made the
                    // pattern -- and everything downstream of it -- undefined.
                    emitLine(instance + "." + variable + " = " +
                                 _syntax->getDefaultValue(port->getType()),
                             stage);
                }
            }
            emitScopeEnd(stage);
            emitLineBreak(stage);
        }
    }
}

void PathTracerShaderGenerator::emitOutputs(GenContext&, ShaderStage&) const
{
    // A compute stage has no pixel outputs. Results leave through the ABI
    // globals emitted in emitPixelStage.
}

void PathTracerShaderGenerator::emitPixelStage(const ShaderGraph& graph,
                                               GenContext& context,
                                               ShaderStage& stage) const
{
    HwResourceBindingContextPtr resourceBindingCtx = getResourceBindingContext(context);

    // The opacity library is spliced into a traversal kernel after that
    // kernel's own `#version`, which GLSL requires to come first.
    const bool opacityLibrary = IsOpacityLibraryGraph(graph);

    // --- Directives ---------------------------------------------------------
    if (!opacityLibrary) {
        emitLine("#version 460", stage, false);
    }
    emitLine("#extension GL_GOOGLE_include_directive : enable", stage, false);
    emitLine("#extension GL_EXT_scalar_block_layout : require", stage, false);
    emitLine("#extension GL_EXT_nonuniform_qualifier : require", stage, false);
    // Buffer device addresses are 64-bit, and GL_EXT_buffer_reference does not
    // imply the integer type extension. See docs/implementation-notes.md.
    emitLine("#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require",
             stage, false);
    if (resourceBindingCtx)
    {
        resourceBindingCtx->emitDirectives(context, stage);
    }
    emitLineBreak(stage);

    // --- Types, constants, uniforms -----------------------------------------
    emitTypeDefinitions(context, stage);
    emitConstants(context, stage);
    emitUniforms(context, stage);
    emitInputs(context, stage);

    // --- Library ------------------------------------------------------------
    emitLibraryInclude("stdlib/genglsl/lib/mx_math.glsl", context, stage);
    emitLineBreak(stage);

    // Directional albedo is evaluated analytically: the table and Monte Carlo
    // methods both assume a rasteriser's prefiltered environment, which this
    // target does not have.
    emitLine("#define DIRECTIONAL_ALBEDO_METHOD 0", stage, false);
    if (const std::optional<unsigned int> iterations =
            AiryFresnelIterations(context.getOptions())) {
        emitLine("#define AIRY_FRESNEL_ITERATIONS " + std::to_string(*iterations),
                 stage, false);
    }
    emitLineBreak(stage);

    // Screen-space derivatives, defined as zero.
    //
    // `genglsl` is a rasteriser target and several of its nodes ask for a
    // filter width -- `mx_aastep` is the common one, reached by any node with
    // a hard edge to antialias. On a compute stage those builtins do not exist
    // without an extension, and enabling the extension would be worse than the
    // compile error it removes: neighbouring lanes in a `shade` dispatch are
    // unrelated paths, possibly on opposite sides of the scene, so a derivative
    // between them measures nothing at all.
    //
    // Zero is the right answer rather than a stand-in. A path tracer
    // antialiases by sampling the pixel, so the filter width at a shading point
    // is zero and `mx_aastep` degrades to the hard step it is approximating.
    // A real footprint, when hdClaude has one, comes from ray differentials --
    // that is the standing rule (docs/implementation-notes.md), and this is
    // what makes the inherited library obey it instead of failing to compile.
    emitComment("Screen-space derivatives do not exist here; see "
                "docs/implementation-notes.md", stage);
    emitLine("#define dFdx(x) ((x) * 0.0)", stage, false);
    emitLine("#define dFdy(x) ((x) * 0.0)", stage, false);
    emitLine("#define fwidth(x) ((x) * 0.0)", stage, false);
    emitLineBreak(stage);

    // --- The hdClaude material ABI ------------------------------------------
    emitComment("hdClaude material ABI, revision " +
                    std::to_string(kMaterialAbiVersion),
                stage);
    emitLine("BSDF " + string(kBsdfGlobal), stage);
    emitLine("vec3 " + string(kEmissionGlobal) + " = vec3(0.0)", stage);
    emitLine("float " + string(kOpacityGlobal) + " = 1.0", stage);
    // Where a displacement program leaves its answer. Declared for every
    // material rather than only for the ones that displace: the struct costs
    // nothing in a program that never writes it, and declaring it
    // conditionally would make the ABI depend on the document.
    emitLine("displacementshader " + string(kDisplacementGlobal) +
                 " = displacementshader(vec3(0.0), 1.0)",
             stage);
    emitLineBreak(stage);

    // --- Token substitutions ---------------------------------------------------
    //
    // The stock `mx_image_*.glsl` implementations open with
    // `#include "lib/$fileTransformUv"`, and the substitution that resolves it
    // is set by GlslShaderGenerator::emitPixelStage -- which this target
    // replaces. Setting it here rather than inheriting it is the cost of
    // owning the pixel stage; without it every material containing an <image>
    // node fails to generate with an unresolved include, which is to say every
    // textured material in a real asset.
    //
    // It must precede emitFunctionDefinitions, which is where those includes
    // are emitted.
    _tokenSubstitutions[ShaderGenerator::T_FILE_TRANSFORM_UV] =
        context.getOptions().fileTextureVerticalFlip ? "mx_transform_uv_vflip.glsl"
                                                     : "mx_transform_uv.glsl";

    // --- Node function definitions ------------------------------------------
    emitFunctionDefinitions(graph, context, stage);

    // --- The opacity library ------------------------------------------------
    //
    // Every cut-out material's opacity graph, in one program, for the
    // traversal kernels to call on a candidate hit. One generation defines
    // each function the materials share once; the materials' own nodes are
    // then emitted each inside its own `case`, so a candidate evaluates its
    // material's graph and no other. The nodes that join the roots into the
    // single output MaterialX generated from are never emitted: they exist so
    // that there was one output to generate from.
    if (opacityLibrary) {
        // The root of each material's graph, in graph order.
        std::vector<std::pair<std::string, const ShaderNode*>> roots;
        const string prefix = kOpacityRootPrefix;
        for (const ShaderNode* node : graph.getNodes()) {
            if (node->getName().rfind(prefix, 0) == 0) {
                roots.emplace_back(node->getName().substr(prefix.size()), node);
            }
        }

        emitLine("bool " + string(kMaterialCutoutQuery) + "(uint material)",
                 stage, false);
        emitScopeBegin(stage);
        emitLine("switch (material)", stage, false);
        emitScopeBegin(stage);
        for (const auto& root : roots) {
            emitLine("case " + root.first + "u:", stage, false);
        }
        emitLine("return true", stage);
        emitScopeEnd(stage);
        emitLine("return false", stage);
        emitScopeEnd(stage);
        emitLineBreak(stage);

        setFunctionName(kMaterialOpacityEntryPoint, stage);
        emitLine("float " + string(kMaterialOpacityEntryPoint) + "(uint material)",
                 stage, false);
        emitScopeBegin(stage);
        emitLine("switch (material)", stage, false);
        emitScopeBegin(stage);
        for (const auto& root : roots) {
            // The nodes this root reads, transitively. Each material's nodes
            // carry a prefix of their own, so no node is in two of these.
            std::set<const ShaderNode*> cone;
            std::vector<const ShaderNode*> pending = {root.second};
            while (!pending.empty()) {
                const ShaderNode* node = pending.back();
                pending.pop_back();
                if (!cone.insert(node).second) {
                    continue;
                }
                for (const ShaderInput* input : node->getInputs()) {
                    const ShaderOutput* source = input->getConnection();
                    if (source && source->getNode()->getParent() == &graph) {
                        pending.push_back(source->getNode());
                    }
                }
            }

            emitLine("case " + root.first + "u:", stage, false);
            emitScopeBegin(stage);
            // In graph order, which is topological, so every node's inputs
            // exist before it reads them.
            for (const ShaderNode* node : graph.getNodes()) {
                if (cone.count(node) != 0) {
                    emitFunctionCall(*node, context, stage);
                }
            }
            emitLine("return " + root.second->getOutput()->getVariable(), stage);
            emitScopeEnd(stage);
        }
        emitScopeEnd(stage);
        emitLine("return 1.0", stage);
        emitScopeEnd(stage);
        emitLineBreak(stage);
        return;
    }

    // --- A displacement program ---------------------------------------------
    //
    // MaterialX's `displacement` terminal is a separate output from `surface`
    // with its own graph, so a displacing material generates twice: once for
    // what the surface looks like and once for where it is. The graph here
    // terminates in a `displacementshader` rather than a `surfaceshader`, and
    // the whole entry point is the pattern nodes that feed it plus the struct
    // they produce -- there are no closures to sample and no ClosureData to
    // thread, which is why it takes no parameter.
    //
    // A displacement is evaluated per *vertex*, before the acceleration
    // structure is built, so this runs in the `displace` kernel rather than in
    // `shade`, over geometry rather than over paths.
    if (IsDisplacementGraph(graph)) {
        setFunctionName(kMaterialDisplaceEntryPoint, stage);
        emitLine("void " + string(kMaterialDisplaceEntryPoint) + "()", stage,
                 false);
        emitScopeBegin(stage);
        emitFunctionCalls(graph, context, stage);
        for (ShaderGraphOutputSocket* socket : graph.getOutputSockets()) {
            if (socket->getConnection()) {
                emitLine(string(kDisplacementGlobal) + " = " +
                             socket->getConnection()->getVariable(),
                         stage);
            }
        }
        emitScopeEnd(stage);
        emitLineBreak(stage);
        return;
    }

    // --- Entry point --------------------------------------------------------
    //
    // The parameter is named `closureData` because every generated closure
    // call references that name literally. Naming it anything else would mean
    // rewriting every closure call site.
    setFunctionName(kMaterialShadeEntryPoint, stage);
    emitLine("void " + string(kMaterialShadeEntryPoint) + "(ClosureData closureData)",
             stage, false);
    emitScopeBegin(stage);

    emitLine(string(kBsdfGlobal) + " = " +
                 _syntax->getDefaultValue(Type::BSDF), stage);
    emitLine(string(kEmissionGlobal) + " = vec3(0.0)", stage);
    emitLine(string(kOpacityGlobal) + " = 1.0", stage);
    emitLineBreak(stage);

    if (graph.hasClassification(ShaderNode::Classification::SHADER |
                                ShaderNode::Classification::SURFACE))
    {
        // Pattern nodes first: they are inputs to the closures and must exist
        // before any closure references them.
        emitFunctionCalls(graph, context, stage, ShaderNode::Classification::TEXTURE);

        for (ShaderGraphOutputSocket* socket : graph.getOutputSockets())
        {
            if (socket->getConnection())
            {
                const ShaderNode* upstream = socket->getConnection()->getNode();
                if (upstream->getParent() == &graph &&
                    (upstream->hasClassification(ShaderNode::Classification::CLOSURE) ||
                     upstream->hasClassification(ShaderNode::Classification::SHADER)))
                {
                    emitFunctionCall(*upstream, context, stage);
                }
            }
        }
    }
    else
    {
        emitFunctionCalls(graph, context, stage);
    }

    emitScopeEnd(stage);
    emitLineBreak(stage);
}

}  // namespace hdclaude

namespace hdclaude {

mx::FileSearchPath MaterialXSourceSearchPath(const mx::FilePath& stdlibDir,
                                             const mx::FilePath& hdclaudeDir)
{
    mx::FileSearchPath search;

    // Ours first: an override's `lib/mx_closure_type.glsl` must resolve to the
    // hdClaude one, never the stock one, or the generated shader declares
    // ClosureData without the path-tracing protocol.
    search.append(hdclaudeDir / mx::FilePath("pbrlib/genglsl_pt"));
    // The stdlib overrides, for the same reason: their `lib/mx_hdclaude_image`
    // must resolve here, and their own `lib/$fileTransformUv` must still reach
    // the stock one below.
    search.append(hdclaudeDir / mx::FilePath("stdlib/genglsl_pt"));
    search.append(hdclaudeDir);
    search.append(hdclaudeDir.getParentPath());

    // The stock target directories supply every shared helper our overrides
    // include and deliberately do not duplicate -- mx_microfacet_specular.glsl
    // and friends. None of those include the closure header, which is what
    // makes mixing the two trees safe (docs/materialx-codegen.md 3).
    search.append(stdlibDir / mx::FilePath("pbrlib/genglsl"));
    search.append(stdlibDir / mx::FilePath("stdlib/genglsl"));

    // The stock `lib` directories, for sibling-relative includes. A helper we
    // override lives in hdClaude's `lib`, but its own includes are written
    // relative to a sibling (`#include "mx_microfacet.glsl"`) and must still
    // reach the stock file.
    search.append(stdlibDir / mx::FilePath("pbrlib/genglsl/lib"));
    search.append(stdlibDir / mx::FilePath("stdlib/genglsl/lib"));

    search.append(stdlibDir);

    // emitLibraryInclude resolves as "libraries/<path>", so the directory
    // *containing* the stock tree has to be present too.
    search.append(stdlibDir.getParentPath());

    return search;
}

mx::FileSearchPath DefaultMaterialXSourceSearchPath()
{
    return MaterialXSourceSearchPath(mx::FilePath(HDCLAUDE_MATERIALX_STDLIB_DIR),
                                     mx::FilePath(HDCLAUDE_MTLX_LIBRARY_DIR));
}

mx::DocumentPtr LoadMaterialXLibraries(const mx::FilePath& stdlibDir,
                                       const mx::FilePath& hdclaudeDir)
{
    mx::DocumentPtr libraries = mx::createDocument();

    mx::FileSearchPath stdlibSearch(stdlibDir.getParentPath());
    mx::loadLibraries({stdlibDir.getBaseName()}, stdlibSearch, libraries);

    mx::FileSearchPath hdclaudeSearch(hdclaudeDir.getParentPath());
    mx::loadLibraries({hdclaudeDir.getBaseName()}, hdclaudeSearch, libraries);

    return libraries;
}

mx::DocumentPtr LoadDefaultMaterialXLibraries()
{
    return LoadMaterialXLibraries(mx::FilePath(HDCLAUDE_MATERIALX_STDLIB_DIR),
                                  mx::FilePath(HDCLAUDE_MTLX_LIBRARY_DIR));
}

namespace {

/// One float input of a node: the authored value, else the nodedef's default.
///
/// The default matters as much as the authored value here. A material that says
/// nothing about dispersion still *has* a dispersion scale -- zero -- and
/// reading it from the declaration rather than assuming it is what keeps this
/// answering the same question MaterialX would.
float ReadFloatInput(const mx::NodePtr& node, const char* input, bool* connected)
{
    *connected = false;
    if (const mx::InputPtr authored = node->getInput(input)) {
        if (authored->hasNodeName() || authored->hasNodeGraphString() ||
            authored->hasInterfaceName()) {
            *connected = true;
            return 0.0f;
        }
        if (const mx::ValuePtr value = authored->getValue()) {
            if (value->isA<float>()) {
                return value->asA<float>();
            }
        }
    }
    if (const mx::NodeDefPtr definition = node->getNodeDef()) {
        if (const mx::InputPtr declared = definition->getActiveInput(input)) {
            if (const mx::ValuePtr value = declared->getValue()) {
                if (value->isA<float>()) {
                    return value->asA<float>();
                }
            }
        }
    }
    return 0.0f;
}

/// A boolean input's authored value, or its nodedef default. `connected` says
/// whether the input is driven by something rather than authored.
bool ReadBoolInput(const mx::NodePtr& node, const char* input, bool* connected)
{
    *connected = false;
    if (const mx::InputPtr authored = node->getInput(input)) {
        if (authored->hasNodeName() || authored->hasNodeGraphString() ||
            authored->hasInterfaceName()) {
            *connected = true;
            return false;
        }
        if (const mx::ValuePtr value = authored->getValue()) {
            if (value->isA<bool>()) {
                return value->asA<bool>();
            }
        }
    }
    if (const mx::NodeDefPtr definition = node->getNodeDef()) {
        if (const mx::InputPtr declared = definition->getActiveInput(input)) {
            if (const mx::ValuePtr value = declared->getValue()) {
                if (value->isA<bool>()) {
                    return value->asA<bool>();
                }
            }
        }
    }
    return false;
}

}  // namespace

bool AuthoredThinWalled(const mx::DocumentPtr& document,
                        std::vector<std::string>* diagnostics)
{
    if (!document) {
        return false;
    }

    bool found = false;
    bool thin = false;
    for (const mx::NodePtr& node : document->getNodes()) {
        const std::string& category = node->getCategory();
        const char* input = category == "open_pbr_surface"   ? "geometry_thin_walled"
                            : category == "standard_surface" ? "thin_walled"
                                                             : nullptr;
        if (!input) {
            continue;
        }

        bool connected = false;
        const bool authored = ReadBoolInput(node, input, &connected);
        if (connected) {
            if (diagnostics) {
                diagnostics->push_back(
                    "node '" + node->getName() + "' connects '" + input +
                    "'; thin-walled is a per-material property here and only an "
                    "authored value can be honoured, so it renders as not "
                    "thin-walled");
            }
            continue;
        }
        if (found && authored != thin) {
            if (diagnostics) {
                diagnostics->push_back(
                    "surface node '" + node->getName() +
                    "' disagrees with an earlier one about being thin-walled; "
                    "the first is used");
            }
            continue;
        }
        found = true;
        thin = authored;
    }
    return thin;
}

void ReportRetroreflection(const mx::DocumentPtr& document,
                           std::vector<std::string>* diagnostics)
{
    if (!document || !diagnostics) {
        return;
    }
    for (mx::ElementPtr element : document->traverseTree()) {
        const mx::NodePtr node = element->asA<mx::Node>();
        if (!node) {
            continue;
        }
        const std::string& category = node->getCategory();
        if (category != "conductor_bsdf" && category != "dielectric_bsdf" &&
            category != "generalized_schlick_bsdf") {
            continue;
        }
        // Inside a library's own implementation graph is not the author's.
        if (const mx::NodeGraphPtr graph = node->getParent()->asA<mx::NodeGraph>()) {
            if (graph->hasNodeDefString()) {
                continue;
            }
        }
        bool connected = false;
        if (ReadBoolInput(node, "retroreflective", &connected) || connected) {
            diagnostics->push_back(
                "node '" + node->getNamePath() + "' " +
                (connected ? "connects" : "sets") +
                " 'retroreflective', which hdClaude does not implement; it "
                "renders as an ordinary reflection");
        }
    }
}

float AuthoredDispersion(const mx::DocumentPtr& document,
                         std::vector<std::string>* diagnostics)
{
    if (!document) {
        return 0.0f;
    }

    float effective = 0.0f;
    for (const mx::NodePtr& node : document->getNodes()) {
        if (node->getCategory() != "open_pbr_surface") {
            continue;
        }

        bool scaleConnected = false;
        bool abbeConnected = false;
        const float scale =
            ReadFloatInput(node, "transmission_dispersion_scale", &scaleConnected);
        const float abbe = ReadFloatInput(
            node, "transmission_dispersion_abbe_number", &abbeConnected);

        if (scaleConnected || abbeConnected) {
            // A connected input varies over the surface, and dispersion reaches
            // the integrator as one number for the whole material. Reported
            // rather than sampled somewhere arbitrary: an index of refraction
            // taken from the wrong texel is worse than no dispersion at all.
            if (diagnostics) {
                diagnostics->push_back(
                    "node '" + node->getName() +
                    "' connects a transmission dispersion input; dispersion is "
                    "a per-material property here and only an authored value "
                    "can be honoured, so it renders without dispersion");
            }
            continue;
        }
        if (!(scale > 0.0f) || !(abbe > 0.0f)) {
            continue;
        }

        const float authored = abbe / scale;
        if (effective > 0.0f && authored != effective) {
            if (diagnostics) {
                diagnostics->push_back(
                    "surface node '" + node->getName() +
                    "' authors a different dispersion from an earlier one; the "
                    "first is used");
            }
            continue;
        }
        effective = authored;
    }
    return effective;
}


namespace {

/// Which of MaterialX's two displacement constructors a node is.
///
/// They share the category `displacement`, so the category cannot tell them
/// apart. What does is the *declared* type of their `displacement` input:
/// `ND_displacement_float` takes a float, "scalar displacement amount along the
/// surface normal direction", and `ND_displacement_vector3` takes a vector3,
/// "vector displacement in (dPdu, dPdv, N) tangent/normal space". Read from the
/// nodedef rather than from the authored input, which a document need not have
/// authored at all.
DisplacementSpace ConstructorSpace(mx::NodePtr node,
                                   std::vector<std::string>* diagnostics)
{
    // `dot` is defined as "a no-op ... used to define a routing point", so
    // following one is honouring the specification rather than guessing: a
    // node editor puts them wherever a connection turns a corner, and the
    // terminal reached through one is still the constructor behind it.
    //
    // Bounded rather than recursive. A cycle is invalid MaterialX and
    // `Document::validate` rejects it, but a renderer that hangs on a bad
    // document is a worse answer than one that reports it.
    for (int hop = 0; node && node->getCategory() == "dot" && hop < 64; ++hop) {
        const mx::NodePtr routed = node->getConnectedNode("in");
        if (!routed) {
            break;
        }
        node = routed;
    }
    if (!node) {
        return DisplacementSpace::AlongNormal;
    }
    if (const mx::NodeDefPtr definition = node->getNodeDef()) {
        if (const mx::InputPtr declared =
                definition->getActiveInput("displacement")) {
            if (declared->getType() == "vector3") {
                return DisplacementSpace::Tangent;
            }
            if (declared->getType() == "float") {
                return DisplacementSpace::AlongNormal;
            }
        }
    }
    // Some other node that happens to produce a `displacementshader`. It is
    // generated and run like any other -- the program is whatever the graph
    // says -- but nothing declares what frame its three floats are in, so this
    // is reported rather than assumed silently.
    if (diagnostics) {
        diagnostics->push_back(
            "displacement terminal '" + node->getName() + "' is a '" +
            node->getCategory() +
            "' rather than one of MaterialX's two `displacement` constructors, "
            "so the frame its offset is in is undeclared; it is read along the "
            "surface normal");
    }
    return DisplacementSpace::AlongNormal;
}

}  // namespace

DisplacementTerminal AuthoredDisplacement(const mx::DocumentPtr& document,
                                          std::vector<std::string>* diagnostics)
{
    DisplacementTerminal result;
    if (!document) {
        return result;
    }

    for (const mx::NodePtr& material : document->getMaterialNodes()) {
        const mx::InputPtr input = material->getInput("displacementshader");
        if (!input) {
            continue;
        }
        const mx::NodePtr terminal = input->getConnectedNode();
        if (!terminal) {
            continue;
        }
        if (result.terminal) {
            if (terminal->getNamePath() != result.terminal->getNamePath() &&
                diagnostics) {
                diagnostics->push_back(
                    "material '" + material->getName() + "' displaces with '" +
                    terminal->getName() +
                    "' where an earlier material displaces with '" +
                    result.terminal->getName() +
                    "'; one program is compiled per material network, so the "
                    "first is used");
            }
            continue;
        }
        result.terminal = terminal;
        result.space = ConstructorSpace(terminal, diagnostics);
    }
    return result;
}

mx::InputPtr FilenameSource(const mx::InputPtr& input)
{
    // The two nodes ShaderGraph bypasses when it optimises, and the input each
    // passes through. Following exactly these is what keeps the path found
    // here the one the generated sampler was baked with.
    static const std::map<std::string, std::string> kRouting = {
        {"dot", "in"},
        {"constant", "value"},
    };

    // Bounded rather than recursive, for the reason ConstructorSpace gives.
    mx::InputPtr current = input;
    for (int hop = 0; current && hop < 64; ++hop) {
        if (const mx::InputPtr interface = current->getInterfaceInput()) {
            current = interface;
            continue;
        }
        const mx::NodePtr upstream = current->getConnectedNode();
        if (!upstream) {
            return current;
        }
        const auto routing = kRouting.find(upstream->getCategory());
        if (routing == kRouting.end()) {
            return nullptr;
        }
        current = upstream->getInput(routing->second);
    }
    return nullptr;
}

namespace {

/// The nodegraph, and its output, that an extracted opacity lives in.
constexpr const char* kOpacityGraph = "hdclaude_opacity";
constexpr const char* kOpacityGraphOutput = "out";

/// What an input reads: the node and which of its outputs. The node is null
/// when the input holds a value. Interface inputs and nodegraph outputs are
/// followed, as MaterialX's own Input::getConnectedNode follows them.
struct Upstream {
    mx::NodePtr node;
    std::string output;
};

/// The input whose value `input` takes when nothing is connected: its
/// nodegraph's interface input when it names one, otherwise itself.
mx::InputPtr ValueSource(const mx::InputPtr& input)
{
    const mx::InputPtr interface = input->getInterfaceInput();
    return interface ? interface : input;
}

Upstream UpstreamOf(const mx::InputPtr& input)
{
    Upstream result;
    const mx::InputPtr port = ValueSource(input);
    result.node = port->getConnectedNode();
    if (!result.node) {
        return result;
    }
    if (const mx::OutputPtr output = port->getConnectedOutput()) {
        // A multi-output node's own output, or a nodegraph output naming one.
        result.output = output->getParent() == result.node
                            ? output->getName()
                            : output->getOutputString();
    } else {
        result.output = port->getOutputString();
    }
    return result;
}

/// A value folded on the host: up to four lanes, which is what MaterialX's
/// float, integer, boolean, colour and vector types all are.
struct Lanes {
    std::array<float, 4> v{};
    int n = 1;
};

int LaneCount(const std::string& type)
{
    if (type == "float" || type == "integer" || type == "boolean") return 1;
    if (type == "vector2") return 2;
    if (type == "color3" || type == "vector3") return 3;
    if (type == "color4" || type == "vector4") return 4;
    return 0;
}

std::optional<Lanes> LanesOf(const mx::ValuePtr& value)
{
    if (!value) {
        return std::nullopt;
    }
    Lanes lanes;
    if (value->isA<float>()) {
        lanes.v[0] = value->asA<float>();
        return lanes;
    }
    if (value->isA<int>()) {
        lanes.v[0] = static_cast<float>(value->asA<int>());
        return lanes;
    }
    if (value->isA<bool>()) {
        lanes.v[0] = value->asA<bool>() ? 1.0f : 0.0f;
        return lanes;
    }
    auto fill = [&lanes](const auto& vector, int n) {
        lanes.n = n;
        for (int i = 0; i < n; ++i) {
            lanes.v[i] = vector[i];
        }
        return lanes;
    };
    if (value->isA<mx::Color3>()) return fill(value->asA<mx::Color3>(), 3);
    if (value->isA<mx::Color4>()) return fill(value->asA<mx::Color4>(), 4);
    if (value->isA<mx::Vector2>()) return fill(value->asA<mx::Vector2>(), 2);
    if (value->isA<mx::Vector3>()) return fill(value->asA<mx::Vector3>(), 3);
    if (value->isA<mx::Vector4>()) return fill(value->asA<mx::Vector4>(), 4);
    return std::nullopt;
}

std::optional<Lanes> FoldNode(const mx::NodePtr& node, const std::string& output,
                              int depth);

/// The value an input of `node` takes, folded, or nothing when it depends on
/// something only the GPU knows.
std::optional<Lanes> FoldInput(const mx::NodePtr& node, const std::string& name,
                               int depth)
{
    if (const mx::InputPtr input = node->getInput(name)) {
        const Upstream upstream = UpstreamOf(input);
        if (upstream.node) {
            return FoldNode(upstream.node, upstream.output, depth + 1);
        }
        const mx::InputPtr source = ValueSource(input);
        if (source->hasValueString()) {
            return LanesOf(source->getValue());
        }
        if (source->hasDefaultGeomPropString()) {
            return std::nullopt;
        }
    }
    // Unauthored: the definition's default, unless that is a geometric
    // property, which varies over the surface.
    const mx::NodeDefPtr nodeDef = node->getNodeDef();
    const mx::InputPtr declared = nodeDef ? nodeDef->getActiveInput(name) : nullptr;
    if (!declared || declared->hasDefaultGeomPropString() ||
        !declared->hasValueString()) {
        return std::nullopt;
    }
    return LanesOf(declared->getValue());
}

/// A node's output, folded, for the stdlib nodes simple enough to evaluate
/// here by their definitions in the MaterialX specification. Anything else --
/// an image, a geometric property, a node this does not know -- is left to the
/// GPU, which is always correct and only slower.
std::optional<Lanes> FoldNode(const mx::NodePtr& node, const std::string& output,
                              int depth)
{
    // Multi-output nodes are not folded: none of the ones folded here has more
    // than the one output.
    if (!node || depth > 64 || (!output.empty() && output != "out")) {
        return std::nullopt;
    }
    const int lanes = LaneCount(node->getType());
    if (lanes == 0) {
        return std::nullopt;
    }
    auto in = [&](const std::string& name) { return FoldInput(node, name, depth); };
    // A lane of `x` as an operand of this node: a scalar is broadcast.
    auto lane = [](const Lanes& x, int i) { return x.n == 1 ? x.v[0] : x.v[i]; };
    // `x` as this node's type: broadcast, truncated, or extended the way
    // MaterialX's `convert` extends -- zero, and one for an alpha.
    auto shaped = [&](const Lanes& x) {
        Lanes result;
        result.n = lanes;
        for (int i = 0; i < lanes; ++i) {
            result.v[i] = x.n == 1 ? x.v[0] : i < x.n ? x.v[i] : (i == 3 ? 1.0f : 0.0f);
        }
        return result;
    };
    auto componentwise = [&](const Lanes& a, const Lanes& b, auto op) {
        Lanes result;
        result.n = lanes;
        for (int i = 0; i < lanes; ++i) {
            result.v[i] = op(lane(a, i), lane(b, i));
        }
        return result;
    };

    const std::string& category = node->getCategory();
    if (category == "constant") {
        const auto value = in("value");
        return value ? std::optional<Lanes>(shaped(*value)) : std::nullopt;
    }
    if (category == "dot" || category == "convert") {
        const auto value = in("in");
        return value ? std::optional<Lanes>(shaped(*value)) : std::nullopt;
    }
    if (category == "extract") {
        const auto value = in("in");
        const auto index = in("index");
        if (!value || !index) return std::nullopt;
        const int i = static_cast<int>(index->v[0]);
        if (i < 0 || i >= value->n) return std::nullopt;
        Lanes result;
        result.v[0] = value->v[i];
        return result;
    }
    if (category == "luminance") {
        const auto value = in("in");
        const auto coefficients = in("lumacoeffs");
        if (!value || !coefficients || value->n < 3 || coefficients->n < 3) {
            return std::nullopt;
        }
        Lanes result = *value;
        const float luminance = value->v[0] * coefficients->v[0] +
                                value->v[1] * coefficients->v[1] +
                                value->v[2] * coefficients->v[2];
        result.v[0] = result.v[1] = result.v[2] = luminance;
        return result;
    }
    if (category == "add" || category == "subtract" || category == "multiply" ||
        category == "divide" || category == "min" || category == "max") {
        const auto a = in("in1");
        const auto b = in("in2");
        if (!a || !b) return std::nullopt;
        if (category == "add") return componentwise(*a, *b, [](float x, float y) { return x + y; });
        if (category == "subtract") return componentwise(*a, *b, [](float x, float y) { return x - y; });
        if (category == "multiply") return componentwise(*a, *b, [](float x, float y) { return x * y; });
        if (category == "min") return componentwise(*a, *b, [](float x, float y) { return std::min(x, y); });
        if (category == "max") return componentwise(*a, *b, [](float x, float y) { return std::max(x, y); });
        for (int i = 0; i < lanes; ++i) {
            if (lane(*b, i) == 0.0f) return std::nullopt;
        }
        return componentwise(*a, *b, [](float x, float y) { return x / y; });
    }
    if (category == "clamp") {
        const auto value = in("in");
        const auto low = in("low");
        const auto high = in("high");
        if (!value || !low || !high) return std::nullopt;
        Lanes result;
        result.n = lanes;
        for (int i = 0; i < lanes; ++i) {
            result.v[i] = std::min(std::max(lane(*value, i), lane(*low, i)), lane(*high, i));
        }
        return result;
    }
    if (category == "mix") {
        // MaterialX's mix is bg + (fg - bg) * mix.
        const auto fg = in("fg");
        const auto bg = in("bg");
        const auto amount = in("mix");
        if (!fg || !bg || !amount) return std::nullopt;
        Lanes result;
        result.n = lanes;
        for (int i = 0; i < lanes; ++i) {
            result.v[i] = lane(*bg, i) + (lane(*fg, i) - lane(*bg, i)) * lane(*amount, i);
        }
        return result;
    }
    if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal") {
        const auto first = in("value1");
        const auto second = in("value2");
        if (!first || !second || first->n != second->n) return std::nullopt;
        bool taken = true;
        for (int i = 0; i < first->n; ++i) {
            const float x = first->v[i];
            const float y = second->v[i];
            taken = taken && (category == "ifgreater"     ? x > y
                              : category == "ifgreatereq" ? x >= y
                                                          : x == y);
        }
        const auto chosen = in(taken ? "in1" : "in2");
        return chosen ? std::optional<Lanes>(shaped(*chosen)) : std::nullopt;
    }
    if (category == "switch") {
        // `which` selects in1 at zero, in2 at one, and so on, saturating at
        // the last of the ten.
        const auto which = in("which");
        if (!which) return std::nullopt;
        const int index = std::min(std::max(static_cast<int>(std::floor(which->v[0])), 0), 9);
        const auto chosen = in("in" + std::to_string(index + 1));
        return chosen ? std::optional<Lanes>(shaped(*chosen)) : std::nullopt;
    }
    return std::nullopt;
}

/// Copies the graph upstream of a node into one nodegraph, flat.
///
/// Connections become `nodename` references inside that graph, values that
/// reached a node through a nodegraph interface are written onto it, and every
/// `filename` is baked resolved. The copy is meant to be read on its own,
/// later, in a document that has neither the original's nodegraphs nor its
/// `fileprefix`.
class OpacityConeCopier {
  public:
    OpacityConeCopier(mx::NodeGraphPtr graph,
                      const std::map<std::string, std::string>* resolvedTextures)
        : _graph(std::move(graph)), _resolvedTextures(resolvedTextures)
    {
    }

    /// The name of the copy of `node`, or empty with `Error()` set.
    std::string Copy(const mx::NodePtr& node, int depth = 0)
    {
        if (depth > 256) {
            _error = "the opacity graph is deeper than 256 nodes";
            return {};
        }
        const auto found = _copied.find(node);
        if (found != _copied.end()) {
            return found->second;
        }

        // An opacity is a pattern. A closure or a shader upstream of one means
        // the graph is not something traversal can evaluate.
        static const std::set<std::string> kNotPatterns = {
            "BSDF", "EDF", "VDF", "surfaceshader", "volumeshader",
            "displacementshader", "lightshader", "material"};
        if (kNotPatterns.count(node->getType()) != 0) {
            _error = "its opacity reads '" + node->getName() + "', a " +
                     node->getType() + ", which is not a pattern";
            return {};
        }

        // Qualified by the nodegraph it came from, because two graphs of one
        // document may each have an `image1`.
        const mx::ElementPtr parent = node->getParent();
        const std::string scope =
            parent && parent->isA<mx::NodeGraph>() ? parent->getName() + "_" : "";
        const std::string name =
            _graph->createValidChildName(mx::createValidName(scope + node->getName()));
        _copied[node] = name;

        const mx::NodePtr copy =
            _graph->addNode(node->getCategory(), name, node->getType());
        copy->copyContentFrom(node);
        for (const mx::InputPtr& input : copy->getInputs()) {
            const mx::InputPtr original = node->getInput(input->getName());
            input->removeAttribute(mx::PortElement::NODE_NAME_ATTRIBUTE);
            input->removeAttribute(mx::PortElement::NODE_GRAPH_ATTRIBUTE);
            input->removeAttribute(mx::PortElement::OUTPUT_ATTRIBUTE);
            input->removeAttribute(mx::ValueElement::INTERFACE_NAME_ATTRIBUTE);

            const Upstream upstream = UpstreamOf(original);
            if (upstream.node) {
                const std::string upstreamName = Copy(upstream.node, depth + 1);
                if (!_error.empty()) {
                    return {};
                }
                input->setNodeName(upstreamName);
                if (!upstream.output.empty()) {
                    input->setOutputString(upstream.output);
                }
                continue;
            }
            const mx::InputPtr source = ValueSource(original);
            if (source != original && source->hasValueString()) {
                input->setValueString(source->getValueString());
                if (!input->hasColorSpace() && source->hasColorSpace()) {
                    input->setColorSpace(source->getColorSpace());
                }
            }
        }

        // The paths, once every input is wired, so a `dot` a file is routed
        // through has been copied and the value lands where the copy reads it.
        for (const mx::InputPtr& input : node->getInputs()) {
            if (input->getType() != "filename") {
                continue;
            }
            const std::string uniform =
                mx::createValidName(node->getName() + "_" + input->getName());
            std::string path;
            if (_resolvedTextures) {
                const auto resolved = _resolvedTextures->find(uniform);
                if (resolved != _resolvedTextures->end()) {
                    path = resolved->second;
                }
            }
            if (path.empty()) {
                if (const mx::InputPtr source = FilenameSource(input)) {
                    path = source->getResolvedValueString();
                }
            }
            const mx::InputPtr target = FilenameSource(copy->getInput(input->getName()));
            if (target && !path.empty()) {
                target->setValueString(path);
            }
        }
        return name;
    }

    const std::string& Error() const { return _error; }

  private:
    mx::NodeGraphPtr _graph;
    const std::map<std::string, std::string>* _resolvedTextures;
    std::map<mx::NodePtr, std::string> _copied;
    std::string _error;
};

}  // namespace

std::string AuthoredCutoutOpacity(
    const mx::DocumentPtr& document,
    const std::map<std::string, std::string>* resolvedTextures,
    std::vector<std::string>* diagnostics)
{
    if (!document) {
        return {};
    }
    auto report = [diagnostics](const std::string& message) {
        if (diagnostics) {
            diagnostics->push_back(message);
        }
    };

    // The surface shader the material binds. hdMtlx builds one material per
    // document, and so does every document hdClaude builds itself.
    auto boundShader = [&document]() -> mx::NodePtr {
        for (const mx::NodePtr& material : document->getMaterialNodes()) {
            const mx::InputPtr input = material->getInput("surfaceshader");
            if (const mx::NodePtr shader = input ? input->getConnectedNode() : nullptr) {
                return shader;
            }
        }
        return nullptr;
    };
    mx::NodePtr shader = boundShader();
    if (!shader) {
        return {};
    }

    // Down to the `surface` node its own nodegraph ends in, flattening that one
    // node and nothing upstream of it: a pattern node with a nodegraph of its
    // own -- `UsdUVTexture` is one -- stays a node, which is what keeps the
    // colour space it reads its file in.
    const mx::InterfaceElementPtr implementation = shader->getImplementation();
    if (implementation && implementation->isA<mx::NodeGraph>()) {
        if (const mx::GraphElementPtr parent =
                shader->getParent()->asA<mx::GraphElement>()) {
            const mx::NodePtr flattening = shader;
            parent->flattenSubgraphs(mx::EMPTY_STRING,
                                     [&flattening](mx::NodePtr node) {
                                         return node == flattening;
                                     });
            shader = boundShader();
        }
    }
    if (!shader) {
        return {};
    }

    const mx::NodeDefPtr nodeDef = shader->getNodeDef();
    if (!shader->getInput("opacity") &&
        !(nodeDef && nodeDef->getActiveInput("opacity"))) {
        report("its surface is a '" + shader->getCategory() +
               "' node, whose opacity hdClaude does not trace; it is rendered "
               "opaque");
        return {};
    }

    // Folded where it can be. The comparison allows for luminance: white's is
    // one by definition and comes out of the float arithmetic a rounding short.
    const std::optional<Lanes> folded = FoldInput(shader, "opacity", 0);
    if (folded && !(folded->v[0] < 1.0f - 1.0e-6f)) {
        return {};
    }

    mx::DocumentPtr extracted = mx::createDocument();
    const mx::NodeGraphPtr graph = extracted->addNodeGraph(kOpacityGraph);
    const mx::OutputPtr output = graph->addOutput(kOpacityGraphOutput, "float");
    if (folded) {
        const mx::NodePtr constant = graph->addNode("constant", "folded", "float");
        constant->setInputValue("value", folded->v[0]);
        output->setNodeName(constant->getName());
    } else {
        const mx::InputPtr opacity = shader->getInput("opacity");
        const Upstream upstream = opacity ? UpstreamOf(opacity) : Upstream();
        if (!upstream.node) {
            // A value that would not fold: not a type this reads.
            report("its opacity is a value hdClaude cannot read; it is rendered "
                   "opaque");
            return {};
        }
        OpacityConeCopier copier(graph, resolvedTextures);
        const std::string root = copier.Copy(upstream.node);
        if (!copier.Error().empty()) {
            report(copier.Error() + "; it is rendered opaque");
            return {};
        }
        output->setNodeName(root);
        if (!upstream.output.empty()) {
            output->setOutputString(upstream.output);
        }
    }
    return mx::writeToXmlString(extracted);
}

OpacityLibrary OpacityLibraryDocument(
    const std::vector<std::pair<std::uint32_t, std::string>>& materials,
    const mx::DocumentPtr& libraries, std::string* error)
{
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return OpacityLibrary();
    };
    if (materials.empty()) {
        return fail("no material is cut out");
    }

    mx::DocumentPtr document = mx::createDocument();
    if (libraries) {
        document->importLibrary(libraries);
    }
    const mx::NodeGraphPtr graph = document->addNodeGraph("hdclaude_opacity_graph");

    mx::NodePtr sum;
    for (const auto& [material, text] : materials) {
        mx::DocumentPtr part = mx::createDocument();
        try {
            mx::readFromXmlString(part, text);
        } catch (const std::exception& exception) {
            return fail("material " + std::to_string(material) +
                        "'s opacity could not be read: " + exception.what());
        }
        const mx::NodeGraphPtr source = part->getNodeGraph(kOpacityGraph);
        const mx::OutputPtr out = source ? source->getOutput(kOpacityGraphOutput) : nullptr;
        if (!out) {
            return fail("material " + std::to_string(material) +
                        "'s opacity names no output");
        }

        // Under a prefix of the material's own, so no node is in two
        // materials' graphs -- the generator relies on that.
        const std::string prefix = "m" + std::to_string(material) + "_";
        for (const mx::NodePtr& node : source->getNodes()) {
            const mx::NodePtr copy =
                graph->addNode(node->getCategory(), prefix + node->getName(),
                               node->getType());
            copy->copyContentFrom(node);
            for (const mx::InputPtr& input : copy->getInputs()) {
                if (input->hasNodeName()) {
                    input->setNodeName(prefix + input->getNodeName());
                }
            }
        }

        const mx::NodePtr root = graph->addNode(
            "multiply", kOpacityRootPrefix + std::to_string(material), "float");
        const mx::InputPtr in1 = root->addInput("in1", "float");
        if (out->hasNodeName()) {
            in1->setNodeName(prefix + out->getNodeName());
            if (out->hasOutputString()) {
                in1->setOutputString(out->getOutputString());
            }
        } else {
            in1->setValueString(out->hasValueString() ? out->getValueString() : "1");
        }
        root->setInputValue("in2", 1.0f);

        if (!sum) {
            sum = root;
        } else {
            const mx::NodePtr add = graph->addNode(
                "add", "hdclaude_opacity_sum" + std::to_string(material), "float");
            add->addInput("in1", "float")->setNodeName(sum->getName());
            add->addInput("in2", "float")->setNodeName(root->getName());
            sum = add;
        }
    }

    OpacityLibrary library;
    library.output = graph->addOutput(kOpacityLibraryOutput, "float");
    library.output->setNodeName(sum->getName());
    library.document = document;
    return library;
}

}  // namespace hdclaude
