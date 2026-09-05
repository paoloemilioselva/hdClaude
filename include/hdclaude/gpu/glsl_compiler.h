// Runtime GLSL to SPIR-V compilation.
//
// MaterialX generates GLSL for every bound material at load time, so this
// compiler is on the critical path of the first frame rather than being a
// build-time tool. It is deliberately a thin, explicit wrapper: the target
// environment, the SPIR-V version, and the optimisation mode are all inputs to
// the shader cache key (docs/materialx-codegen.md 6), so they cannot be
// defaulted somewhere invisible.

#ifndef HDCLAUDE_GPU_GLSL_COMPILER_H
#define HDCLAUDE_GPU_GLSL_COMPILER_H

#include <cstdint>
#include <string>
#include <vector>

#include "hdclaude/core/shader_cache.h"

namespace hdclaude {

enum class ShaderStage {
    Compute,
    Vertex,
    Fragment,
};

struct GlslCompileOptions {
    ShaderStage stage = ShaderStage::Compute;
    std::string entryPoint = "main";
    /// Names the module in diagnostics. Use the material or kernel name.
    std::string moduleName = "shader";
    /// Generate debug info and skip optimisation. Off for gallery renders.
    bool generateDebugInfo = false;
};

struct GlslCompileResult {
    bool ok = false;
    std::vector<std::uint32_t> spirv;
    /// Compiler and linker diagnostics. Populated on success too, since
    /// glslang emits warnings that a material author needs to see.
    std::string log;
};

/// Compiles GLSL to SPIR-V for Vulkan 1.3 / SPIR-V 1.6.
///
/// Thread-safe. glslang's process-wide initialisation is reference counted
/// internally, so several compilers may exist at once; materials are compiled
/// in parallel during scene load.
class GlslCompiler {
  public:
    GlslCompiler();
    ~GlslCompiler();

    GlslCompiler(const GlslCompiler&) = delete;
    GlslCompiler& operator=(const GlslCompiler&) = delete;

    /// Compile. Never throws on a shader error: a material that fails to
    /// compile must be reported with its diagnostics, naming the material, not
    /// unwound through the scene loader.
    GlslCompileResult Compile(const std::string& source,
                              const GlslCompileOptions& options) const;

    /// Compile through a cache. On a miss, compiles and stores. The key's
    /// compiler-version, SPIR-V-version, and target fields are filled in from
    /// this compiler, so a toolchain change cannot serve a stale entry.
    GlslCompileResult CompileCached(const std::string& source,
                                    const GlslCompileOptions& options,
                                    ShaderCache& cache,
                                    ShaderCacheKey key) const;

    /// Version string identifying this compiler build, for the cache key.
    static std::string Version();

    /// Encoded SPIR-V and Vulkan target versions, for the cache key.
    static std::uint32_t SpirvVersion();
    static std::uint32_t VulkanVersion();
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_GLSL_COMPILER_H
