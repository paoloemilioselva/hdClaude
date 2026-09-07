# Spectral rendering

Status: design of record. Last revised 2026-09-07.

hdClaude transports light spectrally. RGB is an asset-input and display-output
format; it is never the path throughput representation. This document specifies
the wavelength model, the RGB-to-spectrum upsampling, and the sensor.

**What is implemented, as of 2026-09-07.** The wavelength model of 1 and the
sensor of 4; the upsampling of 2 in the form described below, with one
deliberate difference recorded there; and the transport itself -- a path carries
four lanes end to end. Dispersion is transported, in the reduced form 1
describes below: a dispersive interface keeps the hero lane and terminates the
other three rather than weighing them against each other. **Not** implemented:
the wavelength MIS that would keep them, thin film, and 3 as written --
upsampling happens where a closure hands back a response rather than at each
texture fetch inside a MaterialX graph, so a material's own colour arithmetic is
still RGB. Sections describing the end state say so where they differ.

## 1. Hero wavelength packets

Each path carries **four correlated wavelengths** in a `vec4`, sampled once at
ray generation and held for the life of the path.

```text
lambda_0 ~ importance-sampled from the CIE Y visual response over [360, 830] nm
lambda_i  = wrap(lambda_0 + i * (range / 4)),  i = 1..3
```

The rotation gives stratified coverage of the visible range from one sample,
which is the standard hero-wavelength construction. Four lanes rather than
three because a `vec4` is the natural GPU register and memory width — three
lanes cost the same registers as four and waste one — and because MIS over four
hero wavelengths measurably reduces colour noise on dispersive and thin-film
materials, which are exactly the cases spectral rendering exists to get right.

**Wavelength MIS.** When a material is wavelength-dependent (dispersive
refraction, thin film, or any spectrally varying IOR) the path becomes
chromatic: the four lanes would follow different directions. hdClaude keeps the
path on `lambda_0` and applies single-wavelength MIS with the balance heuristic
over the four lanes, terminating the other three into the film. This is
unbiased and avoids maintaining four geometric paths.

*As implemented,* the collapse happens without the MIS weight: the hero lane
continues at its own index and the other three are terminated outright. That is
still unbiased, because `lambda_0` is drawn from the film's own density and the
surviving lane is scaled by the lane count the film divides by -- the estimator
becomes the single-wavelength one rather than an average of four. It is
correspondingly four times noisier on any path that meets a dispersive surface,
which is what the balance heuristic over the lanes' densities would recover.
The compensation is applied exactly once per path, which needs a flag: a path
that enters a glass slab and leaves it meets the same dispersive material twice,
and three empty lanes are indistinguishable from a terminated path.

**Every mode uses four lanes.** The interactive preview reduces samples per
pixel and path length. It does not reduce lane count. Dropping to one lane adds
chromatic noise that is not zero-mean over a short temporal window — noise a
temporal reconstructor will lock in — in the image the user navigates with.
See [lessons](lessons-from-hdcodex.md) R6.

## 2. RGB to spectrum

MaterialX materials author colours in RGB. Converting them to spectra must be
smooth (no ringing), bounded, and energy-conserving where the quantity is a
reflectance.

Three distinct upsampling roles, because they have different constraints:

| Quantity | Method | Constraint |
| --- | --- | --- |
| Reflectance, transmittance, albedo | Jakob-Hanika sigmoid polynomial, of the *chromaticity*, scaled by the magnitude | must stay in [0, 1] at every wavelength |
| Emission, light colour | Smits-style non-negative basis, unnormalised | must stay >= 0; unbounded magnitude |
| IOR, extinction | two-term Cauchy from the authored index and Abbe number | physical, authored, not upsampled |

**Reflectance.** The Jakob-Hanika model represents a spectrum as
`S(lambda) = sigmoid(c0*l^2 + c1*l + c2)` with `l` a normalised wavelength. The
sigmoid guarantees `S in [0,1]` for any coefficients, so an upsampled reflectance
cannot exceed unity at any wavelength — no upsampled albedo can create energy.
Coefficients come from a precomputed 3D table over the sRGB gamut, built at
first use and cached under the same key scheme as compiled shaders.

**Emission.** Bounded upsampling is wrong for emission: a light of RGB (5,5,5)
must produce a spectrum integrating to five times white, which no [0,1] model
can express. Emission divides out the magnitude, upsamples the chromaticity that
remains, and reapplies it -- preserving chromaticity exactly and integrating to
the authored luminance while staying non-negative everywhere.

**Reflectance uses the same decomposition**, which is a deliberate departure
from fitting the colour directly. It makes the round trip exact by construction
rather than by convergence, since the scale cancels; and it makes a grey
upsample to a *flat* spectrum, which is what a grey physically is. The cost is
that every colour's spectrum is a scaled saturated one where a direct fit would
find something flatter for a desaturated colour. Both are metameric under D65
and both are smooth and bounded; they differ under a narrowband illuminant,
which nothing here is yet.

An emitted spectrum is further multiplied by the illuminant its RGB was authored
against, so a white light emits D65 -- that is what an RGB emitter means in a
D65-referred pipeline -- and the film divides by the same illuminant's luminous
integral, which is what makes a white surface under a white light resolve to
white.

**Round-trip requirement.** Upsampling followed by CIE integration under the
D65 illuminant must return the original sRGB value to within 1e-3 dE2000 for
all in-gamut colours. This is a phase 1 exit gate, tested without a GPU.

## 3. Textures

Texture texels are upsampled with the same reflectance model, on the GPU, at
sample time — not by precomputing spectral textures, which would multiply
texture memory by the lane count. The sigmoid coefficient lookup is a single
trilinear fetch from the shared coefficient table, so the cost is one extra
texture read per texel fetch and no extra residency.

Colour space is honoured before upsampling: MaterialX filename colour-space
metadata selects sRGB or raw decoding, with the USD/ASWF `srgb_tx` alias mapped
to MaterialX `srgb_texture`. Upsampling a display-encoded value would be wrong
in a way that is hard to see and impossible to correct downstream.

## 4. Lights

`UsdLux` lights are spectral at the source:

- `enableColorTemperature` produces a **Planckian blackbody spectrum** at the
  authored temperature, not an RGB approximation of one. This is the single
  most visible spectral difference from an RGB renderer.
- Authored `color` is upsampled with the emission model.
- HDR dome and area-light textures are upsampled per texel at sample time, as
  in §3.
- `intensity`, `exposure`, `normalize`, `diffuse`, and `specular` scale the
  spectrum, not a colour.

## 5. The sensor

The film integrates CIE XYZ and converts to linear sRGB **once**, at the film,
after accumulation:

```text
per path: for each lane i, accumulate radiance_i * xyzbar(lambda_i) / pdf(lambda_i)
film:     XYZ -> linear sRGB (Bradford-adapted D65)
output:   scene-linear, no tone map, no display encoding
```

The renderer's output is always scene-linear. Gallery JPEGs are produced by a
separate display transform applied after rendering, so the versioned baselines
and the renderer's numerical output stay independent.

CIE 1931 2-degree colour matching functions are used, tabulated at 1 nm and
evaluated by linear interpolation, held in the core library as data so the
sensor is testable with no GPU.

## 6. What is deliberately not done

- **No spectral upsampling of normals, roughness, or other non-colour inputs.**
  They are not colours.
- **No full spectral texture storage.** Cost without benefit; see §3.
- **No fluorescence and no polarisation.** Both are real, both are out of scope,
  and both would change the transport equation rather than extend it.
- **No RGB fast path.** There is no mode in which hdClaude transports RGB.
  Offering one would create two estimators to validate and a permanent
  temptation to use the wrong one.
