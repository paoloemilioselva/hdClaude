# Gallery

These images are versioned visual baselines, not golden-reference renders. They
exist so that an intentional improvement and an unintentional regression are
both visible in a diff.

**Status: eight of ten scenes.** The renderer is under construction; see
[docs/roadmap.md](docs/roadmap.md). These images are what hdClaude produces
today, and every scene below says what its baseline still gets wrong. Two
scenes do not render at all and are recorded as failures rather than omitted.
Parity with the hdCodex baselines is phase 8 and has not been reached.

The first pass through this gallery found seven defects that no test had:
textures uploaded upside down, texture coordinates dropped by refinement and by
the `geompropvalue` path, an image node with no file rendered as a failure, a
prototype reusing another's texture coordinates, an acceleration-structure
scratch address that lost the device, and every refraction evaluated by the
reflection closure. That is what the gallery is for, and it is why the images
are committed before they are right.

## Contract

Baselines are 1024 pixels wide at 1024 samples per pixel, in 32-sample
progressive updates, at the delegate's default path length of eight bounces,
with subdivision at the deterministic level-2 gallery setting. The New Zealand
height map is the deliberate exception: its single authored quad is uniformly
refined at level 6.

Two things the settings deliberately do not claim yet. Transport is RGB —
hero-wavelength transport is phase 6, and the table will say so when it lands.
There is no displacement — that is phase 16, so the height map's quad is refined
and flat.

The renderer's output is always scene-linear. `render_gallery.bat` records
temporary EXRs under `build/gallery-linear`, then writes the display JPEGs
through a neutral HDR highlight compressor and the standard sRGB transfer
function. `HDCLAUDE_GALLERY_EXPOSURE` sets display exposure in stops; the
versioned baselines use the default of zero.

**Every render is compared against its committed baseline before that baseline
is replaced.** RMS, per-pixel maximum, and failed-pixel count all have
thresholds, and a render that fails any of them stops the script. Exit status
alone is not a gate: hdCodex gated on exit status, and a run that lost the
Vulkan device wrote a black image and exited 0, which was accepted. See
[docs/lessons-from-hdcodex.md](docs/lessons-from-hdcodex.md) R9. Use
`render_gallery.bat -Accept` to deliberately adopt a changed image.

Whenever a baseline is regenerated, refresh its wall time, SHA-256, and machine
record in this file using the exact command shown for that scene. The hashes
identify the committed display JPEGs. Do not infer timings from file timestamps
or from partial diagnostic renders. Timings include renderer startup, scene
loading, MaterialX generation and compilation, geometry processing, and
sampling; display-JPEG conversion is excluded.

`render_gallery.bat` does all of that: it renders each scene, times the render
alone, converts the EXR through `hdClaudeDisplayTransform`, runs
`hdClaudeImageDiff` against the committed baseline, and rewrites the table
below. `-Scene <keys>` renders a subset, `-Accept` adopts a deliberate change,
`-UpdateOnly` rewrites the table without rendering. It stops at the first scene
that fails, so a scene listed as not measured has to be run explicitly once the
scenes before it are fixed.

### Machine record

Recorded with every measurement, because a timing without it cannot be compared
and a device-loss investigation cannot start without it. hdCodex spent days on a
"TDR timeout" hypothesis that this one row falsifies
([lessons](docs/lessons-from-hdcodex.md) R11).

| Field | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5060 Ti |
| Driver | 32.0.16.1664 |
| `TdrDelay` | 60 |
| `TdrLevel` | unset (default) |
| `TdrDdiDelay` | unset (default) |
| OpenUSD | 26.03 |
| MaterialX | 1.39.3 (from the OpenUSD distribution) |

<!-- gallery-timings:start -->
| Scene | Measured | Wall time | SHA-256 | Device | Settings |
|---|---:|---:|---|---|---|
| Intel Sponza | 2026-09-06 | 26.751 s (0m 26.751s) | `5ea5b3a24052eec6f1184d813db49960e6ad057cd587cb3aa35eaf66bb2473ac` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| OpenChessSet | 2026-09-06 | 25.841 s (0m 25.841s) | `717de134df8821b66a4e26f548a45a03d44508f8566961abf53d8bfd2c900401` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Gold | 2026-09-06 | 24.057 s (0m 24.057s) | `4f022ec9935d25b028e6b67de4cce4622a6780f8f1ca240a3ce2a0af5743e2f7` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Glass | 2026-09-06 | 26.582 s (0m 26.582s) | `363f2ff6b98dd1855dcf1416f3ad8fdad1226210da013ee04c846f327c25f859` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall BubbleGum | 2026-09-06 | 25.437 s (0m 25.437s) | `e6f802d1de6a1bddf3b5b1ee7b4de5b0c19a7763f28a6e747b927abebba274a4` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Pixar's KitchenSet | 2026-09-06 | 123.157 s (2m 3.157s) | `3b2ef0aa2372fc48394296cead98e296df2245af563b8003c66b39329bb7cc5d` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Collective Project 001 | - | Not measured | `-` | - | - |
| OpenPBR Playground | - | Not measured | `-` | - | - |
| Subdivision Feature Matrix | 2026-09-06 | 12.838 s (0m 12.838s) | `0090f710d67904acb88db93679fc3dc9d92a61873a538e7358034301a9dec058` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| New Zealand Height Map | 2026-09-06 | 7.858 s (0m 7.858s) | `6b413ce63241260aa2b38f6581a098f8d82f9b20b5fb287720127753a169aa31` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 6 |
<!-- gallery-timings:end -->

## External assets

The gallery stages are thin layers that reference assets living outside this
repository, so the repository stays small and carries no redistributed content.
Each scene below names its source. Paths are absolute in the committed `.usda`
files, matching the development workstation; adjust the `subLayers` path to
relocate an asset.

---

### Intel Sponza

**Source:** [Intel Graphics Research Samples](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)

A large textured architectural scene. Its bound materials are USD-native
`UsdPreviewSurface` networks, so this baseline exercises how hdClaude handles a
stage whose materials are not MaterialX.

```cmd
set "HDCLAUDE_SAMPLES_PER_PIXEL=1024"
set "HDCLAUDE_SAMPLES_PER_FRAME=32"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera PhysCamera001 gallery\intel_sponza.usda build\gallery-linear\intel_sponza.exr
```

**Current state.** Renders, and far darker than the same scene and camera in
hdCodex. The stage's lighting reaches the film but almost nothing bounces:
this is the scene that will show what MIS and a light-power heuristic are worth
(roadmap phase 6). Its `UsdPreviewSurface` materials are shaded through
`displayColor`, which is its own gap.

### OpenChessSet

**Source:** [usd-wg/assets OpenChessSet](https://github.com/usd-wg/assets/tree/main/full_assets/OpenChessSet)

Instancing, HDR dome lighting, and many distinct materials. This is the scene
that most directly measures whether per-material dispatch scales.

```cmd
set "HDCLAUDE_SAMPLES_PER_PIXEL=1024"
set "HDCLAUDE_SAMPLES_PER_FRAME=32"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera renderCam gallery\chess_board.usda build\gallery-linear\chess_board.exr
```

**Current state.** The closest of the ten to right: instancing, the dome light,
and the distinct piece materials all read correctly at 1024 samples.

### StandardShaderBall Gold

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

Artistic-metalness conductor response.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_gold.usda build\gallery-linear\shader_ball_gold.exr
```

**Current state.** The conductor reads as metal, the inner shell is neutral,
and the backdrop's printed numbers read the right way round. This is the image
that found the texture-orientation defect: the numbers were mirrored in every
render before it.

### StandardShaderBall Glass

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

Spectral dielectric transmission, dispersion, and total internal reflection.
The scene where four hero wavelengths earn their cost.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_glass.usda build\gallery-linear\shader_ball_glass.exr
```

**Current state.** Transmits. It rendered opaque black until the integrator
stopped evaluating refractions with the reflection closure, which is the defect
this scene exists to catch. Dispersion is still absent -- transport is RGB
until phase 6 -- so the glass is colourless where it should split.

### StandardShaderBall BubbleGum

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

Subsurface transport and image textures.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_bubblegum.usda build\gallery-linear\shader_ball_bubblegum.exr
```

**Current state.** Renders. Subsurface is a published closure parameter and not
yet transported, so the material reads as a diffuse surface.

### Pixar's KitchenSet

**Source:** [OpenUSD Kitchen Set](https://openusd.org/release/dl_kitchen_set.html)

Authored Z-up coordinates, deep instancing, and unbound meshes that fall back to
`displayColor`. Also the subdivision performance reference.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera renderCam gallery\pixar_kitchen.usda build\gallery-linear\pixar_kitchen.exr
```

**Current state.** Renders, and is the most expensive scene in the gallery at
about two minutes. It used to lose the Vulkan device inside scene publication;
the cause was an acceleration-structure scratch address that did not meet
`minAccelerationStructureScratchOffsetAlignment`, which is a build requirement
no allocator enforces. Its unbound meshes fall back to `displayColor` as the
asset intends.

### Collective Project 001

**Source:** [usd-wg/collectiveproject001](https://github.com/usd-wg/collectiveproject001/blob/main/shots/s001_001/index.usda)

UsdSkel deformation through Hydra `extComputation`, and per-face material
subsets. The scene that exercises in-place BLAS update under deformation.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --purposes render --camera mono gallery\collectiveproject001.usda build\gallery-linear\collectiveproject001.exr
```

**Does not render.** A material fails to generate: `No 'geomprop' parameter
found on geompropvalue node 'primvar'. Don't know what property to bind`. The
`geompropvalue` node arrives from `HdMtlxCreateMtlxDocumentFromHdNetwork`
without the input that says which primvar to read. hdClaude reports a material
it cannot compile rather than approximating it, and `usdrecord` treats that as
fatal, so the scene stops. Note that binding it would also need arbitrary
primvars in the geometry setter, which is phase 7 work.

### OpenPBR Playground

**Source:** [OpenPBRShaderPlayground](https://github.com/DigitalProductionExampleLibrary/OpenPBRShaderPlayground/blob/main/ShdrPlygrnd/ShdrPlygrnd_OpenPBR.usda)

The MaterialX/OpenPBR reference. This is the scene hdClaude's whole shading
commitment is aimed at: every material here must compile and execute through
the `genglsl_pt` target with no approximation and no fallback
([docs/materialx-codegen.md](docs/materialx-codegen.md) 8).

It is also the scene on which hdCodex reproducibly lost the Vulkan device.
Recorded here so the first hdClaude run of it is treated as evidence.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --purposes render --camera renderCam_mainCU gallery\openpbr_playground.usda build\gallery-linear\openpbr_playground.exr
```

**Does not render.** Two independent failures. `iceCube` reaches
`mx_aastep`, which calls `dFdx` -- the same class of finding as the `fwidth` one
already recorded in [implementation-notes.md](docs/implementation-notes.md):
a fragment-shader assumption inherited from `genglsl`, meaningless in a compute
stage where neighbouring lanes are unrelated paths. `OJfoam` fails earlier, in
generation. The stage also warns that `geometry_opacity` does not match the
`open_pbr_surface` declaration, which is a MaterialX version difference worth
separating from the other two.

It is also the scene on which hdCodex reproducibly lost the Vulkan device.
hdClaude has not reached that point on it.

### Subdivision Feature Matrix

A renderer-owned scene isolating six subdivision paths: an indexed face-varying
UV seam, a Catmull-Clark cube with edge creases and a sharp corner, a Loop
tetrahedron, a Catmull-Clark grid with a holed coarse face, two coarse-face
material subsets, and MaterialX displacement on a bilinear patch.

```cmd
set "HDCLAUDE_ENABLE_SUBDIVISION=1"
set "HDCLAUDE_SUBDIVISION_LEVEL=2"
set "HDCLAUDE_ENABLE_DISPLACEMENT=1"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\subdivision_features.usda build\gallery-linear\subdivision_features.exr
```

**Current state.** Renders, and is the cheapest scene in the gallery. The
displacement panel is flat, because displacement is phase 16.

### New Zealand Height Map

One authored bilinear quad displaced by a MaterialX `ND_image_float` height map
after uniform level-6 refinement. The same map drives surface colour, so texture
resolution and UV orientation are visible independently of the displaced
silhouette. The texture is committed under `gallery/textures/`.

```cmd
set "HDCLAUDE_ENABLE_SUBDIVISION=1"
set "HDCLAUDE_SUBDIVISION_LEVEL=6"
set "HDCLAUDE_ENABLE_DISPLACEMENT=1"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\newzealand_heightmap.usda build\gallery-linear\newzealand_heightmap.exr
```

**Current state.** Renders as a flat, uniformly coloured quad. The refinement
happens -- the quad is subdivided to level 6 -- but the height map drives
neither the silhouette (no displacement, phase 16) nor the colour, and a colour
that does not vary means the image node is not delivering the texture. That
makes this the sharpest test case for the texture path.
