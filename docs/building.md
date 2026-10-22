# Building hdClaude

Last revised 2026-10-15. Platform of record: Windows 11, MSVC 2022, Ninja.
Linux (GCC or Clang, Ninja) is supported by the build scripts but **has never
been compiled** — see [Linux](#linux).

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
| **OpenUSD** | **external prebuilt install** | discovered by `setup_usd_env.bat` / `.sh` |
| MaterialX | the one OpenUSD was built against | `MATERIALX_ROOT` |
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

MaterialX and OpenSubdiv are never fetched. They are the ones OpenUSD was built
against, so Hydra and hdClaude agree on their ABI: Hydra hands us MaterialX
objects created by its own copy.

MaterialX can be laid out two ways, and both are found without configuration:

- **Bundled.** `build_usd.py` installs MaterialX inside the OpenUSD root, with
  `lib/cmake/MaterialX` and `libraries/` beside `pxrConfig.cmake`. This is the
  Windows workstation's layout.
- **Separate.** A package manager such as rez installs MaterialX as its own
  package. OpenUSD's `pxrConfig.cmake` records the `MaterialX_DIR` it was built
  against, and `cmake/OpenUSD.cmake` and `setup_usd_env.sh` read it from there.

To use a different MaterialX root, set `MATERIALX_ROOT` before running
`compile.py`, or pass `-DHDCLAUDE_MATERIALX_ROOT=<root>` to CMake. The standard
data libraries (`libraries/`) come from the same root and are compiled in as
the default search path.

Under rez, resolve an environment that contains the `usd` package and the
`materialx` package it requires. `setup_usd_env.sh` takes `USDROOT` from
`REZ_USD_ROOT` when `USDROOT` is not set.

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
   `compile.py` enters the developer environment only when the tools are not
   already on `PATH`. On Linux: GCC or Clang with C++20, CMake 3.25 or newer,
   Ninja, and the OpenGL development package (`libgl-dev` or equivalent) that
   OpenUSD's CMake config asks for.

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
compile.bat dev-dlss       :: with the optional NVIDIA DLSS backend (Windows only)
compile.bat clean          :: remove the build tree, then build "dev"
compile.bat houdini-22.0   :: against Houdini 22.0's USD, as a Houdini package
```

`compile.bat` forwards to `compile.py`, which is the build driver on every
platform and takes the same arguments (`python3 compile.py core-only` on Linux).
The build steps live there once. The environment does not: it comes from
`setup_usd_env.bat` on Windows and `setup_usd_env.sh` elsewhere, and those stay
shell scripts because they have to be sourced into an interactive shell for
usdview and usdrecord, which a Python process cannot do for its parent.

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

## Linux

Built and run on Ubuntu 22.04 with GCC 11.2, against OpenUSD 26.05 and a
separately packaged MaterialX 1.39.4 in a rez environment, through
`compile.py`. What the port consists of:

- **`setup_usd_env.sh`**, sourced (`. ./setup_usd_env.sh`), is the counterpart
  of `setup_usd_env.bat` and does the same things in the same order. It takes
  OpenUSD from `REZ_USD_ROOT` in a rez environment, else probes `~/dev/usd-*`
  and `/opt/usd-*`, defaults `USDEXTRA` to the
  repository's `install/`, and puts OpenUSD's libraries on `LD_LIBRARY_PATH` where
  Windows uses `PATH`. Keep the two in step.
- **`render_claude.sh`** passes `usdrecord` the same fixed flags as
  `render_claude.bat`; the reasons are written there once.
- **`plugInfo.json`** is generated from `resources/plugInfo.json.in`, so its
  `LibraryPath` names `hdClaude.so` on Linux and `hdClaude.dll` on Windows.
- **volk** is built position-independent, since it links into the shared
  module.
- **The validation layer** is found under `share/vulkan/explicit_layer.d`, in
  the source-built tree, `$VULKAN_SDK`, or `/usr`. Its manifest names a bare
  `libVkLayer_khronos_validation.so`, so the layer's `lib` directory goes on
  `LD_LIBRARY_PATH` as well as the manifest on `VK_LAYER_PATH`; the tests and
  `setup_usd_env.sh` both do this.
- **The USD tests** run through `tests/usd/run_in_usd_env.sh` instead of the
  `.bat` wrappers.
- **DLSS** is Windows-only: `dev-dlss` stops at configure on Linux, because
  `cmake/NvidiaDLSS.cmake` links only the SDK's Windows library.

GCC's standard library includes fewer headers transitively than MSVC's, so a
header a file uses but reaches only through another compiles on Windows and
not here; the first Linux build found one, `<cmath>` in `path_tracer.cpp`.
OpenUSD's own headers are noisy under `-Wpedantic`.

## Houdini

`compile.bat houdini-22.0` builds the same delegate against the USD inside a
Houdini installation, which is what Solaris and husk can load: Houdini's USD
has its own namespace (`pxrInternal_v0_26_5__pxrReserved__` in 22.0.469), TBB
and Python, so a plugin linked against a standalone OpenUSD of the same version
does not load there.

- **Finding Houdini.** `HDCLAUDE_HOUDINI_VERSION` (22.0 in the preset) selects
  `cmake/HoudiniUSD.cmake` in place of `cmake/OpenUSD.cmake`. The root is
  `HDCLAUDE_HOUDINI_ROOT` if set, else `$HFS` if it is that version, else the
  newest `C:\Program Files\Side Effects Software\Houdini 22.0.*`. The
  standalone environment (`setup_usd_env.bat`) is not used.
- **Linking.** Houdini installs no `pxrConfig.cmake`, and its
  `HoudiniConfig.cmake` describes the HDK. So `HoudiniUSD.cmake` defines,
  over Houdini's `libpxr_*` and `MaterialX*` import libraries, the target
  names the rest of the build links (`hd`, `usd`, `MaterialXCore`, ...).
  Python is linked by name, since Houdini's `pyconfig.h` does not ask for it,
  and OpenSubdiv is Houdini's static `libosdCPU_md`.
- **MaterialX.** Houdini 22.0 ships 1.39.5, where the hardware nodes moved to
  `MaterialXGenHw` and the stock GLSL library gained `AIRY_FRESNEL_ITERATIONS`
  and `mx_ggx_VNDF_reflection_PDF`. hdClaude builds against both 1.39.3 and
  1.39.5, and every material compiles. **It does not yet shade correctly
  against 1.39.5:** `standard_surface` and `open_pbr_surface` are built
  there from BSDF combinators whose rules hdClaude's overrides do not follow,
  and render with fireflies and too much light. displayColor and the fallback
  material render the same as the standalone build. See the 2026-10-22 note
  in `implementation-notes.md`; configure warns about it.
- **Install.** To `hdClaude` in Houdini's user preference directory: the
  plugin tree in `dso/usd_plugins`, `UsdRenderers.json` (the viewport menu
  entry and what the delegate renders) at its root, and a package,
  `packages\hdClaude.json`, that puts it on `HOUDINI_PATH`. Houdini adds every
  `HOUDINI_PATH` entry's `dso/usd_plugins` to the USD plugin path.
- **Which user directory.** The one Houdini uses: `HOUDINI_USER_PREF_DIR`
  if set, else `houdini22.0` in the Documents known folder. That is not
  necessarily `%USERPROFILE%\Documents`: with OneDrive it is
  `%USERPROFILE%\OneDrive\Documents`, and an install to the former is never
  found. `HDCLAUDE_HOUDINI_USER_DIR` overrides it. Configure prints the
  directory it chose. To check what Houdini loads, run `hconfig -ap` from a
  shell without `HOME` set: Houdini prefers `$HOME`, and Git Bash sets it.
- **Settings.** `soho/parameters/HdClaudeRendererPlugin_Viewport.ds` puts
  the delegate's settings in the viewport's Display Options, and
  `HdClaudeRendererPlugin_Global.ds` puts them in the Render Settings LOP,
  which authors them on the RenderSettings prim for husk. Houdini finds both
  by the plugin's name. They are generated from the delegate's own setting
  descriptors by `scripts/generate_houdini_parameters.py`, and the
  `usd_houdini_parameters` test fails when they are stale. Run the script
  after adding or changing a setting; it also refuses a setting it has not
  been told which folder to put in.
- **Depth.** `UsdRenderers.json` declares `depthstyle` `opengl`: depth in
  [0, 1] through the host's projection, which is what the `depth` AOV holds
  for every host. `needsdepth` is false: true would ask Houdini to
  rasterise depth itself instead of using hdClaude's. The `usd_depth` test
  decodes it as usdview does, for a perspective and an orthographic camera
  (Houdini's Top, Front and Side views are orthographic).
- **No tests and no tools.** The test and gallery scripts run against the
  standalone OpenUSD, so the preset turns `BUILD_TESTING` off and skips the
  image tools.

Another Houdini version is a copy of the preset with a different
`HDCLAUDE_HOUDINI_VERSION`.

## Install layout

The delegate installs to `USDEXTRA` (default
`%USERPROFILE%\Desktop\usd-26.03-extra` on Windows, the repository's
gitignored `install/` on Linux), placed outside the OpenUSD install, so that a USD upgrade does not delete the plugin and a
plugin rebuild cannot corrupt a USD distribution. `setup_usd_env.bat`/`.sh` puts both
trees on `PXR_PLUGINPATH_NAME`. To launch usdview against it on Linux:

```sh
. ./setup_usd_env.sh
usdview scene.usda
```

To install elsewhere, export `USDEXTRA` before sourcing. An assignment written
in front of `.` (`USDEXTRA=... . ./setup_usd_env.sh`) is undone by bash when
the script returns.

## Shader caches

hdClaude keeps two caches, both per user and per machine, and reads where they
live from the environment when it starts:

| Variable | What it holds | When unset |
|---|---|---|
| `HDCLAUDE_SHADER_CACHE` | compiled SPIR-V for the kernels and every material, one file per entry | `shader-cache/` under the directory the application was launched from |
| `HDCLAUDE_PIPELINE_CACHE` | the driver's compiled pipelines, one file | `pipeline.cache` in the SPIR-V cache directory |

The fallback is deliberately visible: a launch with nothing configured creates
`shader-cache/` where it started, which is how an unset variable gets noticed.
`setup_usd_env.bat`/`.sh` do not set either one.

Both are meant for local disk. SPIR-V entries are named by a hash of
everything that produces them and written to a per-process temporary that is
renamed into place, so concurrent sessions on one machine are safe. The
pipeline cache is one file that each session rewrites when it closes; the last
to close wins, and its contents are specific to the GPU and driver that wrote
them.

Within a session, a material whose network matches one already compiled --
the same nodes, connections and input values, wherever it sits in the stage --
is not compiled again, and the scene shades both with one program. Copies of
an asset that embed their material, and elements that share a look, cost one
compile per distinct look. `materialsCompiled` and `materialsReused` in the
stats report say how it went.

## Verifying

```bat
compile.bat core-only
ctest --test-dir build\core-only --output-on-failure
compile.bat
ctest --test-dir build\dev --output-on-failure
render_gallery.bat -Scene subdivision_features
```

The core-only preset needs neither a GPU nor OpenUSD, so it is what to run
first when something is wrong: if it fails, the problem is not the environment.
The full suite adds the GPU and MaterialX tests, which fail on any Vulkan
validation error — core or synchronisation — rather than printing one. The
gallery scene at the end is an end-to-end check through `usdrecord`, the
installed plugin, and the image gate.

There is no separate installation-validation script; the tests above are the
check. A plugin that builds and installs but is not discovered is a
`PXR_PLUGINPATH_NAME` problem — see Troubleshooting.

## Troubleshooting

**"OpenUSD was not found."** `setup_usd_env.bat`/`.sh` probed its known roots and
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
`PXR_PLUGINPATH_NAME` (`setup_usd_env.bat` does this), that both
`plugin\usd\plugInfo.json` and `plugin\usd\hdClaude\resources\plugInfo.json`
exist under it -- Plug reads only the first from a search path entry, and its
`Includes` leads to the second -- and that the
plugin's dependent DLLs resolve — a missing dependency makes a plugin silently
undiscoverable rather than producing an error.
