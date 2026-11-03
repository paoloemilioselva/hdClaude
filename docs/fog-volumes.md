# Fog volumes

Status: implemented 2026-11-03. Tested by `tests/usd/fog_volumes.py` (ctest
`usd_fog_volumes`), which checks closed forms rather than reference images.

A **fog volume** is a mesh bound to a material whose only terminal is a
MaterialX `volume`. The mesh is not a surface. It is the boundary of a
participating medium: invisible, index-matched, and crossed without a shading
event. The medium inside is described per point by the material's graph, so
it can vary with position. A ground fog that thins with height is the
motivating case, and it is authored like this:

```
position(object) -> extract y -> density field
anisotropic_vdf(absorption, scattering, g1) -+
anisotropic_vdf(absorption, scattering, g2) -+- mix -> volume.vdf
uniform_edf(emission * density)                  -> volume.edf
```

This is a different mechanism from the interior media the integrator already
had. An interior is entered at a *refracting* surface, is homogeneous, and is
carried on the path. A fog has no surface, varies in space, overlaps other fog,
and contains surfaces. It also differs from a splat cloud, whose density comes
from particles and whose optical properties come from the schema rather than
from a material.

## 1. What the material compiler produces

A network with a `volume` terminal and no `surface` terminal is compiled by
`HdClaudeMaterialCompiler` into:

- the stand-in diffuse program, so the material has a slot in the material
  table. No path ever shades it (section 2);
- `CompiledMaterial::volume`, a MaterialX document of *patterns* that
  `AuthoredVolume` extracts from the VDF and EDF trees.

A volume is transported by the integrator, not shaded. What traversal needs is
the medium's coefficients at a point, so `AuthoredVolume` flattens the closure
tree wherever MaterialX's definitions make it linear:

| Node | Becomes |
| --- | --- |
| `anisotropic_vdf` | absorption, scattering, and one Henyey-Greenstein lobe |
| `absorption_vdf` | absorption |
| `mix` (VDF) | both sides, weighted `mix` and `1 - mix` |
| `add` (VDF) | both sides |
| `multiply` (VDF) | every coefficient, by the float or colour weight |
| `uniform_edf` | emission per unit length |
| `mix`, `add`, `multiply` (EDF) | as for a VDF |

Any other node is reported and contributes nothing. The output is a nodegraph
`hdclaude_volume` with `absorption`, `scattering`, `emission`, and for each of
up to `kMaxVolumeLobes` (4) lobes an `anisotropy<i>` and a `weight<i>`. A
lobe's weight is the mean of its scattering coefficient, which makes it that
lobe's share of the phase function. Lobes beyond the fourth still scatter;
they are reported, and the phase function keeps the first four lobes' shape.

`CompileTraversal` generates every volume material's patterns into the same
library as the cut-out opacities. The generator emits
`hdclaude_material_volume(material)` and
`HdclaudeMedium hdclaude_material_medium(material)`, with one `case` per
volume material, and `SpliceOpacityLibrary` defines
`HDCLAUDE_TRAVERSAL_VOLUME`. A material with both a surface and a volume is a
surface enclosing a medium. That combination is not transported: its surface
is shaded as authored and a warning says the volume was dropped.

### Units

Every coefficient is **per scene unit**, and so is the emission. The MaterialX
specification gives VDF rates in m⁻¹; hdClaude deliberately does not apply
`metersPerUnit`:

- `standard_surface` and `open_pbr_surface` build their interior coefficients
  from `transmission_depth`, which their own specifications give in scene
  units. Scaling every VDF would break each of them.
- Production fog is authored with the stage's unit already folded into the
  density. The test scene this was built against is in centimetres and
  multiplies its density by a `densityUnitScale` of 0.01. Read as m⁻¹ and
  scaled again, its fog would have an optical depth of about 0.01, which is
  invisible.

This closes roadmap open item 11.

## 2. Boundaries

`PathTracer::SetScene` builds a fog table (`FogVolume`, binding 37) from every
visible placement bound to a volume material whose prototype is a
single-material triangle mesh, up to 32. Each entry records the placement, the
material and the object-space bounds. Such a placement gets TLAS mask
`kInstanceMaskFog` (0x10) and *no other bit*, so:

- every surface query asks for `HDCLAUDE_MASK_SURFACES` (0xEF) instead of
  0xFF, and never meets a boundary;
- the fog walk alone asks for 0x10.

**Which volumes contain a point is decided by parity.** `hdclaude_fog_crossings`
finds every boundary crossing along the ray *to infinity*, one closest-hit
query per crossing, up to eight. A volume crossed an odd number of times
contains the origin. This gives the right answer for a closed mesh whichever
way its faces wind. The path carries no record of which fog it is in: the
interior media's record cannot nest, and fog overlaps itself and contains
glass, foliage and cameras routinely.

A path inside an interior medium (glass, skin) does not walk fog. A fog is
outside every closed surface it contains.

## 3. The majorant

Delta tracking needs an upper bound on the extinction. The medium is an
arbitrary graph, and only evaluating it says what it is, so the bound is
*sampled*. At the start of every `Render` call, `extend` runs in its majorant
mode (push constant `mode = 1`): 32³ invocations per volume, each evaluating
a lattice point and a jittered point of the volume's object-space bounds. The
largest extinction found, times a margin of 1.5, is folded into
`fogMajorants` (binding 38, per frame slot) with `atomicMax` on the float's
bits.

A medium that emits needs points to gather emission at even where it has no
density, so its majorant is at least four samples across its diagonal.

A graph with features finer than a lattice cell can exceed the bound. The walk
then treats that point as having no null collisions. It under-attenuates there
rather than failing, and smooth fog fields are nowhere near the margin.

## 4. Transport

### Camera and bounce rays: `extend`

After everything else has decided where a ray ends, `hdclaude_fog_walk` walks
the fog over `[0, tEnd]` interval by interval between crossings. It uses
**spectral tracking** (Kutz et al. 2017): one free flight for the four lanes,
against the sum of the majorants of the volumes containing the interval. Each
tentative collision is an absorption, a scattering or a null collision, with
probabilities taken from the lanes' *average* coefficients. The throughput
takes each lane's own coefficient over that average, so every lane is
unbiased. An achromatic fog, where every ratio is one, is plain delta
tracking.

The coefficients are resolved to wavelengths as the interior media's are. The
extinction goes through `hdclaude_lane_extinction`, and the albedo is
upsampled as the reflectance it is. A grey skips both fits and is exact.

**Emission** is gathered at every tentative collision as `j / majorant`. In
expectation that integrates `T(t) j(t)` along the ray: points arrive at the
majorant's rate for as long as the walk survives, and it survives to `t` with
probability `T(t)`. MaterialX's EDF in a volume is radiance per unit length,
which is what `j` is.

A **scattering** collision is a vertex. It has no material, so the per-material
sort has nothing to sort it into. `extend` handles it in place:

1. `hdclaude_fog_next_event` samples an emitter, with the surface version's
   choice, light linking and multiple importance sampling. The phase function
   stands where a closure's response and density were. The contribution goes
   to the shadow queue: this path is not shaded this bounce, so it is its one
   shadow ray.
2. A direction is drawn from the phase function. A lobe is picked by weight,
   then its Henyey-Greenstein is sampled. The mixture's density is the value
   of the phase function, so the throughput is unchanged, and
   `pathScatterPdf` holds that density for the environment kernel's MIS.
3. Roulette applies as at a surface vertex, and the path goes onto the next
   bounce's queue with record `HDCLAUDE_HIT_MEDIUM`. The sort, the shading
   kernels and the environment kernel all skip that record.

A fog vertex consumes a bounce. On the last bounce it samples a light and stops.

### Light samples: `shadow`

A shadow ray that nothing occludes is attenuated by `hdclaude_fog_transmittance`,
which is **ratio tracking** over the same crossings and majorants. It is
unbiased and never a hard zero where the fog is thin, so a lamp seen through
haze does not flicker. Once the transmittance falls below 0.1, Russian
roulette ends rays that would deliver almost nothing.

This is the change that made next-event estimation *from inside a medium*
possible. Decision 2026-09-15 left NEE out at interior scattering vertices
because every interior boundary deviates a ray. A fog boundary does not.

## 5. What is not done

- **A surface enclosing a medium** (`surface` and `volume` terminals on one
  material). The surface is shaded, and the volume is dropped with a warning.
- **`UsdVol` volumes and OpenVDB/NanoVDB fields.** These would reuse this walk
  with a grid-sampled medium in place of a graph-evaluated one.
- **A majorant grid.** One bound per volume is enough for fog, whose density
  varies smoothly. A dense, strongly varying medium would want the splat
  walk's per-cell bounds (gaussian-splats.md 7.4).
- **Chromatic phase functions.** Lobe weights are the mean of each lobe's
  scattering, so lobes with different chromaticities share one phase shape per
  point.
- **Reconstruction guides at a fog vertex.** A primary ray that scatters in
  fog reports the sky's guides.
- **Cull volumes do not cull fog.** A boundary carries the fog bit alone, so
  no cull volume's mask reaches it, and the walk does not clip its intervals
  by them.
