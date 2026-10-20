# ---------------------------------------------------------------------------
# OpenUSD and MaterialX.
#
# These are *external* dependencies: hdClaude builds against a standalone
# OpenUSD distribution and never vendors, fetches, or patches USD source. That
# constraint is what makes an out-of-tree delegate possible, and it is why only
# public OpenUSD headers may be included (docs/architecture.md 1.3).
#
# MaterialX is never fetched: it is the MaterialX this OpenUSD was built
# against, so Hydra and hdClaude agree on its ABI. Two layouts are supported:
#
#   bundled    build_usd.py installs MaterialX inside the OpenUSD root, with
#              lib/cmake/MaterialX and libraries/ beside pxrConfig.cmake.
#   separate   a package manager (rez, spack, ...) installs it on its own.
#              pxrConfig.cmake records the MaterialX_DIR OpenUSD was built
#              against, and that is used.
#
# HDCLAUDE_MATERIALX_ROOT, defaulting to $MATERIALX_ROOT, overrides both.
#
# Do not add Houdini search paths here. A Houdini-ABI build needs a separate
# preset and dependency graph, not a conditional in this file.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)

if(NOT EXISTS "${HDCLAUDE_OPENUSD_ROOT}/pxrConfig.cmake")
  message(FATAL_ERROR
      "OpenUSD was not found at '${HDCLAUDE_OPENUSD_ROOT}'.\n"
      "Expected '${HDCLAUDE_OPENUSD_ROOT}/pxrConfig.cmake'.\n"
      "Set -DHDCLAUDE_OPENUSD_ROOT=<install root>, or build with compile.py, "
      "whose setup_usd_env.bat/.sh discovers one and exports USDROOT. See "
      "docs/building.md.")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${HDCLAUDE_OPENUSD_ROOT}")

# Point USD's transitive dependencies at the same distribution, so a system
# installation of any of them cannot be picked up and produce an ABI mismatch
# that only shows up as a crash at plugin load.
set(OpenSubdiv_DIR "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/OpenSubdiv" CACHE PATH "" FORCE)
set(Imath_DIR      "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/Imath"      CACHE PATH "" FORCE)

# MaterialX_DIR is resolved here, before find_package(pxr), because
# pxrConfig.cmake finds MaterialX itself and honours a MaterialX_DIR already
# set. It is forced on every configure so a changed OpenUSD root or override
# cannot leave a stale one in the cache.
set(HDCLAUDE_MATERIALX_ROOT "$ENV{MATERIALX_ROOT}" CACHE PATH
    "MaterialX install root. Empty: the one inside, or recorded by, OpenUSD.")
if(HDCLAUDE_MATERIALX_ROOT)
  set(_materialx_dir "${HDCLAUDE_MATERIALX_ROOT}/lib/cmake/MaterialX")
elseif(EXISTS "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/MaterialX/MaterialXConfig.cmake")
  set(_materialx_dir "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/MaterialX")
else()
  file(STRINGS "${HDCLAUDE_OPENUSD_ROOT}/pxrConfig.cmake" _materialx_dir
       REGEX "set\\(MaterialX_DIR \\[\\[[^]]*\\]\\]\\)" LIMIT_COUNT 1)
  string(REGEX REPLACE ".*\\[\\[([^]]*)\\]\\].*" "\\1" _materialx_dir "${_materialx_dir}")
endif()
if(HDCLAUDE_ENABLE_MATERIALX AND
   NOT EXISTS "${_materialx_dir}/MaterialXConfig.cmake")
  message(FATAL_ERROR
      "MaterialX was not found. Expected "
      "'${_materialx_dir}/MaterialXConfig.cmake'.\n"
      "Set -DHDCLAUDE_MATERIALX_ROOT=<install root>, or MATERIALX_ROOT, to the "
      "MaterialX this OpenUSD was built against. See docs/building.md.")
endif()
if(_materialx_dir)
  set(MaterialX_DIR "${_materialx_dir}" CACHE PATH "" FORCE)
endif()

find_package(OpenGL REQUIRED)

find_package(pxr CONFIG REQUIRED
    PATHS "${HDCLAUDE_OPENUSD_ROOT}"
    NO_DEFAULT_PATH)

if(HDCLAUDE_ENABLE_MATERIALX)
  find_package(MaterialX CONFIG REQUIRED
      PATHS "${MaterialX_DIR}"
      NO_DEFAULT_PATH)

  # The generated-code contract in docs/materialx-codegen.md is written against
  # MaterialX 1.39's ClosureData shape. A different minor version may have
  # changed it, and a silent mismatch would miscompile shading.
  if(MaterialX_VERSION AND NOT MaterialX_VERSION MATCHES "^1\\.39")
    message(WARNING
        "hdClaude's genglsl_pt target is written against MaterialX 1.39; "
        "found ${MaterialX_VERSION}. Review docs/materialx-codegen.md 7 "
        "before trusting generated shading.")
  endif()

  # The stock MaterialX data libraries. genglsl_pt inherits from genglsl, so
  # both this tree and hdClaude's own mtlx/ tree must be on the search path.
  # They are the ones of the MaterialX found above, which its config names.
  # Not a cache variable: it follows MaterialX_DIR, and an older cache held
  # one that assumed the bundled layout.
  unset(HDCLAUDE_MATERIALX_LIBRARIES CACHE)
  set(HDCLAUDE_MATERIALX_LIBRARIES "${MATERIALX_STDLIB_DIR}")
  if(NOT HDCLAUDE_MATERIALX_LIBRARIES)
    cmake_path(GET MaterialX_DIR PARENT_PATH _materialx_root)
    cmake_path(GET _materialx_root PARENT_PATH _materialx_root)
    cmake_path(GET _materialx_root PARENT_PATH _materialx_root)
    set(HDCLAUDE_MATERIALX_LIBRARIES "${_materialx_root}/libraries")
  endif()
  if(NOT EXISTS "${HDCLAUDE_MATERIALX_LIBRARIES}/pbrlib/genglsl")
    message(FATAL_ERROR
        "MaterialX standard libraries not found at "
        "'${HDCLAUDE_MATERIALX_LIBRARIES}'. genglsl_pt inherits from genglsl "
        "and cannot generate without them.")
  endif()
endif()

message(STATUS "hdClaude: OpenUSD ${PXR_VERSION} at ${HDCLAUDE_OPENUSD_ROOT}")
if(HDCLAUDE_ENABLE_MATERIALX)
  message(STATUS "hdClaude: MaterialX ${MaterialX_VERSION} at ${MaterialX_DIR}")
endif()
