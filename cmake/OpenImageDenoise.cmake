# ---------------------------------------------------------------------------
# Intel Open Image Denoise (OIDN) - the final-frame denoiser.
#
# Apache 2.0, fetched as the pinned release package for this platform and
# never committed. Nothing links it: src/denoise loads the library at run time
# through its C API, as volk loads Vulkan, so the plugin loads on a machine
# without it and says why the denoiser is unavailable rather than failing to
# load at all. At build time only the package's headers are used.
#
# Which library is loaded is decided at run time (docs/denoising.md 2):
#
#   - A standalone build copies the package's runtime beside the plugin, in
#     plugin/usd/hdClaude/oidn, and installs it there. That is what usdview
#     and the Linux rez package load.
#   - A Houdini build installs none. Houdini ships OIDN in $HFS/bin (2.3.3 in
#     22.0.469), and Karma and the compositor load it into the same process;
#     a second, different copy of a library of the same name in one process is
#     how a plugin crashes its host. So a Houdini build loads Houdini's.
#
# Failure to obtain the package downgrades the feature and does not fail the
# build: the denoiser setting then says "this build has no OIDN headers".
# ---------------------------------------------------------------------------

include_guard(GLOBAL)
include(FetchContent)

# A plain variable, not a cache entry, for the reason cmake/NvidiaDLSS.cmake
# gives: a cached pin is read once, into the first configure, and outlives the
# line that set it.
set(HDCLAUDE_OIDN_VERSION "2.5.1")

if(WIN32)
  set(_oidn_package "oidn-${HDCLAUDE_OIDN_VERSION}.x64.windows.zip")
  set(_oidn_sha256 "f11f91bc072a5e3a564515724cb72ab8fcfbc445c84a84c197ff9b16cc01396f")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(_oidn_package "oidn-${HDCLAUDE_OIDN_VERSION}.x86_64.linux.tar.gz")
  set(_oidn_sha256 "743c3e2aff8c220d5d70fe6cb970fb3d36f2702d2693c61d1d148e404cf37cd6")
else()
  set(_oidn_package "")
endif()

set(HDCLAUDE_OIDN_AVAILABLE FALSE CACHE INTERNAL "")
if(_oidn_package)
  FetchContent_Declare(oidn
      URL      "https://github.com/RenderKit/oidn/releases/download/v${HDCLAUDE_OIDN_VERSION}/${_oidn_package}"
      URL_HASH SHA256=${_oidn_sha256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # A binary package with no CMakeLists.txt of its own; see NvidiaDLSS.cmake
  # for why it is populated directly.
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_GetProperties(oidn)
  if(NOT oidn_POPULATED)
    FetchContent_Populate(oidn)
  endif()
  if(EXISTS "${oidn_SOURCE_DIR}/include/OpenImageDenoise/oidn.h")
    set(HDCLAUDE_OIDN_AVAILABLE TRUE CACHE INTERNAL "")
  endif()
endif()

if(HDCLAUDE_OIDN_AVAILABLE)
  add_library(hdClaudeOidnHeaders INTERFACE)
  target_include_directories(hdClaudeOidnHeaders SYSTEM INTERFACE
      "${oidn_SOURCE_DIR}/include")
  target_compile_definitions(hdClaudeOidnHeaders INTERFACE HDCLAUDE_HAS_OIDN=1)

  # The runtime a standalone build carries: the library, its core, every
  # device module the package has, and what they load -- TBB for the CPU
  # device, the SYCL and Level Zero runtime for Intel GPUs. The executables,
  # import libraries and CMake files are left behind.
  if(WIN32)
    file(GLOB HDCLAUDE_OIDN_RUNTIME "${oidn_SOURCE_DIR}/bin/*.dll")
  else()
    file(GLOB HDCLAUDE_OIDN_RUNTIME "${oidn_SOURCE_DIR}/lib/*.so*")
  endif()
  set(HDCLAUDE_OIDN_LICENCES
      "${oidn_SOURCE_DIR}/doc/LICENSE.txt"
      "${oidn_SOURCE_DIR}/doc/third-party-programs.txt"
      "${oidn_SOURCE_DIR}/doc/third-party-programs-oneTBB.txt"
      "${oidn_SOURCE_DIR}/doc/third-party-programs-DPCPP.txt")
  message(STATUS "hdClaude: Open Image Denoise ${HDCLAUDE_OIDN_VERSION} at ${oidn_SOURCE_DIR}")
else()
  set(HDCLAUDE_ENABLE_OIDN OFF CACHE BOOL "" FORCE)
  message(WARNING
      "hdClaude: Open Image Denoise ${HDCLAUDE_OIDN_VERSION} could not be "
      "obtained for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}. Building "
      "without the denoiser; the "
      "Denoiser setting will say so.")
endif()
