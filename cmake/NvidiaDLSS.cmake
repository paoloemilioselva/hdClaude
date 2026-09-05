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

set(HDCLAUDE_DLSS_TAG "v310.3.0" CACHE STRING "NVIDIA DLSS SDK release tag")

# An already-extracted SDK, from an earlier fetch or a manual download.
if(DEFINED ENV{HDCLAUDE_DLSS_SDK} AND EXISTS "$ENV{HDCLAUDE_DLSS_SDK}/include/nvsdk_ngx_vk.h")
  set(FETCHCONTENT_SOURCE_DIR_DLSS "$ENV{HDCLAUDE_DLSS_SDK}" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(dlss
    GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
    GIT_TAG        ${HDCLAUDE_DLSS_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_GetProperties(dlss)
if(NOT dlss_POPULATED)
  # Populate under an error guard so an unavailable or declined SDK turns the
  # feature off rather than stopping the configure.
  block()
    set(_dlss_ok TRUE)
    FetchContent_Populate(dlss)
  endblock()
endif()

if(EXISTS "${dlss_SOURCE_DIR}/include/nvsdk_ngx_vk.h")
  add_library(hdClaudeNgx INTERFACE)
  target_include_directories(hdClaudeNgx INTERFACE "${dlss_SOURCE_DIR}/include")
  target_link_libraries(hdClaudeNgx INTERFACE
      "${dlss_SOURCE_DIR}/lib/Windows_x86_64/x86_64/nvsdk_ngx_s.lib")
  target_compile_definitions(hdClaudeNgx INTERFACE HDCLAUDE_HAS_DLSS=1)
  set(HDCLAUDE_DLSS_AVAILABLE TRUE CACHE INTERNAL "")
  message(STATUS "hdClaude: NVIDIA DLSS SDK ${HDCLAUDE_DLSS_TAG} at ${dlss_SOURCE_DIR}")
else()
  set(HDCLAUDE_DLSS_AVAILABLE FALSE CACHE INTERNAL "")
  set(HDCLAUDE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
  message(WARNING
      "hdClaude: the NVIDIA DLSS SDK could not be obtained. "
      "Building without DLSS; the renderer-native reconstruction backend is "
      "unaffected. Set HDCLAUDE_DLSS_SDK to a local SDK root to enable it.")
endif()
