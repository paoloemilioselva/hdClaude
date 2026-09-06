#pragma once

// Hydra material network -> MaterialX document -> generated GLSL -> SPIR-V.
//
// The whole shading path in one place. Nothing here interprets a surface model:
// the network is handed to MaterialX, MaterialX generates it, and the result is
// executed (docs/architecture.md 1.1).
//
// A network that is not MaterialX -- a UsdPreviewSurface network, most often --
// is *reported*, not translated. hdClaude has no UsdPreviewSurface extractor and
// will not grow one; such a prim falls back to a generated diffuse material
// built from its displayColor, which is an honest "we did not shade this as
// authored" rather than a lookalike that quietly disagrees with the asset.

#include "texture_loader.h"

#include "hdclaude/gpu/glsl_compiler.h"
#include "hdclaude/gpu/path_tracer.h"

#include <MaterialXCore/Document.h>

#include "pxr/imaging/hd/material.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/path.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdClaudeMaterialCompiler {
  public:
    /// `shadeKernel` is PathTracer::ShadeKernelSource(): the generated material
    /// is joined to it to make a shading pipeline, so the ABI is defined in one
    /// place rather than restated here.
    HdClaudeMaterialCompiler(std::string shadeKernel);

    /// One texture a material samples: where it is, and how to read it.
    ///
    /// The colour space travels with the path because only the material knows
    /// it. The pool decodes and caches on the pair.
    struct TextureRequest {
        std::string path;
        HdClaudeTextureColorSpace colorSpace = HdClaudeTextureColorSpace::Auto;
    };

    struct Result {
        hdclaude::CompiledMaterial material;
        /// Empty when the authored network compiled. Otherwise says why it did
        /// not, in terms a user can act on.
        std::string fallbackReason;
        /// The textures the material samples, in the order the generator
        /// assigned their local indices. The caller loads these and fills in
        /// `material.textureSlots`.
        std::vector<TextureRequest> texturePaths;
    };

    /// Compile a Hydra material network.
    ///
    /// Falls back to a diffuse material of `fallbackColor` when the network has
    /// no MaterialX surface terminal, or when generation or compilation fails.
    Result Compile(const HdMaterialNetworkMap& network, const SdfPath& path,
                   const GfVec3f& fallbackColor);

    /// A diffuse material of a given colour, with no authored network behind
    /// it. Used for unbound geometry and as the delegate's fallback.
    hdclaude::CompiledMaterial CompileDiffuse(const GfVec3f& color,
                                              const std::string& name);

    /// True once the MaterialX libraries loaded. Everything fails cleanly if
    /// they did not, rather than crashing on a null document.
    bool Ready() const { return _libraries != nullptr; }

  private:
    /// Generate and compile one MaterialX document.
    ///
    /// `resolvedTextures` maps a generated sampler's uniform name to the
    /// resolved asset path Hydra supplied, which is the only place a relative
    /// path can still be anchored.
    hdclaude::CompiledMaterial CompileDocument(
        MaterialX::DocumentPtr document, const std::string& name,
        std::string* error,
        std::vector<TextureRequest>* texturePaths = nullptr,
        const std::map<std::string, std::string>* resolvedTextures = nullptr);

    /// Serialises generation and compilation.
    ///
    /// Hydra syncs Rprims in parallel, and an unbound mesh compiles its own
    /// displayColor material during Sync, so two threads can be inside this
    /// object at once. Neither MaterialX code generation nor glslang is
    /// thread-safe, and compilation is not on the interactive path, so one
    /// lock is the right trade rather than per-object copies of the generator.
    mutable std::mutex _mutex;
    std::string _shadeKernel;
    hdclaude::GlslCompiler _compiler;
    MaterialX::DocumentPtr _libraries;
};

PXR_NAMESPACE_CLOSE_SCOPE
