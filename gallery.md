# Gallery

These images are versioned visual baselines, not golden-reference renders. They
exist so that an intentional improvement and an unintentional regression are
both visible in a diff.

**Status: all ten scenes render.** The renderer is under construction; see
[docs/roadmap.md](docs/roadmap.md). These images are what hdClaude produces
today, and every scene below says what its baseline still gets wrong. Judging
those images against the specifications -- USD, MaterialX, the spectral model,
and the physics of path tracing -- is phase 8, and it is not finished.

Each scene shows its baseline inline. The image is the committed artefact
itself, not a reduced copy of it: what a reader sees is the same file the
SHA-256 in the timing table identifies, so a preview cannot drift from the
baseline it stands for.

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

**Transport is spectral.** Every image below carries four hero wavelengths per
path; RGB appears only where the asset authors one and where the film resolves
the image. That has a visible cost at a fixed sample count, and it is worth
knowing before reading these images: a pixel's colour comes from four
wavelengths drawn at random, so a grey surface is neutral only in the mean and
every image is speckled with colour where an RGB renderer's would be smooth. It
also costs time -- between fifteen and forty per cent across these ten scenes.
What is not yet spectral: a MaterialX graph's own colour arithmetic, since the
upsampling happens where a closure hands back its response. Dispersion is
transported, and a path meeting a dispersive interface keeps its hero lane and
terminates the other three, which costs four times the noise on those paths.

There is no displacement — that is phase 16, so the height map's quad is refined
and flat.

The renderer's output is always scene-linear. `render_gallery.bat` records
temporary EXRs under `build/gallery-linear`, then writes the display JPEGs
through a neutral HDR highlight compressor and the standard sRGB transfer
function. `HDCLAUDE_GALLERY_EXPOSURE` sets display exposure in stops; the
versioned baselines use the default of zero.

Each row also carries the device memory the render peaked at, and each scene has
a committed `gallery/<name>.stats` beside its `.usda` with the whole breakdown --
what the scene *is* in one group and what it cost on one machine in another, so
a diff says which of those changed. `build/gallery-timings.json` carries the same
figures for the table's own use: how long the
scene took to ingest and to publish, how long refinement took and how many points it
produced, how many materials and textures were compiled and loaded and what they
weigh, and the camera rays traced. The figures come from the renderer through
`GetRenderStats()`, because only it knows what it is holding -- including the
rays, which are counted on the device and read back once after the frame. The
ratio of traced rays to camera rays is how far paths actually get in a scene, and
it ranges from 1.14 on the subdivision matrix to 4.16 on the glass ball at
identical settings.

Two of the figures in that group are not costs but identities. `hitHash` is a
hash over every hit the render resolved -- which instance, which triangle, and
which path found it -- and `rayHash` the same over the origin and direction bits
of every ray traced. Neither means anything on its own; what they are for is
being equal. Two runs of a scene that agree on both traced the same rays and
found the same geometry, and the scene group is compared against the committed
file before it is replaced, so a run that did *not* reproduce says so in the
suite's output instead of arriving quietly as a diff. That is not hypothetical:
the committed `chess_board.stats` carried a `tracedRays` written by a run that
had the renderer's intermittent nondeterminism, and nothing at the time noticed.

**Wall time is not render time, and on some scenes it is mostly not.** The
`[cost]` group splits it: `traceMs` is the path tracing itself, and
`outsideSeconds` is what the wall clock saw that no stage of the renderer
claims -- opening the stage, USD's plugin discovery, Hydra populating its scene
index before the delegate is asked for anything, writing the EXR, and tearing
the device down. It is not a fixed startup tax: it runs from 4.9 s on the height
map to 61.1 s on the Kitchen Set, scaling with the stage rather than with the
render, while the four shader balls share one asset and sit at a flat 8.3 s
however long they trace. Three quarters of the height map's six seconds are not
rendering; nor are two fifths of the Kitchen Set's two and a half minutes. Read
`traceMs` for what the renderer cost and the table's wall time for what a person
waits.

A change there is reported rather than fatal. The image gate already fails a
render that moved, and a deliberate change that legitimately alters how far
paths travel should not have to fight the suite to land -- so the suite says
what moved and leaves the reading to a person.

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
| Scene | Measured | Wall time | Device memory | SHA-256 | Device | Settings |
|---|---:|---:|---:|---|---|---|
| Intel Sponza | 2026-09-16 | 120.526 s (2m 0.526s) | 5.5 GiB | `c5da684b3866b127fafa520f2e50a18f9af09fc125e830994384ae307d4ab1a2` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| OpenChessSet | 2026-09-16 | 60.760 s (1m 0.760s) | 1.7 GiB | `f4537169c3f1ef9b91e1b5af0e3bd961135dc77f955590efac6654bf8c6efcf3` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Gold | 2026-09-16 | 115.558 s (1m 55.558s) | 1.4 GiB | `9187c371a81ca307a9ab8518bd5d719d2932dd29c5c8bbec2d02aafefa0e269c` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Glass | 2026-09-16 | 48.607 s (0m 48.607s) | 1.4 GiB | `08a406e69f4ed2494bb1dd4e3aa446d486fd1640284fb2a7d3f0fc4b9610f817` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall BubbleGum | 2026-09-16 | 89.352 s (1m 29.352s) | 1.4 GiB | `399cb9aeb5537de51c713177ad508c4989955e06b3e0ef073d785cd9c60cf266` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| StandardShaderBall Honey | 2026-09-16 | 51.796 s (0m 51.796s) | 1.4 GiB | `7fb362476349ee063d1ac879106a30a403b394554322d2edbcb8415ce3045285` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Pixar's KitchenSet | 2026-09-16 | 202.951 s (3m 22.951s) | 1.3 GiB | `00b231edeea1963a6330dc4334268fcf15aa2c1ef9d7d0b7105969923d2d0508` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Collective Project 001 | 2026-09-16 | 46.685 s (0m 46.685s) | 1.1 GiB | `588c2c18f4665bf13ea0d5c4ab87a5bb45d43b445f61cb608eadf478ef476b67` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| OpenPBR Playground | 2026-09-16 | 108.453 s (1m 48.453s) | 12.7 GiB | `560562e8386a34fcf9220e7f8f98b0a285a4e592804d280e72ae1f8fb9905e99` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| Subdivision Feature Matrix | 2026-09-16 | 34.998 s (0m 34.998s) | 832.0 MiB | `679c8ef413b04f9dd948483e15ff9f6aa5a012b581e719cce2823623310788f4` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 2 |
| New Zealand Height Map | 2026-09-16 | 24.974 s (0m 24.974s) | 480.0 MiB | `325b060e49bcf2b2d074eeb5c20e345f41861797a86e95db018d8fadcb228ecb` | NVIDIA GeForce RTX 5060 Ti | 1024 px wide, 1024 spp, 32/update, 8 bounces, subdiv 6 |
<!-- gallery-timings:end -->

## Against hdCodex

**hdCodex is a second opinion, not the answer.** Phase 8 does not ask for parity
with it. Correctness comes from the specifications, and some of that renderer's
images are themselves wrong -- it draws no lights at all, and shades 137 of
Intel Sponza's materials as flat grey -- so a difference is a question rather
than a defect. The question is which renderer the specification agrees with, and
the reading in each row is the answer to that, not a distance still to close.
Several of these divergences are hdClaude being right.

What the comparison is good for is saying where to look. It is what revealed
that the Kitchen Set's committed baseline had been wrong from the day it was
adopted, which no self-comparison could have caught. Mean display brightness of
the two renderers' images of the same stage, at the same camera and settings, is
the crudest possible measure and the one that separates "different" from
"missing":

| Scene | RMS vs hdCodex | mean | reading |
|---|---:|---:|---|
| StandardShaderBall Gold | 0.068 | 0.674 | its mirror now reflects the lights, which hdCodex does not draw at all |
| StandardShaderBall BubbleGum | 0.078 | 0.714 | |
| StandardShaderBall Glass | 0.112 | 0.677 | the largest deliberate divergence in the gallery: the glass holds the highlights of the five rect lights, and hdCodex's image of the same stage has none, because neither renderer used to draw a light. Dispersion still absent |
| Subdivision Feature Matrix | 0.134 | 0.310 | the creased cube is a cube again, now that subdivision tags reach the refiner |
| Pixar's KitchenSet | 0.056 | 0.226 | RMS fell by more than half once instanced meshes stopped losing their transform inside the prototype, and again when the stand-in sun learned which way is up: the stage is Z-up, and the sun's hardcoded Y-up direction had been running horizontally through the room |
| OpenChessSet | 0.045 | 0.556 | 0.199 four changes ago: a normal-map frame, then its dome kept its range and started being sampled where the light is, and now it is lit by that dome alone -- a dome is not in the light table, so the stand-in sun had been adding a second key light to a scene that lights itself |
| OpenPBR Playground | 0.246 | 0.402 | brighter than hdCodex now rather than darker; its float textures no longer clamp |
| Intel Sponza | 0.368 | 0.018 | hdCodex shades 137 `UsdPreviewSurface` materials as flat grey; hdClaude shades the brick and stone the asset authors, in an arcade lit by a stand-in sky and a stand-in sun, which now stands 70 degrees above the horizon and so reaches the floor and the far columns rather than raking the near wall. Roughness and metalness now reach those materials: the asset connects its scalar inputs to a nodegraph's three-component `outputs:rgb`, which `gallery/intel_sponza.usda` overrides to the single channel the maps carry |

Three of these moved *away* from hdCodex on 2026-09-07, when the analytic
lights became emitters a ray can hit. hdCodex does not draw a light either, so
its images have no light in any reflection, and the closer hdClaude gets to what
the scene actually describes the further it reads from that. The glass ball is
the clearest case and the largest divergence in the table.

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

![The Sponza arcade, almost entirely black -- a faint suggestion of columns and arches is all that is above the noise floor](gallery/intel_sponza.jpg)

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

![A marble chess set on a green stone board, all thirty-two pieces placed, under a studio HDRI](gallery/chess_board.jpg)

**Source:** [usd-wg/assets OpenChessSet](https://github.com/usd-wg/assets/tree/main/full_assets/OpenChessSet)

Instancing, HDR dome lighting, and many distinct materials. This is the scene
that most directly measures whether per-material dispatch scales.

```cmd
set "HDCLAUDE_SAMPLES_PER_PIXEL=1024"
set "HDCLAUDE_SAMPLES_PER_FRAME=32"
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera renderCam gallery\chess_board.usda build\gallery-linear\chess_board.exr
```

**Current state.** All thirty-two pieces, in the right places, under the right
half of the HDRI. It took both: the pawns are a `PointInstancer` whose
instances hdClaude was not placing, and the dome was sampled half a turn out of
USD's orientation, so the scene was lit and backed by the wall behind the
camera.

The stone pieces author subsurface, and since 2026-09-08 they transport it: the
marble bleeds instead of reading as painted plaster.

The pieces were faceted and the board carried a fine herringbone until two
defects in how a normal map is read were fixed together: the tangent frame was
taken from a triangle edge rather than solved from the texture coordinates, so
every map was applied at a rotation that changed per triangle, and the maps
themselves -- 8-bit JPEGs on `vector3` and `float` nodes -- were being
sRGB-decoded, which tilts a flat normal map and exaggerates every bump. Every
material in this asset is a `<normalmap>`, so the set showed both at once.

Its dome then stopped being clamped. The 4k HDRI behind this scene is a float
image, and every texture was being quantised into eight bits, so the window and
the lamps that are the whole of its light were flattened to white. Restoring
the range made the image brighter, correct in the mean, and covered in
fireflies -- which is what uniform sphere sampling of a real HDRI produces --
until the environment gained a distribution built from its own luminance. What
the scene shows now is a room lit through a window, with the window's light
where it belongs. RMS against hdCodex has gone 0.199, 0.108, 0.060 across the
three changes.

### StandardShaderBall Gold

![The StandardShaderBall in polished gold, on a backdrop printed with large numbers that the metal reflects](gallery/shader_ball_gold.jpg)

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

> **Asset note, shared by all four shader balls — for the asset's authors.**
>
> `materials/neutral`, which the StandardShaderBall binds to the **base** and to
> the **internal sphere**, ships `maps/neutral.ACEScg.exr` — the
> "Material Preview - 4 cm Grid" lettering the asset's own
> `thumbnails/standard_shader_ball_scene.png` shows around the plinth. Its
> MaterialX surface never reads it.
>
> `neutral`'s `outputs:mtlx:surface` points at a **NodeGraph**, not at a shader,
> and that nodegraph holds *two* surface shaders. Which one its `outputs:out`
> reaches is chosen by a `material_model` variantSet on the enclosing
> `materials` Scope, whose values are `standard_surface` and `OpenPBRSurface`.
> The asset defaults it to `OpenPBRSurface`.
>
> The two variants of `neutral` are not equivalent, and they are meant to be:
>
> - `standard_surface` selects `mtlxstandard_surface1`, which has
>   `inputs:base_color.connect -> mtlximage1`, the correct EXR. The plinth is
>   lettered.
> - `OpenPBRSurface` selects `open_pbr_surface1`, which has **no `base_color`
>   connection at all**. The whole of its reachable network is `emission_color`
>   and `emission_luminance`, both fed from `mtlximage2` — an `ND_image_vector3`
>   with no `inputs:file` authored — plus `specular_weight = 0`. `mtlximage1` is
>   not reached, and the plinth is blank.
>
> Checked, rather than argued: flipping `material_model` to `standard_surface` on
> the unmodified asset reaches `mtlximage1` and the map, with no override of any
> kind. The two variants are otherwise identical — same emission wiring, same
> zero specular — so the OpenPBR one is short exactly one connection.
>
> The sibling materials say the same thing from the other side. `sss_bars` and
> `uvgrid` wire `base_color -> mtlximage1` on **both** of their surface shaders,
> so both of their variants are complete; only `neutral`'s OpenPBR one is not.
> And `neutral`'s third representation, `outputs:surface` ->
> `usdpreview/usdpreviewsurface1`, reads the map into `diffuseColor`. So a
> renderer taking the `mtlx` context under the asset's own default selection is
> the only one that loses the lettering, which is what hdClaude did until
> 2026-09-08.
>
> A second, smaller thing in the same material: `mtlximage2` has no `inputs:file`
> and is wired to both emission inputs. hdClaude reads such a node's *default*
> rather than the magenta missing-texture placeholder — a node with no file is an
> ordinary authored value, not a broken asset reference — so it contributes
> nothing and nothing warns about it.
>
> **What this repository does about it.** The four `gallery/shader_ball_*.usda`
> entrypoints sublayer the asset and author the one missing connection as a local
> `over` on `neutral/mtlx/open_pbr_surface1`. The vendored asset is not edited,
> and removing those four overrides reproduces it as published.
>
> Selecting the `standard_surface` variant instead would also produce the
> lettering, and is not what this gallery does. That variantSet is on the
> `materials` Scope, so it would switch `neutral`, `sss_bars` and `uvgrid` to a
> different surface model while the example material, the box and the walls stay
> on `OpenPBRSurface` — three separate `material_model` variantSets, all of which
> the asset defaults to OpenPBR. These scenes are meant to render the asset in
> the configuration it publishes; the override keeps that and repairs the single
> connection, where flipping the variant would work around it and hide it.


### StandardShaderBall Glass

![The StandardShaderBall in clear glass, with the printed backdrop visible through and reflected in it](gallery/shader_ball_glass.jpg)

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

Spectral dielectric transmission, dispersion, and total internal reflection.
The scene where four hero wavelengths earn their cost.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_glass.usda build\gallery-linear\shader_ball_glass.exr
```

**Current state.** Transmits, and since the plinth carries its authored
lettering the refracted image through the ball is legible rather than a blank
grey -- which is most of what this scene is for. It rendered opaque black until
the integrator stopped evaluating refractions with the reflection closure, which
is the defect this scene exists to catch.

The plinth and the internal sphere are shaded through a local override; see the
asset note under [StandardShaderBall Gold](#standardshaderball-gold).


### StandardShaderBall BubbleGum

![The StandardShaderBall in pink coated plastic on the same printed backdrop](gallery/shader_ball_bubblegum.jpg)

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

Subsurface transport and image textures.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_bubblegum.usda build\gallery-linear\shader_ball_bubblegum.exr
```

**Current state.** Subsurface is transported. The asset authors
`subsurface_color (1, 0.22, 0.493)` with a radius of `(1, 0, 0.068)` at a scale
of 0.0325, so red has a mean free path of 0.0325 where green has none at all and
blue has 0.0022: red travels through the thin parts of the ball and the other
two do not, which is why the pink deepens and the thin handle lights from
within. It read as a flat diffuse surface until 2026-09-08, because
`subsurface_bsdf` published a medium and never sampled a direction into it.

The colour is OpenPBR's `subsurface_color`, which is documented as the light
that comes back *out* and is not the fraction of a collision that survives; van
de Hulst's inversion between them is what makes the ball render the colour it
was authored with rather than about a third of it. The green channel's zero
radius is the case that has to be regularized, and OpenPBR's own text says so.

What is missing: there is no next-event estimation at a scattering vertex, so
the interior is lit only by what its walk runs into on the way out, and the
walk's collision cap is reached rather than avoided where the mean free path is
small against the object -- which biases where the light leaves, not how much of
it there is.

The plinth and the internal sphere are shaded through a local override; see the
asset note under [StandardShaderBall Gold](#standardshaderball-gold).


### StandardShaderBall Honey

![The StandardShaderBall in amber honey on the same printed backdrop, golden where the shell is thin and deepening to red where the light passes through the most material](gallery/shader_ball_honey.jpg)

**Source:** [usd-wg/assets StandardShaderBall](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall),
with an `open_pbr_surface` authored in `gallery/openpbr_honey.mtlx`

An interior medium, which is a different thing from a tinted surface. OpenPBR
hands a transmissive material's colour to its *volume* whenever
`transmission_depth` is above zero -- the surface tint is deliberately dropped --
so this scene renders identically to clear glass in any renderer that publishes
the medium and never transports it, which is what hdClaude did until 2026-09-07.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --camera camera gallery\shader_ball_honey.usda build\gallery-linear\shader_ball_honey.exr
```

**Current state.** Renders, with Beer-Lambert absorption over the distance each
path travels inside the shell and a random walk for the scattering the asset
authors at 0.9, which is what makes it cloudy rather than a clear amber.
Both coefficients are spectral, and the walk weighs the four lanes' densities
against each other by the balance heuristic once per walk. Its albedo is
(1.0, 0.552, 0.229) -- red scatters losslessly and blue is absorbed -- and that
ratio survives being resolved to wavelengths only because the medium is carried
as an extinction and an albedo rather than as two coefficients fitted to spectra
separately, which is what deepened the amber on 2026-09-08. There is still no
next-event estimation at a scattering vertex, so the medium is lit by what
enters it and the estimate is noisier than an opaque surface's.

The plinth and the internal sphere are shaded through a local override; see the
asset note under [StandardShaderBall Gold](#standardshaderball-gold).


### Pixar's KitchenSet

![A crowded kitchen at dusk: a blue refrigerator papered with notes on the right, a green stove beside it, a counter of crockery and utensils, a stool and a rug, and a table with a red chair and a red mug in the left foreground](gallery/pixar_kitchen.jpg)

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

Until recently its refrigerator, stove, table and chairs were simply not in the
image, and the props that were had drifted into the air. Nothing was missing:
all 1788 mesh instances the stage holds were published, each at its instancer's
placement and *without* its own transform inside the prototype, which 1460 of
the 1462 meshes have. Every model's parts therefore collapsed onto that model's
origin. It is the defect that says most about this gallery's limits -- the gate
compares a render against its own committed baseline, so it could never flag a
baseline that was wrong when adopted, and it took hdCodex's image of the same
stage to see it. RMS against hdCodex fell from 0.194 to 0.074.

### Collective Project 001

![An orange robot with two antennae, one hand raised, standing in a spotlight against a dark magenta wall](gallery/collectiveproject001.jpg)

**Source:** [usd-wg/collectiveproject001](https://github.com/usd-wg/collectiveproject001/blob/main/shots/s001_001/index.usda)

UsdSkel deformation through Hydra `extComputation`, and per-face material
subsets. The scene that exercises in-place BLAS update under deformation.

```cmd
render_claude.bat --imageWidth 1024 --colorCorrectionMode disabled --purposes render --camera mono --frames 1246 C:\Users\paolo\Desktop\code\collectiveproject001\shots\s001_001\index.usda build\gallery-linear\collectiveproject001.exr
```

**Current state.** Renders: the skinned character deforms through its
`ExtComputation`, the per-face material subsets read correctly, and the
electric arc between the antennae is a curve primitive reading a float geomprop
this renderer does not carry, so it takes that geomprop's zero and shades
white. It needed `UsdPrimvarReader` rewritten into the `geompropvalue` it
wraps, which MaterialX cannot read through a nodegraph interface.

The eye is a lens. Until 2026-09-08 it was frosted glass, and nothing in the
asset asked for that: its face is a `standard_surface` with `transmission 1` and
a clear interior, but `standard_surface` instantiates `subsurface_bsdf`
unconditionally and has no volume node at all, so hdClaude's hand-off filled
every transmissive material of that kind with the default subsurface medium --
a dense scattering interior nobody authored. The concentric rings behind the
lens are what the fix looks like.

### OpenPBR Playground

![A child's craft table under a desk lamp, crowded with toys, jars and paper, with the scene's remaining sampling noise visible](gallery/openpbr_playground.jpg)

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

**Current state.** Its green jar and the purple toy on the shelf transport
subsurface as of 2026-09-08; both read as opaque diffuse before that.

Its 85 UDIM textures also find the tile they belong to as of the same day. Every
tile of a set used to be shaded with the set'"'"'s first existing image, which for a
single-tile set is right and for this scene was not: the rolled mat on the shelf
was grey rather than teal, the toy beside it had lost its colour, the jar its
label, and the books their separate covers.

It renders after three unrelated refusals were resolved:
`mx_aastep`'s `dFdx`, an `open_pbr_surface` input this MaterialX does not
declare, and eighty-five UDIM textures whose `<UDIM>` token opened nothing. All
100 of its textures load: the ones that did not were a UDIM set authored on
tile 1003 rather than 1001, and three 16-bit masks this renderer used to
refuse -- not, as it looked, a missing TIFF decoder.

It was also a blizzard of fireflies on every surface, and that turned out not
to be a sampling problem: 132 of its material nodes are `<normalmap>`, and both
of the defects the chess set exposed -- an edge-derived tangent frame and
sRGB-decoded data maps -- perturb a normal into directions the surface never
faces, which is a firefly generator. With those fixed the image is still the
noisiest in the gallery, which is what an interior lit through small emitters
looks like while the analytic lights have no MIS of their own, but it is noise
rather than a hash. Its 195 meshes that author face-varying `primvars:normals`
are also read now, though most of them are `catmullClark` and so take the limit
surface's normals rather than the cage's.

Its float textures then stopped being quantised into eight bits, which
brightened it: it now reads brighter than hdCodex where it used to read darker,
and RMS against that baseline moved the wrong way, 0.235 to 0.262. That is a
real change in the right direction being measured against a baseline this
renderer is not yet trying to match. What it costs is time -- 55 seconds to
103 -- because a hundred textures at sixteen bits a channel is twice the
sampler bandwidth. The scene has no dome light, so the environment distribution
does nothing for it, and the firefly cluster on the toy aeroplane's fuselage is
still there: one material, not the whole image, and not yet diagnosed.

It is also the scene on which hdCodex reproducibly lost the Vulkan device.
hdClaude renders it in 55 seconds.

### Subdivision Feature Matrix

![Six subdivision test shapes on a flat blue-grey ground: a dark textured sphere, a sharp-edged brown cube, and a small cyan form above; three quads below, the middle quad split olive and violet by its two material subsets](gallery/subdivision_features.jpg)

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

The creased cube was a smooth brown blob until 2026-09-07, when subdivision
tags started reaching the refiner. Its creases, corners and boundary rule had
been dropped on the way in, so the one shape in the gallery whose whole purpose
is to prove creases survive refinement was proving the opposite -- against a
baseline that had recorded the rounded version as correct.

The UV-seam sphere was a flat magenta until 2026-09-07: its material named
`newzealand_height_map.png`, which lives under `gallery/textures/`, so the file
was never found and hdClaude drew the failure placeholder it draws for a texture
it cannot read. The baseline had encoded that placeholder since the scene was
adopted, so the seam test asserted nothing about a seam -- the gate only ever
compared one flat colour against the same flat colour. `check_usd_materials.py`
found the broken path on its first run, which is the argument for the tool in
one line.

### New Zealand Height Map

![A dark brown quad seen in perspective, with the two islands of New Zealand picked out in green by the height map, and no relief at all](gallery/newzealand_heightmap.jpg)

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

**Current state.** The map reaches the surface: both islands are legible in
the quad's colour, which is what says the image node, the UV orientation and
the texture resolution are all right. The quad is refined to level 6 and is
still perfectly flat, because displacement is phase 16 -- so what this scene
now isolates is displacement alone, where it used to be failing at the texture
before it ever got there.
