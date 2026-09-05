# ---------------------------------------------------------------------------
# Khronos Vulkan validation layers, built from source.
#
# Validation is not a debugging convenience in this project; it is a gate. The
# GPU tests fail if the layer is unavailable, because a validation gate whose
# layer is absent passes against a counter nothing can increment -- lesson R8
# reproduced inside the fix for R8 (docs/implementation-notes.md).
#
# That makes the layer a real dependency, so it is treated like every other
# one: referenced by pinned tag, built from source into the gitignored
# dependency tree, never committed. It is pinned to the same Vulkan SDK release
# as Vulkan-Headers, volk, and glslang, so the layer cannot disagree with the
# headers about what it is validating.
#
# ExternalProject rather than FetchContent: the validation layers configure
# their own dependency set (SPIRV-Headers, SPIRV-Tools) through UPDATE_DEPS,
# and they set enough CMake globals that pulling them into this project's scope
# with add_subdirectory would leak configuration into hdClaude's own targets.
# ExternalProject gives them a separate configure, a separate build, and an
# install prefix we can point VK_LAYER_PATH at.
#
# The build is long -- SPIRV-Tools dominates -- which is why it is opt-in.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)
include(ExternalProject)

set(HDCLAUDE_VALIDATION_LAYERS_PREFIX
    "${HDCLAUDE_DEPS_DIR}/validation-layers" CACHE INTERNAL "")
set(HDCLAUDE_VALIDATION_LAYERS_INSTALL
    "${HDCLAUDE_VALIDATION_LAYERS_PREFIX}/install" CACHE INTERNAL "")

# Where the built layer manifest lands. The Vulkan loader reads VK_LAYER_PATH
# and looks for a .json manifest naming the binary beside it.
if(WIN32)
  set(HDCLAUDE_VALIDATION_LAYER_DIR "${HDCLAUDE_VALIDATION_LAYERS_INSTALL}/bin"
      CACHE INTERNAL "")
else()
  set(HDCLAUDE_VALIDATION_LAYER_DIR
      "${HDCLAUDE_VALIDATION_LAYERS_INSTALL}/share/vulkan/explicit_layer.d"
      CACHE INTERNAL "")
endif()

# Python is required by the validation layers' own dependency bootstrap
# (scripts/update_deps.py). setup_usd_env.bat already guarantees an interpreter
# matching the OpenUSD build, so this reuses it rather than provisioning one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
  message(FATAL_ERROR
      "HDCLAUDE_BUILD_VALIDATION_LAYERS requires a Python interpreter for the "
      "validation layers' dependency bootstrap, and none was found. Run "
      "setup_usd_env.bat first, or turn the option off and install the Vulkan "
      "SDK instead. See docs/building.md.")
endif()

ExternalProject_Add(hdClaudeValidationLayers
    GIT_REPOSITORY  https://github.com/KhronosGroup/Vulkan-ValidationLayers.git
    GIT_TAG         ${HDCLAUDE_VULKAN_SDK_TAG}
    GIT_SHALLOW     TRUE
    GIT_PROGRESS    TRUE
    PREFIX          "${HDCLAUDE_VALIDATION_LAYERS_PREFIX}"
    INSTALL_DIR     "${HDCLAUDE_VALIDATION_LAYERS_INSTALL}"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
        # Fetches and builds the known-good SPIRV-Headers/SPIRV-Tools revisions
        # the layers were tested against, rather than making us pin a second
        # set of versions that could disagree with theirs.
        -DUPDATE_DEPS=ON
        -DBUILD_TESTS=OFF
        -DBUILD_WERROR=OFF
    # Release-only: the layer is a tool, and a debug build of SPIRV-Tools
    # roughly triples an already long build for no benefit to us.
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
    BUILD_BYPRODUCTS
        "${HDCLAUDE_VALIDATION_LAYER_DIR}/VkLayer_khronos_validation.json"
    USES_TERMINAL_DOWNLOAD TRUE
    USES_TERMINAL_BUILD    TRUE)

message(STATUS "hdClaude: validation layers will be built from source "
               "(${HDCLAUDE_VULKAN_SDK_TAG}) into ${HDCLAUDE_VALIDATION_LAYER_DIR}")
