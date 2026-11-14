# OpenUSD proposals hdClaude follows

Status: live. Last revised 2026-11-15.

Some of what hdClaude renders has no home in OpenUSD yet: light spectra,
photometric light power, camera spectral response, material spectra. Where an
OpenUSD proposal covers one of these, hdClaude reads the proposal's schema as
drafted, so that a scene authored for hdClaude today is authored for the
standard the proposal becomes. A draft can change, so this page records:

- what each proposal says, and where;
- what hdClaude implements of it, and how;
- where hdClaude had to decide something the draft leaves open;
- what to do when the draft moves.

**When a proposal moves:**
1. Re-read it.
2. Diff its schema against the "Implemented" column below.
3. Change the reader and its tests together.
4. Record the change here, in the decision log of [roadmap.md](roadmap.md),
   and in the gallery scenes that author it (`scripts/cornell_box.py`).

A name that changes is read under both spellings for one release, and the old
one is reported as superseded. Once a proposal lands in an OpenUSD release, the
registered schema replaces reading attributes by name.

## Physical lighting (UsdLux)

- **Where:** [PixarAnimationStudios/OpenUSD-proposals, `proposals/_notPublished/draft/physical-lighting`](https://github.com/PixarAnimationStudios/OpenUSD-proposals/blob/main/proposals/_notPublished/draft/physical-lighting/README.md)
- **Status (2026-11-15):** draft, unpublished. None of its schemas are in the
  OpenUSD 26.03 or 26.05 builds here.
- **Context:** OpenUSD 25.05 clarified that UsdLux's
  `intensity` x `color` x 2^`exposure` is luminance in nits, without adding
  parameters. This proposal adds them, and cites Weta's physLight as the model
  for its camera part.

| Schema | What it adds | hdClaude |
| --- | --- | --- |
| `PhysicalLightIlluminantAPI` | `token physical:illuminant` (white, blackbody, illuminant D, custom) and `float2[] physical:customIlluminant` (nm, radiance) | **Implemented** 2026-11-15 on every UsdLux light, domes included. See [spectral-rendering.md](spectral-rendering.md) 6 and `usd_physical_illuminant` |
| `PhotometricAreaLightAPI`, `RadiometricAreaLightAPI` | Power in lumens or watts, or illuminance or irradiance at a distance, via `HdLight::ComputePowerScaleFactor` | **Implemented** 2026-11-15 on rect, disk, sphere and cylinder lights, shaped or not. See `usd_physical_brightness` |
| `Photometric/RadiometricDomeLightAPI`, `Photometric/RadiometricDistantLightAPI` | Illuminance in lux or W/m², via `HdLight::ComputeIlluminanceScaleFactor` | **Implemented** 2026-11-15, domes with and without a map |
| `PhysicalCameraResponsivityAPI` | `float2[] physical:responsivity:r/g/b` on a `Camera` | Not implemented. The film uses the CIE 1931 observer, which remains the default |
| `PhysicalColorFilterArrayAPI` | A raw sensor's colour filter array and its filters | Not implemented. No raw output is planned |

**What hdClaude decided where the draft is silent:**
- **Whether the schema is applied.** It is not registered, so hdClaude treats
  it as applied when `physical:illuminant` has a value. Both names are read as
  written and with `inputs:` added.
- **"ASTM-E308" resampling.** E308 delegates abridged-data interpolation to
  its companion E2022. hdClaude uses that rule: Lagrange, four-point (cubic)
  inside and three-point (quadratic) in the first and last intervals. It keeps
  the authored bounds and every multiple of 5 nm between them. A clamped dip
  below zero is reported. *To raise:* the draft should name the procedure, and
  say what happens to negative interpolants.
- **"illuminant D" domain.** The draft defines only white and blackbody on
  [380, 780] nm. Daylight is tabulated on the same grid, and only from 4000 to
  25000 K, where CIE defines it; outside that range it is reported and the
  light emits its colour. *To raise.*
- **"white" versus no schema.** "white" is D65 on [380, 780] at 5 nm. A light
  without the schema emits D65 over hdClaude's full 360-830 nm, so the two
  differ outside 380-780, which is invisible: they match to 1e-4 in
  chromaticity.
- **Luminance of a coloured light on a non-white illuminant.** The draft
  defines L = luminance(`color`) x `intensity` x 2^`exposure`. hdClaude scales
  the illuminant to D65's luminous power, then multiplies it by the upsampled
  colour's spectrum. For a white colour that is exactly the draft's
  luminance. For a saturated colour on a strongly non-white illuminant the
  product's luminance is not luminance(`color`) exactly. *To raise:* the
  draft should say whether the colour is a tint applied before or after
  normalisation.
- **`enableColorTemperature` with the schema.** Ignored, and reported unless
  the illuminant is blackbody or illuminant D, which read
  `colorTemperature` themselves.

**The photometric and radiometric APIs, as implemented.** A light's
luminance is scaled by the target over what that luminance delivers, which is
the reciprocal the draft's `Compute...ScaleFactor()` methods return. What it
delivers is integrated from the light as the path tracer emits it: its shape
and world size, its UsdLuxShapingAPI cone and focus, and a dome's map. So the
light then delivers the authored number, and `usd_physical_brightness`
measures that in a render, to within 0.6%. Its intensity, exposure, colour
luminance and `normalize` cancel, as the draft's formula has them cancel.
Hydra does not tell a delegate the stage's `metersPerUnit` or up axis:
`HdSceneGlobalsSchema` carries the camera, the render settings and pass, the
time codes and the current frame, and nothing of the stage's metrics. A
UsdImaging adapter, though, is handed the prim and so the stage. hdClaude's
keyless adapter (`src/usdImaging/stageMetricsAdapter.cpp`) publishes
`UsdGeomGetStageMetersPerUnit` and `UsdGeomGetStageUpAxis` on every light as
`hdClaude:stageMetersPerUnit` and `hdClaude:stageUpAxis`. The
`metersPerUnit` (default 0.01, USD's fallback) and `upAxis` render settings
stand in where nothing published them -- a host that does not image through
UsdImaging. A setting that was set and contradicts the stage is reported,
and the stage's value is used. *To raise:* the stage's metrics belong in
`HdSceneGlobalsSchema`, where every delegate could read them without an
adapter of its own.

**What hdClaude decided where those APIs are silent:**
- **Whether one is applied.** As for the illuminant: an API counts as applied
  when any of its attributes has a value, and its unauthored attributes take
  the draft's fallbacks. Applying one implies `PhysicalLightIlluminantAPI`,
  which they inherit, so its illuminant is "white" unless authored.
- **Power or illuminance on an area light.** The draft says the illuminance
  "only has effect when" the distance is non-zero, and not which wins when both
  could apply. The distance's own documentation says "When non-zero, the
  light's brightness will be such that...", so a non-zero distance wins, and
  an authored power is reported as unused. An illuminance authored with no
  distance is reported as having no effect.
- **"A surface directly facing the light at" a distance.** Taken to be on the
  light's axis, at that distance from its centre, facing back along the axis.
  A sphere or cylinder whose surface would be at or inside that point is
  reported. *To raise:* the draft should say where the distance is measured
  from.
- **The formula against the definition.** The draft defines
  `k = 1 / (L(lambda) T D A)`, and also says `ComputePowerScaleFactor` makes the
  emitted power "exactly" the target. The two agree only if `D` includes the
  cosine to the emitting surface (`pi` for an unshaped flat light) and the
  light is flat; for a sphere or cylinder, `D A` is not its power. hdClaude
  implements the definition. *To raise.*
- **Radiometric targets and colour.** Watts are converted to lumens through
  the light's illuminant, integrated over the illuminant's own range (the
  draft's "domain"). The colour enters only through its luminance, as the
  draft's `L = luminance(color) ...` says, so a saturated colour's own spectral
  shape does not change the watts. For a white light that is exact.
- **The draft's fallbacks disagree with each other.** 1600 lm against 2.34 W
  is 684 lm/W, the efficacy of monochromatic light at 555 nm, and 10000 lux
  against 14.64 W/m^2 is 683 lm/W. The "white" illuminant both APIs default to
  is about 205 lm/W, so a light left at the radiometric fallbacks delivers
  about a third of the light it would at the photometric ones. *To raise.*
- **A dome's "upward-facing patch".** The stage's up axis, as published on
  the dome (or the `upAxis` setting where it was not). A uniform dome delivers pi times its luminance; a map's integral
  is taken over the upper hemisphere in the map's own placement, texel by
  texel. hdClaude's `environmentIntensity` setting still scales the
  environment after, as it scales any dome.
- **A distant light's illuminance** is the projected solid angle of the cone
  hdClaude samples, pi sin^2 of its half-angle, capped at pi once the cone
  covers the surface's hemisphere. UsdLux's `normalize` uses
  `(2 - sin^2) pi` beyond a hemisphere, which no surface can receive; the
  APIs ask for what a surface receives.
- **Textures and IES on area lights.** The draft's `T` is the light's texture
  integral. hdClaude does not sample an area light's texture or IES profile
  (both reported), so it integrates the light it renders, which has neither.

## Physical cameras

- **Where:** [usd-wg/OpenUSD-proposals, branch `physical-camera`, `proposals/physical-cameras`](https://github.com/usd-wg/OpenUSD-proposals/tree/physical-camera/proposals/physical-cameras)
- **Status (2026-11-15):** proposal. Its exposure part has landed in OpenUSD as
  `UsdGeomCamera`'s `exposure`, `exposure:fStop`, `exposure:time`,
  `exposure:iso` and `exposure:responsivity`, with
  `S = responsivity * time * (iso / 100) * 2^exposure / fStop^2`.
- **hdClaude:** applies that exposure through
  `HdCamera::GetLinearExposureScale()` (since `adce8ec`, 2026-11-01). The
  body, sensor and lens schemas (`PhysicalCamera`, `PhysicalCameraSensor`,
  `PhysicalCameraLens`, `PhysicalCameraLensModelPinhole`) are geometric and
  not implemented. hdClaude frames from `UsdGeomCamera`.
- **Discrepancy to raise:** the camera proposal's retired
  `physcam:responsivity:red/green/blue` extrapolate *linearly* outside their
  samples. The lighting proposal's `physical:customIlluminant` is *zero*
  outside its range. Both are tabulated spectra and should share one rule.
  The lighting proposal's `PhysicalCameraResponsivityAPI` does not say which.

## Material spectra

- **Status (2026-11-15):** no proposal found, in OpenUSD or in MaterialX. The
  AOUSD Materials Working Group's charter lists material models "to describe
  non-visual, non-surface properties such as broader EM-spectrum" as possible
  scope.
- **hdClaude:** `float[] inputs:hdclaude:spectrum:<input>:wavelengths` and
  `:values` beside a colour input
  ([spectral-rendering.md](spectral-rendering.md) 6). These are hdClaude's
  own, and are to be replaced by whatever a proposal defines.
- **Where it would belong:**
  - *Meaning* belongs to each shading library, since UsdShade only carries
    values: MaterialX for its nodes and the models built from them, OpenPBR's
    own specification for its model, UsdPreviewSurface's for itself, and any
    renderer's own library. hdClaude shades UsdPreviewSurface through
    MaterialX's graph of it, so a MaterialX rule would reach it here, but its
    specification would still have to say so.
  - *Encoding* belongs in USD, once, and should be shared by every library:
    the (nm, value) `float2[]`, zero outside its range, that the lighting
    proposal already uses for lights.
- **Open item:** move `hdclaude:spectrum:<input>:*` to a single `float2[]` per
  input, in that shape, so that material and light spectra are written the
  same way. This is a change of name and shape only; the semantics stay as in
  [spectral-rendering.md](spectral-rendering.md) 6.

## physLight (Weta FX)

- **Where:** [wetadigital/physlight](https://github.com/wetadigital/physlight),
  specification v1.3 (2023-08-05) in `docs/`.
- **Role:** the physical model the lighting proposal's camera part cites. It
  specifies lights by spectrum, tint and angular distribution, with
  brightness in luminous power or illuminance. The camera is specified by ISO,
  shutter, f-number and a calibration constant, plus measured spectral
  sensitivities, about 45 cameras in `data/camera`.
- **Its USD folder is a stub.** `usd/physAreaLight` is commented-out TODOs from
  USD's example plugins, with no schema, so there are no physLight attribute
  names to follow. The proposals above are the USD mapping.
- **Camera curves** are scene data, authored per camera through
  `PhysicalCameraResponsivityAPI` once implemented. They are not shipped with
  hdClaude.
