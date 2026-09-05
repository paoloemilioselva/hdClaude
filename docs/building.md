# Building hdClaude

Last revised 2026-09-05. Platform of record: Windows 11, MSVC 2022, Ninja.

## Dependency policy

**Nothing third-party is committed to this repository.** Every dependency is
referenced by an exact pinned tag in
[cmake/Dependencies.cmake](../cmake/Dependencies.cmake) and fetched by CMake
into the gitignored `_deps/` tree. A fresh clone therefore reproduces the same
dependency set with a single configure, and the repository stays small and free
of third-party or proprietary code.

| Dependency | How it is obtained | Pinned by |
|---|---|---|
| Vulkan-Headers | CMake `FetchContent` | `HDCLAUDE_VULKAN_SDK_TAG` |
| volk | CMake `FetchContent` | `HDCLAUDE_VULKAN_SDK_TAG` |
| VulkanMemoryAllocator | CMake `FetchContent` | `HDCLAUDE_VMA_TAG` |
| glslang | CMake `FetchContent` | `HDCLAUDE_VULKAN_SDK_TAG` |
| Vulkan-ValidationLayers | CMake `ExternalProject`, opt-in | `HDCLAUDE_VULKAN_SDK_TAG` |
| NVIDIA DLSS SDK | CMake `FetchContent`, optional | `HDCLAUDE_DLSS_TAG` |
| **OpenUSD** | **external prebuilt install** | discovered by `setup_usd_env.bat` |
| MaterialX | from inside the OpenUSD distribution | — |
| OpenSubdiv | from inside the OpenUSD distribution | — |

Vulkan-Headers, volk, glslang, and the validation layers are held at the same
Vulkan SDK release tag, so the headers, the loader metadata, the SPIR-V
compiler, and the layer that validates our use of all three cannot disagree
about what Vulkan 1.3 and SPIR-V 1.6 mean.

The validation layers use `ExternalProject` rather than `FetchContent` because
they configure their own dependency set (SPIRV-Headers, SPIRV-Tools) through
`UPDATE_DEPS`, and they set enough CMake globals that `add_subdirectory` would
leak configuration into hdClaude's own targets. `ExternalProject` gives them a
separate configure, a separate build, and an install prefix that
`VK_LAYER_PATH` can point at. Their known-good SPIRV-Tools revision is used
rather than a second set of pins that could disagree with theirs.

MaterialX and OpenSubdiv come from **inside** the OpenUSD distribution rather
than being fetched separately. Two MaterialX copies in one process is an ODR
violation waiting to happen, and Hydra hands us MaterialX objects created by its
own copy.

### Reusing an existing download

Both are standard CMake and neither requires editing a file:

```bat
:: point the whole dependency tree at an existing populated directory
cmake --preset dev -DHDCLAUDE_DEPS_DIR=C:\path\to\_deps

:: point one dependency at a local checkout
cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_GLSLANG=C:\src\glslang
```

### Building offline

Once `_deps/` is populated:

```bat
cmake --preset dev -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```

## Prerequisites

1. **OpenUSD**, built with MaterialX and OpenSubdiv. The development
   workstation uses a standalone build at `C:\dev\usd-26.03`.
   `setup_usd_env.bat` probes known roots and validates each by the presence of
   `pxrConfig.cmake`; set `USDROOT` to override.

2. **Python**, matching the interpreter OpenUSD was built against.
   `setup_usd_env.bat` reads that interpreter's path out of the distribution's
   own `pxrConfig.cmake` and derives the required version from it, rather than
   hard-coding one — so the check stays correct when `USDROOT` changes. The
   active environment's Python is used as-is; the script never selects,
   installs, or provisions an interpreter. Set `HDCLAUDE_SKIP_PYTHON=1` for
   builds and renders that do not use the Python bindings.

3. **Visual Studio 2022** with the C++ workload, plus CMake and Ninja.
   `compile.bat` enters the developer environment only when the tools are not
   already on `PATH`.

4. **A Vulkan 1.3 GPU** exposing `VK_KHR_ray_query`,
   `VK_KHR_acceleration_structure`, `VK_KHR_buffer_device_address`,
   `VK_KHR_deferred_host_operations`, and descriptor indexing.

5. **The Vulkan validation layer.** Optional to build and render; **required
   to run the GPU tests**, which fail rather than warn when it is absent. A
   validation gate whose layer is missing passes against a counter nothing can
   increment — lesson R8 reproduced inside the fix for R8, see
   [implementation-notes.md](implementation-notes.md).

   Nothing needs to be installed by hand. Build it from source like every other
   dependency:

   ```bat
   compile.bat dev-validation
   ```

   or configure any preset with `-DHDCLAUDE_BUILD_VALIDATION_LAYERS=ON`. It is
   opt-in because it is a long first build — SPIRV-Tools dominates — and
   incremental afterwards.

   The configure locates a layer in this order: an explicit
   `HDCLAUDE_VALIDATION_LAYER_DIR`, an already-built `_deps` tree, then a
   system `VULKAN_SDK`. Whatever it finds is passed to the GPU test as
   `VK_LAYER_PATH`, so an installed SDK works without the source build. If it
   finds nothing, the configure says so and says how to fix it, rather than
   letting the failure surface later as a puzzling test result.

## Building

```bat
compile.bat core-only      :: dependency-free core and its tests; no GPU, no USD
compile.bat                :: the full delegate ("dev" preset)
compile.bat dev-validation :: the full delegate, building the validation layer
compile.bat dev-dlss       :: with the optional NVIDIA DLSS backend
compile.bat clean          :: remove the build tree, then build "dev"
```

Or with CMake presets directly:

```bat
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The **core-only** preset builds `hdClaudeCore` and its tests with no Vulkan and
no OpenUSD dependency at all. It is the fastest way to check that a change to
hashing, the shader cache, or the spectral pipeline is sound, and it runs on any
machine with a compiler.

## Install layout

The delegate installs to `USDEXTRA` (default
`%USERPROFILE%\Desktop\usd-26.03-extra`), placed beside the OpenUSD install
rather than inside it, so that a USD upgrade does not delete the plugin and a
plugin rebuild cannot corrupt a USD distribution. `setup_usd_env.bat` puts both
trees on `PXR_PLUGINPATH_NAME`.

## Verifying

```bat
compile.bat core-only
build\core-only\tests\hdClaudeCoreTests.exe
validate_usd.bat
```

## Troubleshooting

**"OpenUSD was not found."** `setup_usd_env.bat` probed its known roots and
none contained `pxrConfig.cmake`. Set `USDROOT` explicitly.

**"Python version mismatch."** The active interpreter's version differs from the
one recorded in `pxrConfig.cmake`. The message prints both, plus the recorded
interpreter path. Activate a matching environment, or set
`HDCLAUDE_SKIP_PYTHON=1` if the task does not use the Python bindings.

**"Validation layer ... NOT FOUND" in the configure summary.** The GPU tests
will fail, by design. Either build the layer from source
(`-DHDCLAUDE_BUILD_VALIDATION_LAYERS=ON`) or point
`HDCLAUDE_VALIDATION_LAYER_DIR` at a directory containing
`VkLayer_khronos_validation.json`.

**A dependency fetch fails. Check network access, or populate `_deps/` from
another machine and configure with `FETCHCONTENT_FULLY_DISCONNECTED=ON`. A DLSS
fetch failure is not an error: the feature turns itself off and the build
continues, because DLSS is never a requirement of the default build.

**The delegate does not appear in usdview.** Confirm `USDEXTRA` is on
`PXR_PLUGINPATH_NAME` (`setup_usd_env.bat` does this), that
`plugin\usd\hdClaude\resources\plugInfo.json` exists under it, and that the
plugin's dependent DLLs resolve — a missing dependency makes a plugin silently
undiscoverable rather than producing an error.
