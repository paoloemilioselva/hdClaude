# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Policy: nothing third-party is committed to this repository. Every dependency
# is *referenced* here by URL and an exact pinned tag, and fetched into
# HDCLAUDE_DEPS_DIR (gitignored) by CMake. A fresh clone therefore reproduces
# the same tree with a single configure, and the repository stays small and
# free of third-party or proprietary code.
#
# Reusing an existing download: set HDCLAUDE_DEPS_DIR to a directory that
# already contains populated <name>-src trees, or set FETCHCONTENT_SOURCE_DIR_<NAME>
# to point one dependency at a local checkout. Both are standard CMake and
# neither requires editing this file.
#
# Offline builds: configure with -DFETCHCONTENT_FULLY_DISCONNECTED=ON once the
# dependencies are populated.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)
include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${HDCLAUDE_DEPS_DIR}" CACHE PATH "" FORCE)
set(FETCHCONTENT_QUIET OFF)

# Pinned versions. Vulkan-Headers, volk, and glslang are held at the same
# Vulkan SDK release so that headers, loader metadata, and the SPIR-V compiler
# cannot disagree about what Vulkan 1.3/SPIR-V 1.6 means.
set(HDCLAUDE_VULKAN_SDK_TAG "vulkan-sdk-1.4.357.0" CACHE STRING
    "Vulkan SDK release tag used for Vulkan-Headers, volk, and glslang")
set(HDCLAUDE_VMA_TAG "v3.2.1" CACHE STRING
    "VulkanMemoryAllocator release tag")

# ---------------------------------------------------------------------------
# Vulkan-Headers - the API definition. Header-only.
# ---------------------------------------------------------------------------
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        ${HDCLAUDE_VULKAN_SDK_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

# ---------------------------------------------------------------------------
# volk - meta-loader. hdClaude loads Vulkan entry points dynamically rather
# than linking vulkan-1.lib, so the plugin loads on a machine with no Vulkan
# runtime and reports that cleanly instead of failing to load at all.
# ---------------------------------------------------------------------------
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        ${HDCLAUDE_VULKAN_SDK_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator - suballocation. A wavefront path tracer allocates a
# small number of very large buffers; VMA is used for its budget tracking and
# defragmentation-safe mapping rather than for suballocation density.
# ---------------------------------------------------------------------------
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        ${HDCLAUDE_VMA_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

# ---------------------------------------------------------------------------
# glslang - runtime GLSL to SPIR-V. Required, not optional: MaterialX
# generates GLSL at runtime for every material, so the compiler is on the
# critical path rather than a build-time tool.
# ---------------------------------------------------------------------------
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)  # no spirv-opt dependency
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST            OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        ${HDCLAUDE_VULKAN_SDK_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(vulkan_headers volk vma glslang)

# ---------------------------------------------------------------------------
# VMA ships headers plus an implementation macro; give it a real target.
# ---------------------------------------------------------------------------
if(NOT TARGET hdClaudeVma)
  add_library(hdClaudeVma INTERFACE)
  target_include_directories(hdClaudeVma INTERFACE "${vma_SOURCE_DIR}/include")
  target_link_libraries(hdClaudeVma INTERFACE Vulkan::Headers)
  # volk loads the entry points, so VMA must be told to fetch them dynamically
  # rather than assume a statically linked loader.
  target_compile_definitions(hdClaudeVma INTERFACE
      VMA_STATIC_VULKAN_FUNCTIONS=0
      VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
endif()

# ---------------------------------------------------------------------------
# Optional: NVIDIA DLSS. Proprietary, never committed, and never a requirement
# of the default build. See cmake/NvidiaDLSS.cmake and docs/dlss-integration.md.
# ---------------------------------------------------------------------------
if(HDCLAUDE_ENABLE_DLSS)
  include(NvidiaDLSS)
endif()
