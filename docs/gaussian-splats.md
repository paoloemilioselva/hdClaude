# Gaussian splats

Status: design of record. Written 2026-09-22.

`UsdVolParticleField3DGaussianSplat` reaches hdClaude as an ordinary Hydra
rprim, and hdClaude path traces it. This document states what the schema
specifies, what it leaves unspecified, which transport model hdClaude estimates
and why, and what that model can and cannot do.

Related documents:

- [Architecture](architecture.md) — the three commitments and the wavefront design.
- [Roadmap and phase tracker](roadmap.md) — phase 18, its exit gate, its evidence.
- [Spectral rendering](spectral-rendering.md) — how an RGB radiance becomes four lanes.

## 1. What the schema specifies

Verified against the OpenUSD install the project builds on (26.03, and
identical in 26.05).

`ParticleField3DGaussianSplat` is a **concrete Gprim** deriving from
`ParticleField`, so it is transformable, boundable, instanceable, carries
`purpose` and `visibility`, and can have a material bound to it. It
auto-applies six API schemas, which together are the whole of the data:

| Applied schema | Attribute | Type | Absent means |
| --- | --- | --- | --- |
| `ParticleFieldPositionAttributeAPI` | `positions` / `positionsh` | `point3f[]` / `point3h[]` | no particles at all |
| `ParticleFieldOrientationAttributeAPI` | `orientations` / `orientationsh` | `quatf[]` / `quath[]` | no rotation |
| `ParticleFieldScaleAttributeAPI` | `scales` / `scalesh` | `float3[]` / `half3[]` | unit scale |
| `ParticleFieldOpacityAttributeAPI` | `opacities` / `opacitiesh` | `float[]` / `half[]` | fully opaque |
| `ParticleFieldKernelGaussianEllipsoidAPI` | — | — | the kernel itself |
| `ParticleFieldSphericalHarmonicsAttributeAPI` | `radiance:sphericalHarmonicsCoefficients[h]`, `uniform int radiance:sphericalHarmonicsDegree` | `float3[]` / `half3[]`, `int` | a DC signal of (0.5, 0.5, 0.5) at degree 0 |

Five rules from the schema documentation that are requirements rather than
advice, and that hdClaude implements as written:

1. **`positions` defines the particle count.** Every other per-particle array
   is *truncated* if longer, and **discarded entirely** if shorter — in which
   case the attribute's documented default is used for every particle. Not
   padded, not partially honoured.
2. **The float flavour wins.** Each attribute exists in a `float` and a `half`
   form; the schema says consumers should prefer `float` where it is authored,
   and supplies `UsesFloatPositions()` and its four siblings to decide, keyed on
   whether the float array is authored and non-empty.
3. **`opacities` are linear**, "in line with the traditional (linear) sense of
   computer graphics opacity, **not** the transformed data sometimes seen in PLY
   files associated with gaussian splats, where the values need to be processed
   with a sigmoid activation function."
4. **`scales` are linear**, "and not specified in log-format as is sometimes
   seen in PLY files".
5. **The kernel is fully specified.** An untransformed kernel "will define
   opacity at point `p` by `g(u=0; o=1; x = p.length())`. Note that since the
   standard deviation is 1, the 3-sigma point is 3.0 and 99.7% of the splat
   support is within a spherical region of radius 3." Per-splat opacity
   multiplies the falloff; scale and orientation turn the sphere into an
   ellipsoid.

Rules 3 and 4 matter more than they look. They mean **PLY conversion is the
asset's job, not the renderer's.** hdClaude applies no sigmoid and no
exponential on the way in, and an asset whose opacities or scales are still in
their trained encoding is reported, never repaired — the same rule that governs
every other malformed input.

Two attributes on the splat prim are explicitly rasterizer hints:
`projectionModeHint` (`perspective`/`tangential`) and `sortingModeHint`
(`zDepth`/`cameraDistance`). Both are documented as tuning that "renderers are
free to ignore", and both describe a projection-and-sort pipeline that a ray
tracer does not have. hdClaude intersects the 3D kernel itself and therefore
honours neither — not as an approximation of them, but because it answers the
question they exist to approximate. It says so once per prim rather than
silently.

Two sibling kernels are declared and are not part of the 3DGS prim:
`ParticleFieldKernelGaussianSurfletAPI` (a Gaussian disk in local XY, opacity
zero off the plane, support radius 3) and
`ParticleFieldKernelConstantSurfletAPI` (a hard disk, opacity 1 inside radius
1). They are flat rather than solid, which is the only difference that reaches
the intersector.

## 2. What the schema does not specify

Recorded here because each one had to be decided, and a decision taken in the
absence of a specification belongs in writing.

### 2.1 An opacity field is not an extinction coefficient

The kernel defines *opacity* — dimensionless, in [0, 1]. Transport needs
*extinction*, in reciprocal length. The two are not interchangeable, and the
gap is not a matter of picking a constant: **no density field reproduces alpha
compositing of 3D Gaussians for all ray directions.** The line integral of a
Gaussian along a ray scales with the Gaussian's standard deviation *along that
ray*, which depends on direction, so a per-splat density that matched the
authored opacity for one direction would not match for another.

This is the central finding. It is a property of the 3DGS formulation that the
schema faithfully carries, not a defect in the schema.

### 2.2 Where along a ray the kernel is evaluated

The schema describes a field in 3D and hints at how a *rasterizer* should
project it. For a ray it says nothing. hdClaude evaluates the kernel at its
**maximum response along the ray**, which is available in closed form: with
`A = inv(S) transpose(R) d` and `b = inv(S) transpose(R) (o - mu)`, the response
is greatest at `t* = -(A.b)/(A.A)` and equals
`exp(-0.5 (|b|^2 - (A.b)^2/|A|^2))`.

That choice is the per-ray form of the question `projectionModeHint` asks
about: it is exact for every ray rather than exact for one projection, it is
view-consistent, and it is what the ray-traced 3DGS literature uses. It costs
two three-vector transforms and three dot products per candidate.

### 2.3 The spherical-harmonics conventions

The schema pins the basis normalisation only by implication: it says an absent
or discarded SH array should behave as "a SH coefficient corresponding to a DC
signal of (0.5, 0.5, 0.5), with degree 0". With the standard real SH basis,
`Y(0,0) = 1/(2 sqrt(pi))`, so that coefficient is `0.5 * 2 sqrt(pi) = sqrt(pi)`.
Two consequences:

- The coefficients multiply the **normalised real SH basis**, and there is **no
  `+0.5` offset** — unlike the reference 3DGS implementation, whose colour is
  `0.5 + Y(0,0) * f_dc`. A converter must fold the offset into the DC
  coefficient.
- Coefficient *ordering* is not stated, but `elementSize = (degree+1)^2` is, and
  `elementSize` in USD means contiguous per-element blocks. hdClaude reads the
  array as **particle-major**: all of a particle's coefficients together, in the
  conventional `l`-major, `m`-ascending order.

**The direction convention is genuinely absent.** Nothing says whether the
radiance is a function of the direction from the particle toward the viewer or
the reverse, and getting it backwards mirrors every view-dependent highlight in
the asset. hdClaude evaluates the SH in the direction **from the particle toward
the viewer** — the outgoing radiance direction, which is what "radiance" means
everywhere else in this renderer — and reports the assumption, so that an asset
which disagrees is diagnosable rather than merely wrong. This should be raised
upstream.

### 2.4 No extent computation, and no albedo

`UsdGeomBoundable::ComputeExtentFromPlugins` returns nothing for the type: USD
registers no extent plugin for `ParticleField`. hdClaude therefore computes the
extent itself, as the particle positions expanded by **each particle's own
kernel support** — the 3-sigma ellipsoid, not the bare point cloud, because a
bounding box that clipped the falloff would clip the image.

And the schema carries radiance, never reflectance. **There is no albedo in a
Gaussian splat**, so there is nothing to make a BSDF out of; see section 4.

## 3. The transport model hdClaude estimates

3DGS is defined by front-to-back alpha compositing:

```text
C = sum_i  c_i a_i  prod_{j<i} (1 - a_j)
```

That expression is **exactly the expectation** of a much simpler process: walk
the ray in depth order, and at each particle stop with independent probability
`a_i` and return its colour. The probability of stopping at particle `i` is
`a_i prod_{j<i}(1 - a_j)`, so the expected returned radiance is `C`, term for
term.

hdClaude therefore renders splats as **stochastic coverage**. For each candidate
particle along a ray, evaluate `a_i` at the peak response, draw one uniform, and
commit an intersection at that `t` only if it accepts. What this buys is not a
shortcut but a list of things that stop being problems:

- **No sorting.** Vulkan's committed-intersection rule already returns the
  nearest accepted candidate, so the depth ordering the estimator needs comes
  out of traversal. `sortingModeHint` has nothing left to tune, and the popping
  that per-frame sorting produces cannot occur.
- **No projection.** The kernel is intersected where it is, so the perspective
  distortion that `projectionModeHint` exists to trade against does not arise.
- **Correct interleaving with everything else.** A splat hit competes with
  triangle and curve hits on `t` alone, so splats occlude and are occluded by
  ordinary geometry, appear in reflections and through refraction, and are
  blurred by depth of field and motion blur — with no compositing pass and no
  separate ordering authority.
- **Shadow rays for free.** Independent draws per candidate in the `shadow`
  kernel deliver `prod(1 - a_i)` in expectation, which is the transmittance the
  compositing model implies. Splats occlude scene lights.
- **It is the schema's own image.** Nothing is approximated and no constant is
  invented; the estimator's mean is the definition.

The cost is variance where a rasterizer has none, which is the ordinary cost of
estimating any integral by sampling, and which the accumulation the renderer
already has resolves.

A splat hit is an **emitter**: the SH radiance is evaluated in the outgoing
direction, upsampled through the existing *emission* path — unbounded, magnitude
carried, because a radiance is not a reflectance — added to the film scaled by
the path throughput, and the path terminates. Once coverage has accepted, the
particle is opaque by construction, so there is nothing behind it to gather and
no new shading dispatch is needed: no MaterialX program runs, which is why this
does not contradict commitment 1.1. It is the `displayColor` situation, not an
approximated material.

Splat emission is found by **implicit hits only**; there is no next-event
estimation toward a splat cloud. That is the same trade the integrator already
records for its medium walk and for delta closures, and it is recorded rather
than hidden: a glossy surface lit only by a splat cloud will be noisier than one
lit by an analytic light.

### 3.1 The volumetric reading, later and opt-in

The alternative is to read the kernel as a density —
`sigma_t(x) = sum_i o_i G_i(x)`, emissive and non-scattering — and integrate the
volume rendering equation through it with the spectral walk `extend` already
performs. That is physically meaningful, order-free, and lets a splat cloud sit
inside fog or behind a participating medium on equal terms. It needs a length
scale the schema does not supply (section 2.1), and it does not reproduce the
appearance the asset was trained for.

It is therefore a **second, opt-in model** behind a render setting rather than
the default, and the difference between the two is to be measured on a real
asset and recorded, not asserted.

## 4. What this cannot do, stated plainly

The spherical-harmonics coefficients are **baked outgoing radiance**. A splat
emits and occludes; it cannot be lit, cannot be relit, casts no coloured shadow,
and adds no indirect bounce of its own beyond the light it emits. This is a
property of the representation: the schema has no reflectance anywhere in it, so
there is no quantity from which a BSDF could be derived. A renderer that
appeared to light splats would be inventing the albedo.

There is one principled way forward, and it is a later phase rather than a
footnote. `ParticleField` is a Gprim, so a material **can** be bound to it. A
bound MaterialX `volumeshader` would supply the optical properties — extinction,
single-scattering albedo, phase anisotropy, emission — while the splat kernel
supplies only the *spatial* modulation of density. That is commitment 1.1
applied to a volume, it needs no invented constant beyond section 2.1's, and
`anisotropic_vdf` and `uniform_edf` already exist as `genglsl_pt` closures. What
is missing is the `volumeshader` terminal in the material compiler, which today
handles `surfaceshader` and `displacementshader` only.

## 5. How the data is laid out

Object space, like every other prototype: one acceleration structure per
distinct splat cloud, placement by TLAS instance transform, so a cloud
instanced by a `PointInstancer` builds once.

Per particle the traversal kernel needs the centre, the opacity, and the nine
floats of `inv(S) transpose(R)` — the map from an object-space offset into the
kernel's unit space, where the falloff is `exp(-0.5 |x|^2)`. The rows of that
matrix are the columns of the rotation divided by the corresponding scale, which
is why the quaternion and the scales are resolved on the host once rather than
per candidate. Thirteen floats a particle, packed with no padding because every
hdClaude buffer is `scalar`-layout, exactly as a curve segment packs ten.

Spherical harmonics are a second buffer, `3(degree+1)^2` floats a particle,
particle-major. At degree 3 that is 192 bytes against 52 for everything else, so
a million-particle capture is about 244 MB as `float` and about 148 MB if the
asset authored the `half` flavour — which is the reason rule 2 above resolves to
the authored precision rather than widening everything on load.

The acceleration structure is built over **3-sigma axis-aligned boxes**, one per
particle. For the ellipsoid kernel the half extent along axis `k` is
`3 sqrt(sum_j (R[k][j] s_j)^2)`; for a surflet the sum runs over the first two
columns only, because the support is a disk. The radius is the spec's own 99.7%
figure rather than a number chosen here.

## 6. What is measured, not looked at

The gate for this work is closed-form, because a splat cloud is exactly the kind
of image that can look plausible while being wrong:

1. **One particle has an analytic profile.** A single splat, seen down an axis,
   must produce `o exp(-0.5 d^2)` across a row of pixels, to Monte Carlo error.
2. **Two overlapping particles must match the analytic composite**
   `c1 a1 + c2 a2 (1 - a1)` **regardless of traversal order** — the assertion
   that catches a reused random draw or a dependence on which box was visited
   first, neither of which a picture shows.
3. **Transmittance through a cloud must match `prod(1 - a_i)`** on a shadow ray,
   so that occlusion and visibility agree with each other.
4. The SH basis is checked for orthonormality by numerical integration, and the
   documented default coefficient must evaluate to 0.5 in every direction.

Items 1 to 3 are renderer gates; item 4 is a core unit test and runs without a
GPU or a USD runtime.
