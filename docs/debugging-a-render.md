# Debugging a render

Status: live. Written 2026-09-16, from the session that found why the OpenPBR
Playground burned at thirty-two bounces.

This is method rather than findings. The findings are in
[implementation-notes.md](implementation-notes.md); what is here is how they were
arrived at, including the parts that wasted a day, because the wasted parts are
the ones worth not repeating.

## 1. Turn the report into a series that must converge

A report is a perception: "too much indirect", "it burns at high bounces". The
first move is to turn it into a number that has a *predicted shape*.

For light transport the cheapest is a **bounce ladder**: render the same frame at
1, 2, 4, 8, 16 and 32 bounces and read the mean. Each extra bounce must add less
than the one before, and the series must converge. The Playground read 0.105,
0.168, 0.206, 0.230, 0.275, 1.53 -- and the last term is not a scene converging.
That single series established there was a defect, bounded where it lived (only
between sixteen and thirty-two did it explode), and told the difference between
the two failures it could have been:

* **Bias** moves the mean smoothly and leaves the image clean.
* **A multiplicative gain** compounds: the mean jumps and a handful of pixels
  reach 10^5 while the rest are unchanged.

Read the *maximum* and the count of extreme pixels beside the mean.
`hdClaudeImageDiff --scan` prints all three, and its negative and non-finite
counts have caught defects the mean never would.

## 2. Eliminate by measurement, never by argument

Every suspect gets an experiment that could exonerate it, and the experiment runs
before the next hypothesis is formed. In this session four confident hypotheses
died this way -- the thin-walled delta's acceptance cone, a negative extinction
in a medium, Russian roulette on negative spectral lanes, and thin-walled mode
itself -- and each had a plausible story behind it that would have survived any
amount of discussion.

The order that worked, cheapest first:

1. **Disable a whole strategy.** Dropping next-event estimation took the peak
   from 124,828 to 2,754, which said NEE delivered the energy. (It did not say
   NEE *caused* it, and that distinction cost an hour.)
2. **Bisect the scene.** Deactivate groups of prims; deactivate lights one at a
   time; turn the dome off and the analytic lights off separately.
3. **Bisect the materials.** Material ids are what the GPU knows, so
   `scene_store.cpp` traces `material <id>: <prim path>` under `HDCLAUDE_TRACE`.
   Drive the range from an environment variable read into a push constant, so
   **one build serves every step** of the bisect rather than one build per step.
4. **Cap one factor of the estimate at a time** -- the closure's response, the
   light's density, the light's radiance, the path's throughput -- and read the
   plain image maximum. Whichever cap changes the image names the factor. This
   is what finally isolated a per-scatter gain of four.

## 3. Instrument by removing, not by writing

Two attempts to have the film *report* a quantity (a material id, then a
logarithm of the offending factor) were useless twice over, and the reasons
generalise:

* The film converts a spectral packet to RGB. Anything written into radiance
  comes back scaled by the spectral basis and divided by the sample count, so
  the number read is not the number written.
* The image's maximum was an ordinary firefly rather than the marker, so the
  readout measured the scene instead of the experiment.

Capping a factor and reading the unmodified image answered the same question with
no scaffolding. When a marker really is needed, it must be the *only* thing the
image can contain: zero everything else and terminate the path, or it is
measuring the film.

## 4. Two results that mean "your experiment did not run"

* **Identical to the last digit.** A shader edit that leaves the mean and the
  maximum bit-identical did not take effect, or its branch is never reached.
  Treat it as a fault in the experiment until proven otherwise -- and check the
  install (below), since hdClaude renders through the *installed* plugin.
* **Every variant reporting the same number.** Two diagnostics keyed on the same
  selector collided in this session and silently produced four runs of identical
  nonsense. Give each diagnostic a disjoint range and assert the ranges do not
  overlap.

## 5. Verify the install before believing a render

`compile.bat`'s install step can fail while the build succeeds -- a usdview
session, or a crashed render still holding the DLL, is enough. The failure is a
line in the middle of a long log and the tests then run against the *previous*
plugin, which is how a suite can report ten passes about code that was never
installed. Compare hashes:

    (Get-FileHash <install>\plugin\usd\hdClaude.dll).Hash -eq
      (Get-FileHash build\dev-dlss\plugin\usd\hdClaude.dll).Hash

## 6. Ask what the renderer received, not what the asset authored

Two defects in one day came from the difference between the two. ALab's lights
were read as casting no shadows because UsdImaging turns a valueless attribute
into `false`; the Playground's paper carries a subsurface albedo of four because
a `colorcorrect` node multiplies a texture by four on the way in.

`HDCLAUDE_TRACE` prints what each light and material actually became -- "shadows
no" was the whole of the first finding. When the value is texture-driven, no
host-side report can attribute it, which is why
`scripts/check_scene_materials.py` reads the *graph*.

## 7. Furnaces: closed shapes, and long paths

A furnace is the strongest instrument here because its answer is known exactly:
a lossless surface in a uniform environment renders one. Two properties of the
test matter as much as the material under it.

* **A closed shape, not a quad.** A path crosses a quad once and leaves; it
  enters a sphere, crosses, re-enters and crosses again. A per-crossing error of
  a few per cent is invisible on a quad and unmistakable on a sphere. The
  thin-walled sheet measured 1.8400 against a closed form of 1.8462 on a quad
  and 0.0769 on a sphere.
* **Long paths.** A furnace at three bounces cannot see a gain of four per
  scatter compounding over thirty-two. Every furnace worth having is run at
  several bounce limits, and the numbers must agree across them.

## 8. What a gain means, and what to do about it

`response / pdf` is what a scattering event multiplies a path by. Above one, the
estimator diverges: the mean grows with path length, no sample count converges,
and the image fills with fireflies rather than getting brighter. An albedo -- the
fraction of arriving light that leaves -- above one is that, and it is clamped
where it is used, as it already was for the volumetric subsurface lobe and for a
medium's single-scattering albedo.

Clamping is the renderer's only option and it is not a repair: the asset is still
wrong, and the report that says so belongs at the USD level where the graph is
visible.

## 9. Compare against a closed form, not against another renderer

Another renderer is a second opinion, never the standard. When one disagrees,
build the smallest scene whose answer is known and measure both against it:

* `tests/usd/closed_form_rect_light.usda` -- a Lambertian floor under a rect
  light, answered by the configuration factor. A flat floor cannot see itself,
  so this isolates direct illumination and does not depend on bounce limits.
* `tests/usd/closed_form_cavity.usda` -- a sealed box that emits and reflects,
  answered by `L_e / (1 - rho)`. This is the measurement a convex furnace cannot
  make, since a sphere cannot see itself; it tests the whole interreflection
  series, which is where renderers actually differ.

Two cautions learned by getting them wrong. A test scene must apply
`MaterialBindingAPI`, or hdClaude resolves no binding and shades its own
fallback -- a 0.5 grey Lambertian, which is close enough to many test materials
to pass for one. And check what the other renderer's image really contains: a
Houdini Apprentice licence burns a watermark into every Karma render, which is
where a `max = 1.0` in an otherwise dim EXR comes from.

## 10. Housekeeping that cost time

* Run one heavy render at a time. Two ALab-sized stages at once took the machine
  into swap and the background job was killed.
* A wait loop must watch the process that actually exists: `usdrecord` runs as
  `python`, so a loop watching for `usdrecord` exits immediately and reports
  success on a render that never happened.
* Diagnostics are removed before the commit, and the commit is checked for the
  word `DIAGNOSTIC` before it is made.
