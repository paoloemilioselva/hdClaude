# ---------------------------------------------------------------------------
# NVIDIA DLSS (NGX) - optional reconstruction backend.
#
# The SDK is proprietary. It is never committed to this repository; it is
# referenced by a pinned tag and fetched into the gitignored dependency tree
# like every other dependency. Its runtime binaries are redistributed under
# NVIDIA's licence, which is why hdClaude ships none of them.
#
# Failure to obtain the SDK downgrades the feature and does not fail the
# build. That is deliberate: DLSS must never be a requirement of the default
# build (docs/architecture.md 9).
# ---------------------------------------------------------------------------

include_guard(GLOBAL)
include(FetchContent)

# A plain variable, not a cache entry. As a cache default the pin was read once,
# into the first configure, and a build tree kept that tag however the line
# here changed: moving from 310.3.0 to 310.9.1 edited this file and went on
# building against 310.3.0 without a word. A pinned version is the one
# committed. A different SDK is still one variable away: HDCLAUDE_DLSS_SDK.
set(HDCLAUDE_DLSS_TAG "v310.9.1")

# A different SDK, extracted by hand, used instead of the pinned fetch.
#
# Only while the variable is set. The override is a cache entry, so without the
# `else` it outlived the environment that asked for it and every later configure
# kept using that directory -- which, pointed at the fetch's own checkout, froze
# it at whatever tag it was first cloned at.
if(DEFINED ENV{HDCLAUDE_DLSS_SDK} AND EXISTS "$ENV{HDCLAUDE_DLSS_SDK}/include/nvsdk_ngx_vk.h")
  set(FETCHCONTENT_SOURCE_DIR_DLSS "$ENV{HDCLAUDE_DLSS_SDK}" CACHE PATH "" FORCE)
  message(STATUS "hdClaude: using the DLSS SDK at $ENV{HDCLAUDE_DLSS_SDK} "
                 "instead of the pinned ${HDCLAUDE_DLSS_TAG}")
else()
  unset(FETCHCONTENT_SOURCE_DIR_DLSS CACHE)
endif()

FetchContent_Declare(dlss
    GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
    GIT_TAG        ${HDCLAUDE_DLSS_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

# Populated directly rather than through FetchContent_MakeAvailable, because
# the SDK is a binary drop with no CMakeLists.txt of its own: MakeAvailable
# would find nothing to add_subdirectory and the policy warning it raises here
# is about a call this one deliberately makes. CMP0169 is set OLD for this
# file's scope so the deprecation does not turn into an error on a newer CMake.
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_GetProperties(dlss)
if(NOT dlss_POPULATED)
  FetchContent_Populate(dlss)
endif()

# Not inside a block().
#
# FetchContent_Populate sets dlss_SOURCE_DIR in its *calling* scope, and
# block() is a new variable scope, so wrapping the call made the variable
# empty everywhere below it. The check then asked whether
# "/include/nvsdk_ngx_vk.h" existed, which it never does, and the SDK was
# reported unobtainable immediately after being cloned successfully. The block
# was there as "an error guard", which it never was: block() scopes variables
# and catches nothing.
if(EXISTS "${dlss_SOURCE_DIR}/include/nvsdk_ngx_vk.h")
  # The NGX import library, at the path and CRT this SDK actually uses.
  #
  # `lib/Windows_x86_64/` holds `x64`, `dev`, `rel`, `khr`, `uwp` and a set of
  # `vsNNNN` directories that stop at vs2013; the modern MSVC import library is
  # in `x64`. An earlier `x86_64` here named a directory that has never
  # existed in this SDK, which the misscoped variable above kept anyone from
  # discovering.
  #
  # `_d` and not `_s`, which is a CRT choice and not a debug one. Read out of
  # the libraries themselves rather than inferred from the naming:
  # nvsdk_ngx_d.lib carries MSVCRT default-lib directives and nvsdk_ngx_s.lib
  # carries LIBCMT, so `_d` is the dynamic-CRT build and `_s` the static one.
  # hdClaude and OpenUSD are both /MD, so linking `_s` gives "mismatch detected
  # for RuntimeLibrary: value MT_StaticRelease does not match MD_DynamicRelease"
  # -- which nothing could discover until something actually linked NGX, and
  # nothing did until 2026-09-08.
  #
  # The `_dbg` variants are the debug CRT, so that half follows the
  # configuration rather than being fixed at the release one.
  set(_ngx_lib_dir "${dlss_SOURCE_DIR}/lib/Windows_x86_64/x64")
  if(NOT EXISTS "${_ngx_lib_dir}/nvsdk_ngx_d.lib")
    message(FATAL_ERROR
        "hdClaude: the DLSS SDK at ${dlss_SOURCE_DIR} has its headers but not "
        "${_ngx_lib_dir}/nvsdk_ngx_d.lib. The SDK's layout has changed; "
        "cmake/NvidiaDLSS.cmake names the path and has to be corrected rather "
        "than the feature quietly turned off.")
  endif()

  add_library(hdClaudeNgx INTERFACE)
  target_include_directories(hdClaudeNgx INTERFACE "${dlss_SOURCE_DIR}/include")
  target_link_libraries(hdClaudeNgx INTERFACE
      "$<IF:$<CONFIG:Debug>,${_ngx_lib_dir}/nvsdk_ngx_d_dbg.lib,${_ngx_lib_dir}/nvsdk_ngx_d.lib>")
  target_compile_definitions(hdClaudeNgx INTERFACE HDCLAUDE_HAS_DLSS=1)

  # Where the runtime models live, for the support query and for NGX's own
  # loader. hdClaude ships none of these -- redistribution is governed by
  # NVIDIA's licence -- so this points at the fetched tree rather than copying
  # anything out of it. `rel` is the shipping build; `dev` is the one that
  # writes an overlay and a log, and is what a developer points NGX at when a
  # feature refuses to initialise.
  set(HDCLAUDE_DLSS_RUNTIME_DIR "${dlss_SOURCE_DIR}/lib/Windows_x86_64/rel"
      CACHE PATH "Directory holding nvngx_dlss.dll and nvngx_dlssd.dll")
  target_compile_definitions(hdClaudeNgx INTERFACE
      HDCLAUDE_DLSS_RUNTIME_DIR="${HDCLAUDE_DLSS_RUNTIME_DIR}")

  set(HDCLAUDE_DLSS_AVAILABLE TRUE CACHE INTERNAL "")
  message(STATUS "hdClaude: NVIDIA DLSS SDK at ${dlss_SOURCE_DIR}")
else()
  set(HDCLAUDE_DLSS_AVAILABLE FALSE CACHE INTERNAL "")
  set(HDCLAUDE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
  message(WARNING
      "hdClaude: the NVIDIA DLSS SDK could not be obtained. "
      "Building without DLSS; the renderer-native reconstruction backend is "
      "unaffected. Set HDCLAUDE_DLSS_SDK to a local SDK root to enable it.")
endif()
