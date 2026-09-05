#include "hdclaude/gpu/glsl_compiler.h"

#include <atomic>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace hdclaude {
namespace {

/// glslang requires process-wide initialisation exactly once, and finalisation
/// after the last user. Reference counted here so several compilers can exist
/// and be destroyed in any order.
std::mutex& ProcessMutex()
{
    static std::mutex mutex;
    return mutex;
}
int& ProcessRefCount()
{
    static int count = 0;
    return count;
}

EShLanguage ToGlslang(ShaderStage stage)
{
    switch (stage) {
        case ShaderStage::Vertex: return EShLangVertex;
        case ShaderStage::Fragment: return EShLangFragment;
        case ShaderStage::Compute: break;
    }
    return EShLangCompute;
}

const char* StageName(ShaderStage stage)
{
    switch (stage) {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute: break;
    }
    return "compute";
}

}  // namespace

GlslCompiler::GlslCompiler()
{
    std::lock_guard<std::mutex> lock(ProcessMutex());
    if (ProcessRefCount()++ == 0) {
        glslang::InitializeProcess();
    }
}

GlslCompiler::~GlslCompiler()
{
    std::lock_guard<std::mutex> lock(ProcessMutex());
    if (--ProcessRefCount() == 0) {
        glslang::FinalizeProcess();
    }
}

std::string GlslCompiler::Version()
{
    return std::string("glslang-") + glslang::GetGlslVersionString();
}

std::uint32_t GlslCompiler::SpirvVersion() { return 0x00010600u; }  // SPIR-V 1.6

std::uint32_t GlslCompiler::VulkanVersion()
{
    // VK_MAKE_API_VERSION(0, 1, 3, 0), written out so this translation unit
    // needs no Vulkan headers. The compiler depends on glslang alone, which
    // keeps it usable from the MaterialX layer without dragging the GPU
    // backend's headers along.
    return 0x00403000u;
}

GlslCompileResult GlslCompiler::Compile(const std::string& source,
                                        const GlslCompileOptions& options) const
{
    GlslCompileResult result;

    const EShLanguage stage = ToGlslang(options.stage);
    glslang::TShader shader(stage);

    const char* sources[] = {source.c_str()};
    const char* names[] = {options.moduleName.c_str()};
    const int lengths[] = {static_cast<int>(source.size())};
    shader.setStringsWithLengthsAndNames(sources, lengths, names, 1);
    shader.setEntryPoint(options.entryPoint.c_str());
    shader.setSourceEntryPoint(options.entryPoint.c_str());

    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (options.generateDebugInfo) {
        messages = static_cast<EShMessages>(messages | EShMsgDebugInfo);
    }

    // MaterialX resolves its own #include directives during generation, so the
    // source arriving here is already a single translation unit and no includer
    // is required. If that ever stops being true it will surface as an
    // unresolved include error naming the file, not as silent wrong behaviour.
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        result.log = std::string("GLSL parse failed for ") + options.moduleName +
                     " (" + StageName(options.stage) + "):\n" +
                     shader.getInfoLog() + shader.getInfoDebugLog();
        return result;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log = std::string("GLSL link failed for ") + options.moduleName +
                     ":\n" + program.getInfoLog() + program.getInfoDebugLog();
        return result;
    }
    if (!program.mapIO()) {
        result.log =
            std::string("GLSL IO mapping failed for ") + options.moduleName;
        return result;
    }

    glslang::SpvOptions spvOptions;
    spvOptions.generateDebugInfo = options.generateDebugInfo;
    spvOptions.stripDebugInfo = !options.generateDebugInfo;
    spvOptions.disableOptimizer = true;   // ENABLE_OPT is off; see Dependencies.cmake
    spvOptions.optimizeSize = false;
    spvOptions.validate = true;

    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(stage), result.spirv, &logger,
                          &spvOptions);

    result.log = logger.getAllMessages();
    if (const char* info = shader.getInfoLog(); info != nullptr && *info != '\0') {
        result.log += info;
    }

    // A module that produced no words, or that does not begin with the SPIR-V
    // magic number, is a failure however glslang reported it.
    result.ok = !result.spirv.empty() && result.spirv.front() == 0x07230203u;
    if (!result.ok && result.log.empty()) {
        result.log = "SPIR-V generation produced no module for " + options.moduleName;
    }
    return result;
}

GlslCompileResult GlslCompiler::CompileCached(const std::string& source,
                                              const GlslCompileOptions& options,
                                              ShaderCache& cache,
                                              ShaderCacheKey key) const
{
    // Fields the caller cannot know are filled in here, so a toolchain change
    // invalidates every entry without the caller having to remember.
    key.source = source;
    key.entryPoint = options.entryPoint;
    key.stage = StageName(options.stage);
    key.compilerVersion = Version();
    key.spirvVersion = SpirvVersion();
    key.vulkanVersion = VulkanVersion();
    key.optimize = !options.generateDebugInfo;

    if (auto cached = cache.Find(key)) {
        GlslCompileResult result;
        result.ok = true;
        result.spirv = std::move(*cached);
        return result;
    }

    GlslCompileResult result = Compile(source, options);
    if (result.ok) {
        // A cache write failure is not a compilation failure: the shader is
        // valid and rendering proceeds, just without the saving next time.
        cache.Store(key, result.spirv);
    }
    return result;
}

}  // namespace hdclaude
