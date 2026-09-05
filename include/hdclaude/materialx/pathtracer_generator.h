// The MaterialX `genglsl_pt` shader generator.
//
// This is the C++ half of the target described in docs/materialx-codegen.md.
// The other half is the data library under mtlx/, which overrides the pbrlib
// closure nodes to add importance sampling. Neither half works alone.
//
// Three pieces, each an existing MaterialX extension point with an upstream
// precedent:
//
//   PathTracerSyntax          extends the BSDF struct, which MaterialX
//                             registers as a type syntax rather than emitting
//                             from a library file.
//   PathTracerSurfaceNode     replaces the `surface` node's calling
//                             convention. The stock one builds ClosureData
//                             inside a rasteriser light loop.
//   PathTracerShaderGenerator emits a compute stage instead of vertex/pixel,
//                             and the hdClaude material ABI entry point.

#ifndef HDCLAUDE_MATERIALX_PATHTRACER_GENERATOR_H
#define HDCLAUDE_MATERIALX_PATHTRACER_GENERATOR_H

// MaterialX 1.39.3 header layout -- the version inside OpenUSD 26.03. 1.39.6
// moved the hardware-generator pieces into MaterialXGenHw and renamed
// SurfaceNodeGlsl to HwSurfaceNode; see docs/implementation-notes.md.
#include <MaterialXGenGlsl/GlslShaderGenerator.h>
#include <MaterialXGenGlsl/Nodes/SurfaceNodeGlsl.h>
#include <MaterialXGenGlsl/VkShaderGenerator.h>
#include <MaterialXGenGlsl/VkSyntax.h>
#include <MaterialXGenShader/HwShaderGenerator.h>

#include <cstdint>

#include <string>

namespace hdclaude {

namespace mx = MaterialX;

/// Revision of the generated-code contract between the shader generator and
/// the wavefront kernels.
///
/// Bumped whenever the entry-point signature, the BSDF struct, or the ClosureData
/// struct changes. It is part of the shader cache key, so a change here
/// invalidates every cached module by construction rather than by remembering.
inline constexpr std::uint32_t kMaterialAbiVersion = 1;

/// Name of the generated entry point the `shade` kernel calls.
inline constexpr const char* kMaterialShadeEntryPoint = "hdclaude_material_shade";

/// Syntax for `genglsl_pt`.
///
/// Identical to Vulkan GLSL except that `BSDF` carries the path-tracing fields.
/// MaterialX registers that struct as a type syntax in GlslSyntax:
///
///     registerTypeSyntax(Type::BSDF, AggregateTypeSyntax(
///         this, "BSDF", "BSDF(vec3(0.0),vec3(1.0))", ...,
///         "struct BSDF { vec3 response; vec3 throughput; };"));
///
/// so extending it means re-registering with a matching default value. Both
/// strings come from one registration and cannot drift apart.
class PathTracerSyntax : public mx::VkSyntax {
  public:
    explicit PathTracerSyntax(mx::TypeSystemPtr typeSystem);

    static mx::SyntaxPtr create(mx::TypeSystemPtr typeSystem)
    {
        return std::make_shared<PathTracerSyntax>(typeSystem);
    }
};

/// The `surface` node for `genglsl_pt`.
///
/// The stock `SurfaceNodeGlsl` constructs `ClosureData` itself, inside a loop
/// over `u_lightData` for reflection and an environment lookup for indirect.
/// A path tracer supplies its own direction and its own light sample, so the
/// construction has to be replaced -- not the closures it wraps.
///
/// This implementation evaluates the BSDF and EDF against the `closureData`
/// the *caller* passed in, and publishes the results through the ABI globals
/// the entry point reads. No light loop, no environment.
class PathTracerSurfaceNode : public mx::SurfaceNodeGlsl {
  public:
    static mx::ShaderNodeImplPtr create();

    /// Registers only what a path tracer needs.
    ///
    /// The stock implementation also declares `u_viewPosition` and the whole
    /// lighting uniform set. Both are rasteriser state: the view direction and
    /// the light sample reach a path tracer through `closureData`, supplied by
    /// the integrator. Leaving them registered would put members in the
    /// material's uniform interface -- part of the ABI -- that no kernel can
    /// meaningfully fill.
    void createVariables(const mx::ShaderNode& node, mx::GenContext& context,
                         mx::Shader& shader) const override;

    void emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context,
                          mx::ShaderStage& stage) const override;
};

/// Shader generator for `genglsl_pt`.
class PathTracerShaderGenerator : public mx::VkShaderGenerator {
  public:
    explicit PathTracerShaderGenerator(mx::TypeSystemPtr typeSystem);

    static mx::ShaderGeneratorPtr create(mx::TypeSystemPtr typeSystem = nullptr)
    {
        return std::make_shared<PathTracerShaderGenerator>(
            typeSystem ? typeSystem : mx::TypeSystem::create());
    }

    const mx::string& getTarget() const override { return TARGET; }

    static const mx::string TARGET;

  protected:
    /// Emits a compute stage in place of the pixel stage. The stage *slot* is
    /// still Stage::PIXEL because that is simply where MaterialX puts the
    /// non-vertex stage; the content is a compute shader.
    void emitPixelStage(const mx::ShaderGraph& graph, mx::GenContext& context,
                        mx::ShaderStage& stage) const override;

    /// Geometric data reaches a compute shader as a struct the `shade` kernel
    /// fills, not as a rasteriser's interpolated stage connectors. The block
    /// MaterialX already computed is emitted as that struct, so the material
    /// declares exactly the geometry it uses and nothing more.
    void emitInputs(mx::GenContext& context, mx::ShaderStage& stage) const override;

    /// A compute stage has no pixel outputs. Results leave through the ABI.
    void emitOutputs(mx::GenContext& context, mx::ShaderStage& stage) const override;
};

/// Name of the generated struct carrying interpolated geometry into shading.
inline constexpr const char* kSurfaceHitStruct = "SurfaceHit";

/// Search path the generator resolves `#include` directives against.
///
/// Getting this wrong is not a subtle failure -- generation throws with the
/// unresolved filename -- but getting it *right* is fiddly enough to be worth
/// having in one place rather than in every caller.
///
/// MaterialX resolves an include relative to the including file first, then
/// against these entries. Two consequences shape the order below:
///
///  - hdClaude's `pbrlib/genglsl_pt` comes first, so `lib/mx_closure_type.glsl`
///    referenced from one of our overrides resolves to ours.
///  - the stock `pbrlib/genglsl` and `stdlib/genglsl` directories must be
///    present, because our overrides include shared helpers such as
///    `lib/mx_microfacet_specular.glsl` that we deliberately do not duplicate.
///
/// `stdlibDir` is the MaterialX library root of the OpenUSD distribution
/// (`<usd>/libraries`); `hdclaudeDir` is this repository's `mtlx` tree.
mx::FileSearchPath MaterialXSourceSearchPath(const mx::FilePath& stdlibDir,
                                             const mx::FilePath& hdclaudeDir);

/// The same, using the paths compiled in at build time.
mx::FileSearchPath DefaultMaterialXSourceSearchPath();

/// Load the stock MaterialX libraries plus hdClaude's genglsl_pt target into
/// one document. The targetdef's `inherit="genglsl"` is what makes every node
/// hdClaude does not override resolve to the stock implementation.
mx::DocumentPtr LoadMaterialXLibraries(const mx::FilePath& stdlibDir,
                                       const mx::FilePath& hdclaudeDir);

/// The same, using the paths compiled in at build time.
mx::DocumentPtr LoadDefaultMaterialXLibraries();

}  // namespace hdclaude

#endif  // HDCLAUDE_MATERIALX_PATHTRACER_GENERATOR_H
