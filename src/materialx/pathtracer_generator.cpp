#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Nodes/HwImageNode.h>
#include <MaterialXGenShader/ShaderGraph.h>
#include <MaterialXGenShader/ShaderStage.h>

#include <algorithm>
#include <cctype>

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
    // material declares; `tileCount` is one for an ordinary image, and the
    // hdClaude `mx_image_*` overrides read it.
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
        "    int tileCount;   // 1 when the image is not a UDIM set" "\n"
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

        // Opacity first: it gates nothing here but the kernels need it for
        // cutout tests during traversal.
        const ShaderInput* opacityInput = node.getInput("opacity");
        if (opacityInput)
        {
            shadergen.emitLineBegin(stage);
            shadergen.emitString(string(kOpacityGlobal) + " = ", stage);
            if (opacityInput->getConnection())
            {
                shadergen.emitInput(opacityInput, context, stage);
            }
            else
            {
                shadergen.emitString("1.0", stage);
            }
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
                const auto range = udimRanges.find(uniform->getVariable());
                if (range == udimRanges.end())
                {
                    _textureOrder.push_back({uniform->getVariable(), 0});
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
                    _textureOrder.push_back({uniform->getVariable(), tile});
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
        if (!vertexData.empty())
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
            emitLine("void " + string(kSurfaceHitSetter) +
                         "(vec3 P, vec3 N, vec3 T, vec3 B, vec3 Pobj, "
                         "vec3 Nobj, vec3 Tobj, vec3 Bobj, vec2 uv)",
                     stage, false);
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

    // --- Directives ---------------------------------------------------------
    emitLine("#version 460", stage, false);
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

}  // namespace hdclaude
