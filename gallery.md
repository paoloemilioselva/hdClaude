# Gallery

These images are versioned visual baselines, not golden-reference renders. They
exist so that an intentional improvement and an unintentional regression are
both visible in a diff.

**Status: all ten scenes render.** The renderer is under construction; see
[docs/roadmap.md](docs/roadmap.md). These images are what hdClaude produces
today, and every scene below says what its baseline still gets wrong. Parity with the hdCodex baselines is phase 8 and has not been reached.

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
| Intel Sponza | 2026-09-06 | 50.346 s (0m 50.346s) | `c550555b5fc987b063555e5959c6d7cd74260972c143627db91dcb6ff01075d3` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| OpenChessSet | 2026-09-06 | 25.695 s (0m 25.695s) | `346c22e8f3360a508bbf7f298af7692c0c2bbeae92c64e70fa9af389ef83dbb1` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Gold | 2026-09-06 | 25.681 s (0m 25.681s) | `34d521c3c0ec76559a21b716160c38b73bb79512e62843882212776845bd25d9` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Glass | 2026-09-06 | 27.554 s (0m 27.554s) | `be5abeff93faebe9433919fe5de9edd5bc6aab89e918b44c6287c5e24df1811b` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall BubbleGum | 2026-09-06 | 27.146 s (0m 27.146s) | `49be77c86c16dc0383a0ea56280d04e19504115d4be82eb5025298183c2e4d39` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Pixar's KitchenSet | 2026-09-06 | 123.045 s (2m 3.045s) | `9b2381ee7811f1319c641845ee51e14ec644bb87c4785b559e76ce0bd45dd8a8` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Collective Project 001 | 2026-09-06 | 21.707 s (0m 21.707s) | `963610e602da0eb5b787d941285b56ff9016ee686a83a6bffdb3bde656d0980a` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| OpenPBR Playground | 2026-09-06 | 93.244 s (1m 33.244s) | `ce6b3a3f4cf6fc5fe9aa770c622414f5f5bca4e23379e553ba7dea119470c3b5` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Subdivision Feature Matrix | 2026-09-06 | 12.899 s (0m 12.899s) | `4315f04b76b6fb8af0818c284f130c78b1967cc1404bdff83a5e08f2463bd015` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| New Zealand Height Map | 2026-09-06 | 7.904 s (0m 7.904s) | `eb707b7e775734929d9406fa01d506d521c0ec6050d0671733695c49f43204a2` | NVIDIA GeForce RTX 5060 Ti | 1024x1024, 1024 spp, 32/update, 8 bounces, subdiv 6 |
<!-- gallery-timings:end -->

## Against hdCodex

Phase 8 asks for parity with the hdCodex baselines. Mean display brightness of
the two renderers' images of the same stage, at the same camera and settings,
is the crudest possible comparison and the one that separates "different" from
"missing":

| Scene | hdCodex | hdClaude | reading |
|---|---:|---:|---|
| Intel Sponza | 0.353 | 0.004 | hdCodex shades 137 `UsdPreviewSurface` materials as flat grey; hdClaude shades the brick and stone the asset authors, in an arcade lit only by a stand-in sky |
| OpenChessSet | 0.558 | 0.424 | comparable |
| Pixar's KitchenSet | 0.192 | 0.274 | hdClaude brighter |
| Subdivision Feature Matrix | 0.344 | 0.316 | comparable |

Parity is not the goal where the two disagree about how much of the asset to
shade. What the comparison is for is finding the places where hdClaude is
missing something, and Sponza is now understood rather than suspect: the
estimator is confirmed by a white furnace -- a 0.8 Lambertian under a unit sky
renders 0.811 against a closed form of 0.800 -- and the same stage rendered
from outside is bright, textured and correct.

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
`UsdPreviewSurface` networks, which hdClaude shades through MaterialX's own
`ND_UsdPreviewSurface_surfaceshader` -- so this baseline is the test of that
path, not of a fallback.

```cmd
set "HDCLAUDE_SAMPLES_PER_PIXEL=1024"
set "HDCLAUDE_SAMPLES_PER_FRAME=32"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera PhysCamera001 gallery\intel_sponza.usda build\gallery-linear\intel_sponza.exr
```

**Current state.** Renders its authored materials -- 137 of them, and 25
textures where there were none -- and is still much darker than hdCodex's
baseline of the same stage. The difference is now understood rather than
suspected, and it is two things.

The stage authors no light at all, so both renderers are showing their own
stand-in. hdCodex's is brighter, and hdClaude's is now bright enough to light a
room rather than a tenth of that, which is what the stand-in is for.

The larger part is that hdCodex shades all 137 materials as flat grey
`displayColor` while hdClaude shades the brick, stone and fabric the asset
authors, and an interior of dark albedo under sky-and-sun is genuinely dark.
Matching the brighter image would mean shading less of the asset. What would
close the gap honestly is a stage that authors its own lighting, and an
environment density that samples where the sky actually is -- uniform sphere
sampling spends most of its shadow rays on the ceiling of a covered arcade.

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
and the ground now carries the region of its texture that it should -- the same
numbers in the same places as hdCodex. This scene found two texture defects:
every texture was uploaded upside down, and the ground and all five walls
author their UVs face-varying, which hdClaude used to reject and shade from
barycentrics instead.

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

**Current state.** Renders: the skinned character deforms through its
`ExtComputation`, the per-face material subsets read correctly, and the
electric arc between the antennae is a curve primitive reading a float geomprop
this renderer does not carry, so it takes that geomprop's zero and shades
white. It needed `UsdPrimvarReader` rewritten into the `geompropvalue` it
wraps, which MaterialX cannot read through a nodegraph interface.

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

**Current state.** Renders, after three unrelated refusals were resolved:
`mx_aastep`'s `dFdx`, an `open_pbr_surface` input this MaterialX does not
declare, and eighty-five UDIM textures whose `<UDIM>` token opened nothing. All
100 of its textures load: the ones that did not were a UDIM set authored on
tile 1003 rather than 1001, and three 16-bit masks this renderer used to
refuse -- not, as it looked, a missing TIFF decoder. The image is the noisiest
in the gallery, which is what an interior lit through small emitters looks like
while the analytic lights have no MIS of their own.

It is also the scene on which hdCodex reproducibly lost the Vulkan device.
hdClaude renders it in 46 seconds.

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
