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
    acceleration_structure.cpp
    compute_pipeline.cpp
    environment_distribution.cpp
    glsl_compiler.cpp
    ngx_support.cpp
    path_tracer.cpp
    vertex_frames.cpp
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

# The kernel sources are read at runtime rather than embedded, so a shader can
# be edited and re-run without relinking. The Hydra plugin installs them beside
# itself; this is the in-tree location.
target_compile_definitions(hdClaudeGpu PUBLIC
    HDCLAUDE_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")

# The optional NGX backend.
#
# Linked PRIVATE, and its HDCLAUDE_HAS_DLSS define reaches only this library:
# nothing above hdClaudeGpu may see an NVIDIA type or change shape depending on
# whether the SDK was found (docs/dlss-integration.md 2). Absent, ngx_support.cpp
# compiles its stubs and every caller behaves the same.
if(HDCLAUDE_ENABLE_DLSS AND HDCLAUDE_DLSS_AVAILABLE)
  target_link_libraries(hdClaudeGpu PRIVATE hdClaudeNgx)
  # version.lib, for reading which nvngx_dlss.dll is about to be loaded. NGX has
  # no call that reports its own version, and the version is the model, so the
  # answer comes off the file itself.
  if(WIN32)
    target_link_libraries(hdClaudeGpu PRIVATE version)
  endif()
endif()

set_target_properties(hdClaudeGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
