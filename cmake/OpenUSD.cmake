# ---------------------------------------------------------------------------
# OpenUSD and MaterialX.
#
# These are *external* dependencies: hdClaude builds against a standalone
# OpenUSD distribution and never vendors, fetches, or patches USD source. That
# constraint is what makes an out-of-tree delegate possible, and it is why only
# public OpenUSD headers may be included (docs/architecture.md 1.3).
#
# MaterialX comes from inside the OpenUSD distribution rather than being
# fetched separately. Two MaterialX copies in one process is an ODR violation
# waiting to happen, and Hydra hands us MaterialX objects created by its copy.
#
# Do not add Houdini search paths here. A Houdini-ABI build needs a separate
# preset and dependency graph, not a conditional in this file.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)

if(NOT EXISTS "${HDCLAUDE_OPENUSD_ROOT}/pxrConfig.cmake")
  message(FATAL_ERROR
      "OpenUSD was not found at '${HDCLAUDE_OPENUSD_ROOT}'.\n"
      "Expected '${HDCLAUDE_OPENUSD_ROOT}/pxrConfig.cmake'.\n"
      "Set -DHDCLAUDE_OPENUSD_ROOT=<install root>, or run setup_usd_env.bat "
      "which discovers one and exports USDROOT. See docs/building.md.")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${HDCLAUDE_OPENUSD_ROOT}")

# Point USD's transitive dependencies at the same distribution, so a system
# installation of any of them cannot be picked up and produce an ABI mismatch
# that only shows up as a crash at plugin load.
set(OpenSubdiv_DIR "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/OpenSubdiv" CACHE PATH "" FORCE)
set(MaterialX_DIR  "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/MaterialX"  CACHE PATH "" FORCE)
set(Imath_DIR      "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/Imath"      CACHE PATH "" FORCE)

find_package(OpenGL REQUIRED)

find_package(pxr CONFIG REQUIRED
    PATHS "${HDCLAUDE_OPENUSD_ROOT}"
    NO_DEFAULT_PATH)

if(HDCLAUDE_ENABLE_MATERIALX)
  find_package(MaterialX CONFIG REQUIRED
      PATHS "${HDCLAUDE_OPENUSD_ROOT}/lib/cmake/MaterialX"
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
  set(HDCLAUDE_MATERIALX_LIBRARIES "${HDCLAUDE_OPENUSD_ROOT}/libraries"
      CACHE PATH "MaterialX standard data libraries")
  if(NOT EXISTS "${HDCLAUDE_MATERIALX_LIBRARIES}/pbrlib/genglsl")
    message(FATAL_ERROR
        "MaterialX standard libraries not found at "
        "'${HDCLAUDE_MATERIALX_LIBRARIES}'. genglsl_pt inherits from genglsl "
        "and cannot generate without them.")
  endif()
endif()

message(STATUS "hdClaude: OpenUSD ${PXR_VERSION} at ${HDCLAUDE_OPENUSD_ROOT}")
if(HDCLAUDE_ENABLE_MATERIALX)
  message(STATUS "hdClaude: MaterialX ${MaterialX_VERSION}")
endif()
