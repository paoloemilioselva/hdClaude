cmake_minimum_required(VERSION 3.25)

project(hdClaude VERSION 0.1.0 LANGUAGES C CXX)

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
option(HDCLAUDE_WARNINGS_AS_ERRORS "Treat hdClaude compiler warnings as errors" OFF)

set(HDCLAUDE_OPENUSD_ROOT "$ENV{USDROOT}" CACHE PATH
    "OpenUSD install root. Defaults to the USDROOT set by setup_usd_env.bat.")
if(NOT HDCLAUDE_OPENUSD_ROOT)
  set(HDCLAUDE_OPENUSD_ROOT "C:/dev/usd-26.03" CACHE PATH "OpenUSD install root" FORCE)
endif()

# Dependencies are fetched into this tree and are never committed; see
# .gitignore and cmake/Dependencies.cmake. Pointing this at an existing
# populated tree reuses an earlier download instead of fetching again.
set(HDCLAUDE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps" CACHE PATH
    "Directory for fetched third-party dependencies. Never committed.")

set(HDCLAUDE_SHADER_CACHE_DIR "shader-cache" CACHE STRING
    "Default runtime SPIR-V cache directory")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
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
  add_subdirectory(src/gpu)
endif()

if(HDCLAUDE_BUILD_HYDRA_PLUGIN OR HDCLAUDE_ENABLE_MATERIALX)
  include(OpenUSD)
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
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
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
message(STATUS "  OpenUSD root ....... ${HDCLAUDE_OPENUSD_ROOT}")
message(STATUS "  Install prefix ..... ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Dependencies ....... ${HDCLAUDE_DEPS_DIR}")
message(STATUS "  Hydra plugin ....... ${HDCLAUDE_BUILD_HYDRA_PLUGIN}")
message(STATUS "  Vulkan backend ..... ${HDCLAUDE_ENABLE_VULKAN}")
message(STATUS "  MaterialX genglsl_pt ${HDCLAUDE_ENABLE_MATERIALX}")
message(STATUS "  NVIDIA DLSS ........ ${HDCLAUDE_ENABLE_DLSS}")
message(STATUS "  Testing ............ ${BUILD_TESTING}")
message(STATUS "")
