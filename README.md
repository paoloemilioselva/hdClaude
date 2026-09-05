# hdClaude

An out-of-tree OpenUSD Hydra render delegate: a **spectral, wavefront GPU path
tracer** whose shading is **MaterialX-generated code, compiled and executed**.

> **Status: under construction.** The design is settled and recorded; the
> renderer is being built phase by phase. See
> [docs/roadmap.md](docs/roadmap.md) for what works today. Nothing in this
> README describes behaviour that is not yet built without saying so.

## What makes it different

**MaterialX shaders are executed, not reinterpreted.** There is no OpenPBR
parameter extractor, no Standard Surface special case, and no hand-written
"closure ABI" that a material graph is lowered into. A bound material is
compiled by MaterialX's own shader generator into GLSL, compiled to SPIR-V, and
run. Arbitrary pattern graphs, procedural nodes, and user nodegraphs work
because MaterialX generates them.

The one thing stock MaterialX GLSL generation does not emit is BSDF importance
sampling, so hdClaude adds it as a derived MaterialX target — `genglsl_pt`,
declared with `<targetdef name="genglsl_pt" inherit="genglsl"/>` — overriding
only the `pbrlib` closure nodes and inheriting everything else unchanged. That
is the extension mechanism the MaterialX documentation prescribes, and it is
why there is no third "approximated" material state: a material either compiles
and renders with full fidelity, or it is reported as an error.

**Light is transported spectrally.** Four correlated hero wavelengths per path.
Authored RGB reflectances are upsampled to bounded, energy-conserving spectra;
colour temperature resolves to an actual Planckian blackbody; dielectric IOR
varies by wavelength where the material says so. The sensor integrates CIE XYZ
once, at the film. RGB is an asset-input and display-output format, never the
transport representation — and there is no RGB fast path to be tempted by.

**The integrator is wavefront, not a megakernel.** Path state lives in device
memory; each bounce traverses, sorts hits by material, and dispatches **once
per material** so that each dispatch contains only that material's compiled
program. This is what makes real MaterialX programs affordable: a 400-node
nodegraph on one object cannot cost occupancy on every pixel in the frame.

**Real-time is reached by reconstruction, correctly.** A renderer-native
spatiotemporal filter works on any Vulkan 1.3 ray-query device; NVIDIA DLSS
Super Resolution, DLAA, and Ray Reconstruction are optional and discovered at
runtime. Reconstruction guides come from what the MaterialX closures report
about themselves, never from a surface-model name. Reference renders never pass
through a reconstruction backend at all.

## Design documents

The architecture is written down before it is built, and updated as it is.

| Document | Contents |
|---|---|
| [Architecture](docs/architecture.md) | commitments, layering, frame lifecycle, resource rules |
| [Roadmap](docs/roadmap.md) | phase tracker, exit gates, decision log, open questions |
| [MaterialX code generation](docs/materialx-codegen.md) | the `genglsl_pt` target, closures, combinators |
| [Spectral rendering](docs/spectral-rendering.md) | wavelengths, upsampling, illuminants, the sensor |
| [Wavefront integrator](docs/wavefront-integrator.md) | path state, queues, kernels, scheduling |
| [DLSS integration](docs/dlss-integration.md) | backends, guides, frame metadata, boundaries |
| [Lessons from hdCodex](docs/lessons-from-hdcodex.md) | defects inherited from the predecessor as rules |
| [Implementation notes](docs/implementation-notes.md) | running log of findings that corrected the design |
| [Building](docs/building.md) | dependencies, environment, toolchain |
| [Gallery](gallery.md) | versioned baselines, settings, and the machine record |

hdClaude is a second implementation. Its predecessor,
[`hdCodex`](../hdCodex), is a working renderer whose Phase 2 was reopened after
three reviews found thirteen defects, most traceable to four structural
decisions. Those four are adopted here as rules from day one rather than
rediscovered — that is what
[docs/lessons-from-hdcodex.md](docs/lessons-from-hdcodex.md) is for.

## Building

```bat
compile.bat core-only      :: dependency-free core and its tests
compile.bat                :: the full delegate
compile.bat dev-dlss       :: with the optional NVIDIA DLSS backend
```

Third-party dependencies are **not committed**. They are referenced by pinned
tag in [cmake/Dependencies.cmake](cmake/Dependencies.cmake) and fetched by CMake
into the gitignored `_deps/` tree on the first configure, so a fresh clone
reproduces the same tree with one command and no manual setup. OpenUSD is the
one exception: it is an external prebuilt distribution, discovered by
`setup_usd_env.bat`, never vendored.

Every USD-facing script calls `setup_usd_env.bat`, which locates OpenUSD,
derives the required Python version from the distribution's own
`pxrConfig.cmake` rather than hard-coding it, and discovers the optional Vulkan
and DLSS SDKs. See [docs/building.md](docs/building.md).

## Rendering

```bat
render_claude.bat --imageWidth 1024 --camera renderCam gallery\chess_board.usda out.exr
launch_claude.bat gallery\shader_ball_gold.usda
render_gallery.bat
```

`render_claude.bat` accepts normal `usdrecord` arguments and always disables
camera lighting, so authored or fallback lighting is what gets tested. For tools
that do not expose Hydra settings, set them in the environment:

```bat
set HDCLAUDE_SAMPLES_PER_PIXEL=1024
set HDCLAUDE_SAMPLES_PER_UPDATE=32
set HDCLAUDE_MAX_BOUNCES=8
set HDCLAUDE_ENABLE_SUBDIVISION=1
set HDCLAUDE_SUBDIVISION_LEVEL=2
set HDCLAUDE_ENABLE_DISPLACEMENT=1
```

## Licence

hdClaude is available under the [MIT Licence](LICENSE). It vendors no
third-party code. The optional NVIDIA DLSS SDK is proprietary, is never
committed here, and is obtained through the same pinned-fetch mechanism as
every other dependency.
