# ===========================================================================
# hdClaude - shared standalone OpenUSD runtime environment (Linux).
#
# The counterpart of setup_usd_env.bat, and kept in step with it: every
# USD-facing script calls one of the two, and they are the only places that
# know where OpenUSD, the Vulkan SDK, and optional SDKs live.
#
# Source it, do not run it -- the variables it sets must survive into the
# caller:
#
#   . ./setup_usd_env.sh
#
# Overrides, all optional, all honoured if already set:
#   USDROOT                 OpenUSD install root
#   MATERIALX_ROOT          MaterialX install root, when it is not inside OpenUSD
#   USDEXTRA                install prefix for hdClaude's own plugin
#   HDCLAUDE_VALIDATION_LAYER_DIR  directory holding VkLayer_khronos_validation.json
#   HDCLAUDE_SHADER_CACHE   SPIR-V cache directory; unset, the renderer uses
#                           shader-cache/ under the directory it was launched from
#   HDCLAUDE_PIPELINE_CACHE driver pipeline-cache file; unset, pipeline.cache in
#                           the SPIR-V cache directory
#   HDCLAUDE_SKIP_PYTHON    set to 1 to skip the Python conformance check
# ===========================================================================

_hdclaude_env() {
  local script="$1"
  HDCLAUDE_ROOT="$(cd "$(dirname "$script")" && pwd)"
  export HDCLAUDE_ROOT

  # -------------------------------------------------------------------------
  # 1. Locate OpenUSD.
  #     An explicit USDROOT always wins. Otherwise probe the usd package of a
  #     rez environment, then known-good roots, newest first, and validate by
  #     the presence of pxrConfig.cmake.
  # -------------------------------------------------------------------------
  if [ -z "${USDROOT:-}" ]; then
    local candidate
    for candidate in "${REZ_USD_ROOT:-}" \
        "$HOME/dev/usd-26.03" "$HOME/dev/usd-26.05" "$HOME/dev/usd-25.08" \
        "/opt/usd-26.03" "/opt/usd-26.05" "/opt/usd-25.08"; do
      if [ -f "$candidate/pxrConfig.cmake" ]; then
        USDROOT="$candidate"
        break
      fi
    done
  fi
  if [ -z "${USDROOT:-}" ]; then
    echo "[hdClaude] OpenUSD was not found."
    echo "           Set USDROOT to an OpenUSD install root containing pxrConfig.cmake,"
    echo "           or install one under ~/dev or /opt. See docs/building.md."
    return 1
  fi
  if [ ! -f "$USDROOT/pxrConfig.cmake" ]; then
    echo "[hdClaude] USDROOT does not look like an OpenUSD install: $USDROOT"
    echo "           Expected \"$USDROOT/pxrConfig.cmake\" to exist."
    return 1
  fi
  export USDROOT

  # Install prefix for the hdClaude plugin itself. Never inside the USD
  # install, so a USD upgrade does not delete the plugin and a plugin rebuild
  # cannot corrupt a USD distribution -- which off Windows is often a read-only
  # package. The repository's own gitignored install/ keeps the plugin beside
  # the source that built it.
  export USDEXTRA="${USDEXTRA:-$HDCLAUDE_ROOT/install}"

  # -------------------------------------------------------------------------
  # 2. Python conformance.
  #     OpenUSD's Python bindings are ABI-locked to the interpreter the
  #     distribution was built against. pxrConfig.cmake records that exact
  #     interpreter, so the requirement is derived rather than hard-coded.
  #
  #     The active environment's Python is used as-is. This script never
  #     selects, installs, or provisions an interpreter.
  # -------------------------------------------------------------------------
  if [ "${HDCLAUDE_SKIP_PYTHON:-}" != "1" ]; then
    local version='import sys;print("%d.%d"%sys.version_info[:2])'
    local recorded required active python
    recorded="$(sed -n 's/.*set(Python3_EXECUTABLE \[\[\([^]]*\)\]\].*/\1/p' \
                "$USDROOT/pxrConfig.cmake" | head -n 1)"
    if [ -n "$recorded" ] && [ -x "$recorded" ]; then
      required="$("$recorded" -c "$version" 2>/dev/null)"
    fi
    # Fall back to the version OpenUSD 26.03 ships against when the recorded
    # interpreter has been moved or removed.
    required="${required:-3.12}"

    python="$(command -v python3 || command -v python)"
    if [ -z "$python" ]; then
      echo "[hdClaude] Python was not found in the active environment."
      echo "           OpenUSD at $USDROOT requires Python $required."
      return 1
    fi
    active="$("$python" -c "$version" 2>/dev/null)"
    if [ "$active" != "$required" ]; then
      echo "[hdClaude] Python version mismatch."
      echo "           Active:   $active  ($python)"
      echo "           Required: $required  (the interpreter OpenUSD was built against)"
      [ -n "$recorded" ] && echo "           Recorded: $recorded"
      echo "           Activate a matching environment, or set HDCLAUDE_SKIP_PYTHON=1"
      echo "           for builds and renders that do not use the Python bindings."
      return 1
    fi
  fi

  # -------------------------------------------------------------------------
  # 3. OpenUSD runtime paths.
  #     Plugin discovery is a union, and both trees are required for
  #     usdview/usdrecord to find the delegate. Linux resolves shared
  #     libraries through LD_LIBRARY_PATH where Windows uses PATH.
  # -------------------------------------------------------------------------
  export PXR_PLUGINPATH_NAME="$USDROOT:$USDROOT/plugin/usd:$USDEXTRA/plugin/usd"
  export PYTHONPATH="$USDROOT/lib/python:$USDEXTRA/lib/python${PYTHONPATH:+:$PYTHONPATH}"
  export PATH="$USDROOT/bin:$USDEXTRA/bin:$PATH"
  export LD_LIBRARY_PATH="$USDROOT/lib:$USDEXTRA/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

  # -------------------------------------------------------------------------
  # 4. Vulkan validation layer.
  #     Validation is a gate, not a convenience: the GPU tests fail without it
  #     (docs/lessons-from-hdcodex.md R8). A layer installed by the system
  #     package manager is registered with the loader already; a source-built
  #     or SDK-tarball layer is not, so VK_LAYER_PATH is set explicitly. Its
  #     manifest names a bare libVkLayer_khronos_validation.so, so the layer's
  #     lib directory goes on LD_LIBRARY_PATH as well.
  #
  #     Search order: an explicit override, the source-built layer, the SDK.
  # -------------------------------------------------------------------------
  if [ -z "${HDCLAUDE_VALIDATION_LAYER_DIR:-}" ]; then
    local layers="$HDCLAUDE_ROOT/_deps/validation-layers/install/share/vulkan/explicit_layer.d"
    if [ -f "$layers/VkLayer_khronos_validation.json" ]; then
      HDCLAUDE_VALIDATION_LAYER_DIR="$layers"
    elif [ -n "${VULKAN_SDK:-}" ] &&
         [ -f "$VULKAN_SDK/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" ]; then
      export HDCLAUDE_VULKAN_SDK="$VULKAN_SDK"
      HDCLAUDE_VALIDATION_LAYER_DIR="$VULKAN_SDK/share/vulkan/explicit_layer.d"
    fi
  fi
  if [ -n "${HDCLAUDE_VALIDATION_LAYER_DIR:-}" ] &&
     [ -f "$HDCLAUDE_VALIDATION_LAYER_DIR/VkLayer_khronos_validation.json" ]; then
    export HDCLAUDE_VALIDATION_LAYER_DIR
    export VK_LAYER_PATH="$HDCLAUDE_VALIDATION_LAYER_DIR${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
    # <root>/share/vulkan/explicit_layer.d -> <root>/lib
    export LD_LIBRARY_PATH="$(cd "$HDCLAUDE_VALIDATION_LAYER_DIR/../../.." && pwd)/lib:$LD_LIBRARY_PATH"
  fi

  # -------------------------------------------------------------------------
  # 5. NVIDIA DLSS SDK - optional, proprietary, never committed. Nothing is
  #     set here, for the reason setup_usd_env.bat gives.
  # -------------------------------------------------------------------------

  # -------------------------------------------------------------------------
  # 6. RenderMan - optional. Make its runtime visible when RMANTREE names an
  #     install. Non-RenderMan setups are unaffected.
  # -------------------------------------------------------------------------
  if [ -n "${RMANTREE:-}" ] && [ -f "$RMANTREE/lib/libprman.so" ]; then
    export PATH="$RMANTREE/bin:$PATH"
    export LD_LIBRARY_PATH="$RMANTREE/lib:$LD_LIBRARY_PATH"
  fi

  # -------------------------------------------------------------------------
  # 7. hdClaude runtime defaults. Only set what is not already set, so a
  #     caller's explicit choice always wins. MaterialX separates search paths
  #     with ':' off Windows.
  #
  #     The MaterialX standard libraries are found the way cmake/OpenUSD.cmake
  #     finds MaterialX: MATERIALX_ROOT, else inside OpenUSD, else the
  #     MaterialX_DIR that OpenUSD recorded in pxrConfig.cmake.
  # -------------------------------------------------------------------------
  local materialx="${MATERIALX_ROOT:-}"
  if [ -z "$materialx" ] && [ -d "$USDROOT/libraries" ]; then
    materialx="$USDROOT"
  elif [ -z "$materialx" ]; then
    # <root>/lib/cmake/MaterialX -> <root>
    materialx="$(sed -n 's/.*set(MaterialX_DIR \[\[\([^]]*\)\]\].*/\1/p' \
                 "$USDROOT/pxrConfig.cmake" | head -n 1)"
    materialx="${materialx%/lib/cmake/MaterialX}"
  fi
  if [ ! -d "$materialx/libraries" ]; then
    echo "[hdClaude] The MaterialX standard libraries were not found."
    echo "           Looked in \"$materialx/libraries\"."
    echo "           Set MATERIALX_ROOT to the MaterialX this OpenUSD was built against."
    return 1
  fi
  export MATERIALX_SEARCH_PATH="${MATERIALX_SEARCH_PATH:-$HDCLAUDE_ROOT/mtlx:$materialx/libraries}"
  return 0
}

# bash names a sourced file in BASH_SOURCE; zsh in $0.
_hdclaude_env "${BASH_SOURCE[0]:-$0}"
_hdclaude_status=$?
unset -f _hdclaude_env
return $_hdclaude_status 2>/dev/null || exit $_hdclaude_status
