// GLSL to SPIR-V compilation tests.
//
// These need glslang but no GPU and no OpenUSD, so a material's generated code
// can be checked for compilability on any machine.

#include "test_support.h"

#include "hdclaude/gpu/glsl_compiler.h"

#include <cstdio>
#include <filesystem>

using namespace hdclaude;

namespace {

const char* kValidCompute = R"(#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Values { float values[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    values[i] = values[i] * 2.0;
}
)";

const char* kInvalidCompute = R"(#version 450
layout(local_size_x = 64) in;
void main() {
    undeclaredFunction(qux);
}
)";

/// A compute shader exercising the Vulkan 1.3 / SPIR-V 1.6 features hdClaude's
/// kernels actually rely on: scalar block layout, buffer references, and
/// descriptor indexing. If the target environment were misconfigured, this is
/// the test that would notice rather than the first kernel written.
const char* kTargetFeatureCompute = R"(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
// Required for the uint64_t that holds a buffer device address. Easy to omit,
// and the resulting diagnostic ("syntax error, unexpected IDENTIFIER") points
// at the declaration rather than the missing extension, so it is asserted here
// once for every kernel that uses an address.
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(local_size_x = 64) in;

layout(buffer_reference, scalar) buffer PathState {
    vec4 throughput[];
};

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 1, scalar) uniform Frame {
    uint64_t pathStateAddress;
    uint textureIndex;
} frame;

layout(set = 0, binding = 2, std430) buffer Output { vec4 result[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    PathState state = PathState(frame.pathStateAddress);
    vec4 t = state.throughput[i];
    result[i] = t * texture(textures[nonuniformEXT(frame.textureIndex)], vec2(0.5));
}
)";

void TestValidShaderCompiles(const GlslCompiler& compiler)
{
    GlslCompileOptions options;
    options.moduleName = "test.valid";

    const GlslCompileResult result = compiler.Compile(kValidCompute, options);
    if (!result.ok) {
        std::fprintf(stderr, "  compile log:\n%s\n", result.log.c_str());
    }
    CHECK(result.ok);
    CHECK(!result.spirv.empty());
    CHECK_EQ(result.spirv.front(), std::uint32_t(0x07230203u));

    // Word 1 of a SPIR-V module is the version. hdClaude targets SPIR-V 1.6,
    // and a silently lower target would fail much later, inside a driver.
    CHECK_EQ(result.spirv[1], std::uint32_t(0x00010600u));
}

void TestInvalidShaderReportsRatherThanThrows(const GlslCompiler& compiler)
{
    // A material that fails to compile must be reported with diagnostics naming
    // it, not unwound through the scene loader (docs/materialx-codegen.md 6).
    GlslCompileOptions options;
    options.moduleName = "test.invalid";

    const GlslCompileResult result = compiler.Compile(kInvalidCompute, options);
    CHECK(!result.ok);
    CHECK(result.spirv.empty());
    CHECK(!result.log.empty());
    CHECK(result.log.find("test.invalid") != std::string::npos);
}

void TestTargetFeaturesAreAvailable(const GlslCompiler& compiler)
{
    GlslCompileOptions options;
    options.moduleName = "test.targetFeatures";

    const GlslCompileResult result = compiler.Compile(kTargetFeatureCompute, options);
    if (!result.ok) {
        std::fprintf(stderr, "  compile log:\n%s\n", result.log.c_str());
    }
    CHECK(result.ok);
}

void TestCacheServesTheSecondCompile(const GlslCompiler& compiler)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "hdclaude-glsl-cache-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    ShaderCache cache(directory);
    GlslCompileOptions options;
    options.moduleName = "test.cached";

    ShaderCacheKey key;
    key.generatorTarget = "genglsl_pt";
    key.generatorVersion = "1.39.3";
    key.abiVersion = 1;

    const GlslCompileResult first =
        compiler.CompileCached(kValidCompute, options, cache, key);
    CHECK(first.ok);
    CHECK_EQ(cache.MissCount(), std::uint64_t(1));
    CHECK_EQ(cache.HitCount(), std::uint64_t(0));

    const GlslCompileResult second =
        compiler.CompileCached(kValidCompute, options, cache, key);
    CHECK(second.ok);
    CHECK_EQ(cache.HitCount(), std::uint64_t(1));
    CHECK(second.spirv == first.spirv);

    // A change to the material ABI must not be served from cache, even though
    // the source is byte-identical.
    ShaderCacheKey changedAbi = key;
    changedAbi.abiVersion = 2;
    const GlslCompileResult third =
        compiler.CompileCached(kValidCompute, options, cache, changedAbi);
    CHECK(third.ok);
    CHECK_EQ(cache.MissCount(), std::uint64_t(2));

    std::filesystem::remove_all(directory, error);
}

}  // namespace

int main()
{
    std::printf("hdClaudeGlslCompilerTests\n");
    const GlslCompiler compiler;
    std::printf("  compiler ....... %s\n", GlslCompiler::Version().c_str());

    TestValidShaderCompiles(compiler);
    TestInvalidShaderReportsRatherThanThrows(compiler);
    TestTargetFeaturesAreAvailable(compiler);
    TestCacheServesTheSecondCompile(compiler);

    return hdclaude_test::Summarize("hdClaudeGlslCompilerTests");
}
