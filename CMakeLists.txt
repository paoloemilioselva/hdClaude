cmake_minimum_required(VERSION 3.25)

project(hdClaude VERSION 1.0.1 LANGUAGES C CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Options
#
# The default build is portable: no vendor-specific requirement is enabled by
# default. HDCLAUDE_ENABLE_DLSS is opt-in and downgrades cleanly if the SDK is
# unavailable, so a clone without network access still builds a working
# renderer.
# ---------------------------------------------------------------------------
option(HDCLAUDE_BUILD_HYDRA_PLUGIN "Build the OpenUSD Hydra render delegate" ON)
option(HDCLAUDE_ENABLE_VULKAN      "Build the Vulkan ray-query GPU backend" ON)
option(HDCLAUDE_ENABLE_MATERIALX   "Build the MaterialX genglsl_pt shader generator" ON)
option(HDCLAUDE_ENABLE_DLSS        "Build the optional NVIDIA DLSS reconstruction backend" OFF)
option(HDCLAUDE_BUILD_VALIDATION_LAYERS
       "Build the Khronos Vulkan validation layers from source" OFF)
option(HDCLAUDE_WARNINGS_AS_ERRORS "Treat hdClaude compiler warnings as errors" OFF)
option(HDCLAUDE_BUILD_CULL_VOLUMES
       "Build the usdCullVolume schema and its UsdImaging adapter (extensions/cullVolume)" ON)

# Building against the USD inside a Houdini installation rather than a
# standalone one: its version, such as 22.0 (cmake/HoudiniUSD.cmake). Empty
# for a standalone OpenUSD, which HDCLAUDE_OPENUSD_ROOT then names.
set(HDCLAUDE_HOUDINI_VERSION "" CACHE STRING
    "Build against this Houdini version's USD, e.g. 22.0. Empty: standalone OpenUSD.")

set(HDCLAUDE_OPENUSD_ROOT "$ENV{USDROOT}" CACHE PATH
    "OpenUSD install root. Defaults to the USDROOT set by setup_usd_env.bat/.sh.")
if(NOT HDCLAUDE_OPENUSD_ROOT AND WIN32)
  set(HDCLAUDE_OPENUSD_ROOT "C:/dev/usd-26.03" CACHE PATH "OpenUSD install root" FORCE)
endif()

# Dependencies are fetched into this tree and are never committed; see
# .gitignore and cmake/Dependencies.cmake. Pointing this at an existing
# populated tree reuses an earlier download instead of fetching again.
set(HDCLAUDE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps" CACHE PATH
    "Directory for fetched third-party dependencies. Never committed.")

set(HDCLAUDE_SHADER_CACHE_DIR "shader-cache" CACHE STRING
    "Default runtime SPIR-V cache directory")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT HDCLAUDE_HOUDINI_VERSION)
  if(DEFINED ENV{USDEXTRA})
    set(CMAKE_INSTALL_PREFIX "$ENV{USDEXTRA}" CACHE PATH "hdClaude install prefix" FORCE)
  endif()
endif()

# ---------------------------------------------------------------------------
# Shared compile options
# ---------------------------------------------------------------------------
add_library(hdClaudeProjectOptions INTERFACE)
target_compile_features(hdClaudeProjectOptions INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(hdClaudeProjectOptions INTERFACE
      /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
  target_compile_definitions(hdClaudeProjectOptions INTERFACE
      NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
  if(HDCLAUDE_WARNINGS_AS_ERRORS)
    target_compile_options(hdClaudeProjectOptions INTERFACE /WX)
  endif()
else()
  target_compile_options(hdClaudeProjectOptions INTERFACE -Wall -Wextra -Wpedantic)
  if(HDCLAUDE_WARNINGS_AS_ERRORS)
    target_compile_options(hdClaudeProjectOptions INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Subsystems, in dependency order.
#
# src/core has no Vulkan and no OpenUSD dependency and always builds. This is
# what makes the core-only preset useful: hashing, the shader cache, spectral
# upsampling, and the display transform are testable with neither a GPU nor a
# USD runtime present.
# ---------------------------------------------------------------------------
add_subdirectory(src/core)

if(HDCLAUDE_ENABLE_VULKAN)
  include(Dependencies)

  # ---------------------------------------------------------------------------
  # Validation layers.
  #
  # Validation is a gate, not a convenience: the GPU tests fail without it
  # (docs/lessons-from-hdcodex.md R8). So the layer is located here, and if it
  # cannot be located the configure says exactly how to obtain it rather than
  # letting the failure surface later as a puzzling test failure.
  #
  # Search order: an explicit override, an already-built source tree, a
  # discovered SDK. Building from source is opt-in because it is a long build.
  # ---------------------------------------------------------------------------
  set(HDCLAUDE_VALIDATION_LAYER_DIR "" CACHE PATH
      "Directory containing VkLayer_khronos_validation.json")

  # The manifest sits beside the DLL on Windows. Elsewhere it lives in
  # share/vulkan/explicit_layer.d, and names a bare libVkLayer_khronos_validation.so
  # that the loader finds on the library search path -- so pointing
  # VK_LAYER_PATH at the manifest is not enough, and the tests also need
  # HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR on LD_LIBRARY_PATH.
  if(WIN32)
    set(_layer_candidates
        "$ENV{HDCLAUDE_VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "${HDCLAUDE_DEPS_DIR}/validation-layers/install/bin")
  else()
    set(_layer_candidates
        "$ENV{HDCLAUDE_VULKAN_SDK}/share/vulkan/explicit_layer.d"
        "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d"
        "${HDCLAUDE_DEPS_DIR}/validation-layers/install/share/vulkan/explicit_layer.d"
        "/usr/local/share/vulkan/explicit_layer.d"
        "/usr/share/vulkan/explicit_layer.d")
  endif()

  if(HDCLAUDE_BUILD_VALIDATION_LAYERS)
    include(ValidationLayers)
  elseif(NOT HDCLAUDE_VALIDATION_LAYER_DIR)
    foreach(_candidate ${_layer_candidates})
      if(NOT HDCLAUDE_VALIDATION_LAYER_DIR AND
         EXISTS "${_candidate}/VkLayer_khronos_validation.json")
        set(HDCLAUDE_VALIDATION_LAYER_DIR "${_candidate}" CACHE PATH "" FORCE)
      endif()
    endforeach()
  endif()

  # <root>/share/vulkan/explicit_layer.d -> <root>/lib. Unused on Windows,
  # where the loader resolves the manifest's library path relative to it.
  if(HDCLAUDE_VALIDATION_LAYER_DIR AND NOT WIN32)
    cmake_path(GET HDCLAUDE_VALIDATION_LAYER_DIR PARENT_PATH _layer_share)
    cmake_path(GET _layer_share PARENT_PATH _layer_share)
    cmake_path(GET _layer_share PARENT_PATH _layer_root)
    set(HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR "${_layer_root}/lib")
  endif()

  if(HDCLAUDE_VALIDATION_LAYER_DIR AND NOT HDCLAUDE_BUILD_VALIDATION_LAYERS)
    message(STATUS "hdClaude: validation layer found at ${HDCLAUDE_VALIDATION_LAYER_DIR}")
  elseif(NOT HDCLAUDE_BUILD_VALIDATION_LAYERS)
    message(STATUS
        "hdClaude: no Vulkan validation layer found. The GPU tests will fail "
        "without one, by design. Configure with "
        "-DHDCLAUDE_BUILD_VALIDATION_LAYERS=ON to build it from source, or set "
        "VULKAN_SDK / HDCLAUDE_VALIDATION_LAYER_DIR. See docs/building.md.")
  endif()

  add_subdirectory(src/gpu)
endif()

# Where the plugin tree goes under the install prefix: beside OpenUSD's own
# plugins, or where Houdini looks for them.
set(HDCLAUDE_PLUGIN_INSTALL_DIR "plugin/usd")
if(HDCLAUDE_BUILD_HYDRA_PLUGIN OR HDCLAUDE_ENABLE_MATERIALX)
  if(HDCLAUDE_HOUDINI_VERSION)
    include(HoudiniUSD)
    set(HDCLAUDE_PLUGIN_INSTALL_DIR "dso/usd_plugins")
    # The prefix follows the user directory unless someone chose one: a
    # prefix this derived on an earlier configure is derived again, so a
    # corrected user directory reaches an existing build tree.
    set(_houdini_prefix "${HDCLAUDE_HOUDINI_USER_PREF_DIR}/hdClaude")
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR
       CMAKE_INSTALL_PREFIX STREQUAL HDCLAUDE_DERIVED_INSTALL_PREFIX)
      set(CMAKE_INSTALL_PREFIX "${_houdini_prefix}"
          CACHE PATH "hdClaude install prefix" FORCE)
    endif()
    set(HDCLAUDE_DERIVED_INSTALL_PREFIX "${_houdini_prefix}" CACHE INTERNAL "")
  else()
    include(OpenUSD)
  endif()
endif()

if(HDCLAUDE_ENABLE_MATERIALX)
  if(NOT HDCLAUDE_ENABLE_VULKAN)
    message(FATAL_ERROR
        "HDCLAUDE_ENABLE_MATERIALX requires HDCLAUDE_ENABLE_VULKAN: the "
        "genglsl_pt target compiles to SPIR-V through the GPU subsystem.")
  endif()
  add_subdirectory(src/materialx)
endif()

if(HDCLAUDE_BUILD_HYDRA_PLUGIN)
  if(NOT HDCLAUDE_ENABLE_MATERIALX)
    message(FATAL_ERROR
        "HDCLAUDE_BUILD_HYDRA_PLUGIN requires HDCLAUDE_ENABLE_MATERIALX: "
        "MaterialX is the only shading path (see docs/architecture.md 1.1).")
  endif()
  add_subdirectory(src/hydra)
  if(HDCLAUDE_BUILD_CULL_VOLUMES)
    add_subdirectory(extensions/cullVolume)
  endif()
endif()

# The image tools serve the test and gallery scripts, which run against a
# standalone OpenUSD; built against Houdini they would only run inside it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt" AND
   NOT HDCLAUDE_HOUDINI_VERSION)
  add_subdirectory(tools)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary. Printed so a build's feature set is never a mystery.
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "hdClaude ${PROJECT_VERSION} configuration")
if(HDCLAUDE_HOUDINI_VERSION)
  message(STATUS "  Houdini ............ ${HDCLAUDE_HOUDINI_VERSION} at ${HDCLAUDE_HOUDINI_ROOT}")
else()
  message(STATUS "  OpenUSD root ....... ${HDCLAUDE_OPENUSD_ROOT}")
endif()
message(STATUS "  Install prefix ..... ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Dependencies ....... ${HDCLAUDE_DEPS_DIR}")
message(STATUS "  Hydra plugin ....... ${HDCLAUDE_BUILD_HYDRA_PLUGIN}")
message(STATUS "  Vulkan backend ..... ${HDCLAUDE_ENABLE_VULKAN}")
message(STATUS "  MaterialX genglsl_pt ${HDCLAUDE_ENABLE_MATERIALX}")
if(HDCLAUDE_ENABLE_MATERIALX)
  message(STATUS "  MaterialX libraries  ${HDCLAUDE_MATERIALX_LIBRARIES}")
endif()
message(STATUS "  Cull volumes ....... ${HDCLAUDE_BUILD_CULL_VOLUMES}")
message(STATUS "  NVIDIA DLSS ........ ${HDCLAUDE_ENABLE_DLSS}")
if(HDCLAUDE_ENABLE_VULKAN)
  if(HDCLAUDE_BUILD_VALIDATION_LAYERS)
    message(STATUS "  Validation layer ... building from source")
  elseif(HDCLAUDE_VALIDATION_LAYER_DIR)
    message(STATUS "  Validation layer ... ${HDCLAUDE_VALIDATION_LAYER_DIR}")
  else()
    message(STATUS "  Validation layer ... NOT FOUND (GPU tests will fail)")
  endif()
endif()
message(STATUS "  Testing ............ ${BUILD_TESTING}")
message(STATUS "")
