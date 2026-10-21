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
#include <MaterialXFormat/File.h>

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
    /// `shadeKernel` is PathTracer::ShadeKernelSource() and `displaceKernel`
    /// is PathTracer::DisplaceKernelSource(): a material's surface program is
    /// joined to the first and its displacement program to the second, so
    /// each ABI is defined in one place rather than restated here.
    HdClaudeMaterialCompiler(std::string shadeKernel,
                             std::string displaceKernel);

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
        /// The same for the displacement program, which numbers its own
        /// samplers from zero and fills `material.displaceTextureSlots`.
        std::vector<TextureRequest> displacementTexturePaths;
        /// Empty when the network authors no displacement, or says why one it
        /// does author could not be compiled. A displacement that fails is not
        /// a reason to fall back on the whole material: the surface is still
        /// the surface, and the mesh is simply not displaced.
        std::string displacementReason;
        /// What this material compiled from: its network, with node paths
        /// relative to the material and every input value. Two materials with
        /// the same key compile to the same program -- copies of an asset with
        /// its material embedded, or elements sharing a look -- so the second
        /// is not compiled at all, and the scene shades both with one.
        std::string contentKey;
        /// True when this result is an earlier compile of the same key.
        bool reused = false;
    };

    /// Compile a Hydra material network.
    ///
    /// Falls back to a diffuse material of `fallbackColor` when the network has
    /// no MaterialX surface terminal, or when generation or compilation fails.
    Result Compile(const HdMaterialNetworkMap& network, const SdfPath& path,
                   const GfVec3f& fallbackColor);

    /// A diffuse material of a given colour, with no authored network behind
    /// it. Used for unbound geometry and as the delegate's fallback.
    /// The one material for unbound geometry: diffuse, coloured by each
    /// placement's displayColor through a `displayColor` geomprop. Compiled
    /// once, where a colour baked into each program would compile a material
    /// per unbound mesh.
    hdclaude::CompiledMaterial CompileDisplayColor(const std::string& name);

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

    /// Generate and compile a document's *displacement* terminal.
    ///
    /// A separate entry point rather than a flag on the one above, because
    /// almost nothing is shared: it generates from the terminal the document
    /// names rather than from `findRenderableElements`, which answers with
    /// surfaces; it joins the displace kernel rather than the shade kernel;
    /// and it has its own texture order, because the two programs each number
    /// their samplers from zero.
    ///
    /// Writes the SPIR-V and the space into `material`, and leaves it
    /// untouched when the document authors no displacement. Returns false, and
    /// says why in `error`, only when a displacement that *was* authored could
    /// not be compiled.
    bool CompileDisplacement(
        MaterialX::DocumentPtr document, const std::string& name,
        hdclaude::CompiledMaterial* material, std::string* error,
        std::vector<TextureRequest>* texturePaths = nullptr,
        const std::map<std::string, std::string>* resolvedTextures = nullptr);

    /// Serialises generation and compilation.
    ///
    /// Hydra syncs Rprims in parallel, and an unbound mesh compiles its own
    /// displayColor material during Sync, so two threads can be inside this
    /// object at once. Neither MaterialX code generation nor glslang is
    /// thread-safe, and compilation is not on the interactive path, so one
    /// lock is the right trade rather than per-object copies of the generator.
    Result CompileUncached(const HdMaterialNetworkMap& network,
                           const SdfPath& path, const GfVec3f& fallbackColor);

    mutable std::mutex _mutex;
    /// Every network compiled so far, by content key. Held for the delegate's
    /// lifetime: a key names exactly what was compiled, so an entry cannot go
    /// stale, and a material switched away from and back to costs nothing.
    std::map<std::string, Result> _compiled;
    std::string _shadeKernel;
    std::string _displaceKernel;
    hdclaude::GlslCompiler _compiler;
    MaterialX::FileSearchPath _sourceSearchPath;
    MaterialX::DocumentPtr _libraries;
};

PXR_NAMESPACE_CLOSE_SCOPE
