#include "hdclaude/materialx/pathtracer_generator.h"

#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/Shader.h>
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
        "BSDF(vec3(0.0),vec3(1.0),vec4(0.0),vec3(0.0),0.0,0.0,vec3(0.0),0.0)";
    static const string kBsdfDefinition =
        "struct BSDF {\n"
        "    vec3  response;\n"
        "    vec3  throughput;\n"
        "    vec4  spectrum;\n"
        "    vec3  sampledL;\n"
        "    float pdf;\n"
        "    float isDelta;\n"
        "    vec3  guideAlbedo;\n"
        "    float guideRoughness;\n"
        "};";

    registerTypeSyntax(Type::BSDF,
                       std::make_shared<AggregateTypeSyntax>(
                           this, "BSDF", kBsdfDefault, EMPTY_STRING,
                           EMPTY_STRING, kBsdfDefinition));

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

    if (textureCount > 0)
    {
        emitComment("Shared texture array; see shaders/path_state.glsl", stage);
        emitLine("layout(set = 0, binding = 16) uniform sampler2D "
                 "hdclaude_textures[" + std::to_string(kTextureCapacity) + "]",
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
                // An index into the shared array, not a descriptor of its own.
                // `texture(name, uv)` in the stock mx_image_* implementations
                // then expands to a lookup in the array with no change to
                // those files.
                const size_t index = _textureOrder.size();
                _textureOrder.push_back(uniform->getVariable());
                emitLine("#define " + uniform->getVariable() +
                             " hdclaude_textures[" + std::to_string(index) + "]",
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
            "This material declares " + std::to_string(_textureOrder.size()) +
            " textures; hdClaude's shared array holds " +
            std::to_string(kTextureCapacity) +
            ". Raise kTextureCapacity in pathtracer_generator.h and "
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
                         "(vec3 P, vec3 N, vec3 T, vec3 Pobj, vec3 Nobj, "
                         "vec3 Tobj, vec2 uv)",
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
                    emitLine(instance + "." + variable + " = cross(N, T)", stage);
                } else if (key.find("tangentworld") != string::npos) {
                    emitLine(instance + "." + variable + " = T", stage);
                } else if (key.find("positionobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Pobj", stage);
                } else if (key.find("normalobject") != string::npos) {
                    emitLine(instance + "." + variable + " = Nobj", stage);
                } else if (key.find("bitangentobject") != string::npos) {
                    emitLine(instance + "." + variable + " = cross(Nobj, Tobj)",
                             stage);
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

}  // namespace hdclaude
