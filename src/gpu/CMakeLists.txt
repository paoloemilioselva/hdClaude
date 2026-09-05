# ---------------------------------------------------------------------------
# hdClaudeGpu
#
# Vulkan device ownership, resources, acceleration structures, and the
# wavefront integrator. Knows nothing about OpenUSD: this library never
# includes a `pxr` header, which is what allows the GPU backend to be tested
# without a USD runtime and keeps the layering in docs/architecture.md 3
# enforceable by the compiler rather than by convention.
# ---------------------------------------------------------------------------

add_library(hdClaudeGpu STATIC
    compute_pipeline.cpp
    glsl_compiler.cpp
    vulkan_context.cpp
    vulkan_memory.cpp
    vulkan_resources.cpp
)

add_library(hdClaude::Gpu ALIAS hdClaudeGpu)

target_include_directories(hdClaudeGpu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(hdClaudeGpu
    PUBLIC
        hdClaudeProjectOptions
        hdClaude::Core
        volk
        Vulkan::Headers
    PRIVATE
        hdClaudeVma
        glslang
        SPIRV
        glslang-default-resource-limits
)

# volk loads every entry point at runtime, so nothing links vulkan-1. A machine
# with no Vulkan runtime must fail with a clear message from our own code, not
# fail to load the plugin at all.
target_compile_definitions(hdClaudeGpu PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
  target_compile_definitions(hdClaudeGpu PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()

set_target_properties(hdClaudeGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
