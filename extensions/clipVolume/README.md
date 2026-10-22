# Clip volumes

Status: proposal and prototype. Schema drafted 2026-10-22; the Hydra adapter
and the hdClaude implementation are not written yet (see
[Status](#status-in-this-repository)).

A **clip volume** is a simple closed shape -- a cube or a sphere -- placed in a
USD scene. Inside it, a renderer ignores ray hits on geometry. Primary
(camera) rays and secondary rays are controlled separately, and a collection
says which geometry a volume clips. The aim is to propose this as an addition
to OpenUSD, so the schema is kept small and has no renderer-specific parts.
hdClaude is where it is first implemented and tested.

## Contents

- [Why](#why)
- [Use cases](#use-cases)
- [The schema](#the-schema)
- [Semantics a renderer must implement](#semantics-a-renderer-must-implement)
- [Design decisions](#design-decisions)
- [Hydra mapping](#hydra-mapping)
- [Implementing it in a ray tracer](#implementing-it-in-a-ray-tracer)
- [Open questions](#open-questions)
- [Status in this repository](#status-in-this-repository)

## Why

Hiding geometry from some rays and not others is a common production need.
USD has no way to express it per region of space:

- **`visibility`** and **`purpose`** apply to the whole prim and to every ray.
  To open a hole in a wall, you have to split the wall or hide all of it.
- **Camera `clippingRange` and `clippingPlanes`** apply to all geometry. They
  only use infinite planes, and they only exist for the camera. A near clip
  that moves the camera "through" a wall also cuts away everything else
  closer than that distance.
- **Light linking** chooses which lights affect which geometry. It has no
  notion of where in space a hit happens.

Several production renderers already ship their own version of this feature,
so it exists in practice but can't be exchanged through USD. One example is
V-Ray's *VRayClipper*, which has an option to affect camera rays only. (This
prior-art list must be checked against current renderer documentation before
the proposal is submitted.)

## Use cases

### Seeing inside a closed space

The camera has to be outside a set or building, for framing or lens choice,
but the shot is of the interior. Today the choices are:

- hide wall pieces, which changes the lighting inside;
- move the near clip, which cuts away everything at that distance and needs
  retuning every time the camera moves;
- break the set into camera-specific variants.

A cube clip volume around the part of the wall that faces the camera, with
`affectsSecondaryRays = false`, removes the wall from the camera's view only.
The wall still casts shadows, still blocks the sun, and still bounces light,
so the interior looks the same as it would with the wall in place.

This is useful in offline rendering, in virtual production (where a camera
often sits behind set walls that exist for lighting), and in games and
real-time previews of architecture.

See [`examples/cutaway.usda`](examples/cutaway.usda).

### Shaping light through dense foliage

A jungle shot has the camera under a canopy that is far too dense for the
look the director wants, and the canopy is above the frame. Sphere clip
volumes placed in the canopy, outside the frustum, remove leaves and branches
for every ray. Sunlight then reaches the ground through them as pools and
shafts of light.

The canopy does not have to be edited or re-dressed, and the change is local,
easy to animate, and can be undone. The clip volumes are art direction, kept
on their own and in the shot layer, not baked into the set.

See [`examples/canopy.usda`](examples/canopy.usda).

### Others the same schema covers

- Architectural and product cutaways ("section views").
- Pushing a camera through a wall during a move, with a clip volume keyed to
  the camera.
- Removing an occluder from shadow rays only, to open up a light on a
  character without changing what the camera sees
  (`affectsPrimaryRays = false`).

## The schema

Definition: [`usdClipVolume/schema.usda`](usdClipVolume/schema.usda).

```text
Xformable
 └── ClipVolume              (abstract; built-in CollectionAPI:clipLink)
      ├── CubeClipVolume     size   (as UsdGeomCube)
      └── SphereClipVolume   radius (as UsdGeomSphere)
```

| Property | Type | Fallback | Meaning |
| --- | --- | --- | --- |
| `clipVolume:affectsPrimaryRays` | `bool` | `true` | Clip camera rays. |
| `clipVolume:affectsSecondaryRays` | `bool` | `true` | Clip every other ray, including shadow rays. |
| `collection:clipLink:*` | CollectionAPI | `includeRoot = true` | The geometry this volume clips. |
| `size` (`CubeClipVolume`) | `double` | `2.0` | Edge length; the cube spans `[-size/2, size/2]` on each axis. |
| `radius` (`SphereClipVolume`) | `double` | `1.0` | Sphere radius. |

Both flags are animatable, and so are the transform and the shape. `visibility`
and `purpose` come from `Imageable` and turn a volume on or off in the usual
way.

A minimal example:

```usda
def CubeClipVolume "Cutaway"
{
    bool clipVolume:affectsSecondaryRays = false
    prepend rel collection:clipLink:excludes = </World/Room/Pillar>
    double size = 1
    double3 xformOp:translate = (0, 1.4, 3.05)
    float3 xformOp:scale = (4, 2.2, 0.6)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
}
```

## Semantics a renderer must implement

These rules are the contract. They are written so that two renderers given the
same scene produce the same image, up to sampling noise.

1. **What is clipped.** For a ray of a kind the volume affects, take an
   intersection with geometry in the volume's `clipLink` collection. If the
   world-space hit point is inside the volume, discard it. The ray goes on
   as if that surface were not there. Nothing else changes: the hit is not
   shaded, not counted as an occlusion, and not replaced by anything.
2. **Inside** means inside the closed shape in its local frame, after the
   volume's full world transform at the ray's time is applied. Points exactly
   on the boundary may be treated either way.
3. **Primary rays** are the rays leaving the camera, *including their
   continuation* past surfaces the renderer treats as not hit: cutout
   opacity, a surface removed by a clip volume, and so on. Every other ray
   is **secondary**: shadow and occlusion rays toward lights, and every
   scattered ray (diffuse, glossy, specular reflection or refraction). A
   mirror therefore shows a wall that is clipped for primary rays only, and
   that is correct.
4. **Several volumes combine as a union.** An intersection is discarded if
   *any* active volume that affects the ray's kind and links the hit geometry
   contains the hit point.
5. **Active** means visible, with a purpose the render includes, and with a
   valid shape. A negative `size` or `radius` is invalid input. A renderer
   reports it and ignores that volume; it does not clamp or fix the value.
6. **Clip volumes act on geometry, not lights.** Gprims and their instances
   are clipped. Lights, including geometry lights, are controlled by light
   linking and are never clipped.
7. **Volumetric primitives** (UsdVol volumes, particle fields) are treated
   the same way, as material that is absent inside the region. For an affected
   ray, the part of the primitive inside the volume has no density, no
   emission and no scattering.
8. **No capping.** A clipped solid becomes an open shell, and rays that pass
   through the hole see its inside. Filling the cut with a surface is not part
   of this proposal.
9. **The volume itself is never visible.** It casts no shadow, has no
   material, and is not part of any bounding box.

## Design decisions

### A typed schema, not an API schema on `UsdGeomCube`

The obvious alternative is to apply a `ClipVolumeAPI` to an ordinary `Cube` or
`Sphere` and reuse its shape. UsdPhysics does this with `CollisionAPI`, and
UsdLux does it with `MeshLightAPI`. It was rejected because of what happens in
a renderer that does not know the schema. The gprim is still a gprim, so
that renderer draws an opaque cube over the subject of the shot. The fix
would be to make the shape `invisible` or `purpose = guide`. But then a
renderer that *does* know the schema has to ignore the visibility and purpose
it otherwise respects, and the authored data would mean two opposite things.

A clip volume derived from `Xformable` is never drawn by UsdImaging. A
renderer that doesn't know the type does nothing with it, and the scene
renders as it did before the volume was added. That is the right way for it
to fail.

The cost is two small attributes (`size`, `radius`) that repeat what
`UsdGeomCube` and `UsdGeomSphere` already define. They are defined to mean
exactly the same thing, so an author's intuition carries over.

### An abstract base, so new shapes don't need new rules

All of the semantics live on `ClipVolume`, and the concrete types only add a
shape. A cylinder, a capsule, a half-space, or a closed mesh referenced by a
relationship would each be one new subclass, with no change to the contract.

### Two booleans, not a list of ray types

Production renderers name ray types differently (camera, shadow, diffuse,
glossy, specular, transmission, volume...), and no two lists match. Primary
versus secondary is the one split every ray tracer has, and it is what both
motivating use cases need. A finer split can be added later as more
attributes, without changing what these two mean.

### A collection, as light linking does it

`clipLink` is a standard `CollectionAPI` instance, and it defaults to the
whole stage, like `lightLink`. Authors get includes, excludes and membership
expressions without learning anything new. On the Hydra side, UsdImaging's
existing CollectionAPI adapter already carries the collection into the scene
index.

### Names

"Clip" is already used in USD for *value clips* (`UsdClipsAPI`) and for the
camera's clipping range and planes. `ClipVolume` and the `clipVolume:`
namespace do not collide with either. Still, review may prefer `CullVolume`,
`CutawayVolume` or `RayClipVolume`. The term is used in only a few places, so
renaming it before the proposal is submitted is cheap.

## Hydra mapping

A UsdImaging prim adapter (a `UsdImagingSceneIndexPrimAdapter`) puts each
concrete clip volume into the scene index as a prim of a single Hydra type,
**`clipVolume`**, carrying:

| Data source | Contents |
| --- | --- |
| `clipVolume` | `shape` (`cube` or `sphere`), `size`, `radius`, `affectsPrimaryRays`, `affectsSecondaryRays` |
| `xform` | the usual Xformable data source |
| `visibility`, `purpose` | the usual Imageable data sources |
| `collections/clipLink` | from UsdImaging's CollectionAPI adapter, with no extra code |

A render delegate that supports clip volumes lists `clipVolume` among its
supported sprim types, and resolves `clipLink` membership against the paths of
its geometry rprims. Using one Hydra type with a `shape` token keeps the
renderer's job simple: it gets one kind of prim with a small, closed set of
shapes, not one prim type per shape.

## Implementing it in a ray tracer

The rule is a test on each candidate hit: *is this hit point inside a volume
that applies to this ray and this geometry?* The natural place for that test is
where a ray tracer already decides whether to accept a candidate hit. That is
the any-hit shader in a ray tracing pipeline, or the candidate loop of a ray
query, and it is the same place cutout opacity is tested.

- Upload the active volumes as world-to-local transforms plus a shape. Test a
  point by transforming it into the volume's frame and comparing against the
  unit cube or sphere.
- Store each geometry instance's linking as a bitmask over the active volumes,
  computed on the CPU from `clipLink`. A ray's kind selects the volumes whose
  flag allows it.
- Only instances whose world bounds overlap an active volume that links them
  need to be traced as non-opaque. Everything else stays on the opaque fast
  path, so a scene with no clip volumes, or with volumes far away, costs
  nothing.

## Open questions

- **Granularity of ray kinds.** Is primary/secondary enough for a first
  version, or should shadow rays get their own flag from the start?
- **Inverted volumes ("keep only what is inside").** Section boxes in
  architecture need this. The union rule in semantics rule 4 does not extend
  to it cleanly, so it is left out for now and would come with its own
  combination rule.
- **Viewport display.** The adapter could publish a guide-purpose wireframe as
  a second subprim, as UsdLux light adapters do. Then Storm would show where
  the volumes are without clipping anything.
- **Instancing.** Resolving membership for instance proxies inside a native
  instance has the same limits as `HdsiLightLinkingSceneIndex`, which hdClaude
  already relies on for light linking.

## Status in this repository

| Part | State |
| --- | --- |
| Schema (`usdClipVolume`, codeless) | Drafted; loads in OpenUSD 26.03 and gives both concrete types their properties and built-in collection. |
| Example scenes | `examples/cutaway.usda`, `examples/canopy.usda`. |
| UsdImaging adapter + Hydra `clipVolume` prim | Not written. |
| hdClaude: sprim, linking masks, extend/shadow kernel tests | Not written. |

To make the schema visible to USD, add
`extensions/clipVolume/usdClipVolume` to `PXR_PLUGINPATH_NAME`.

The schema is codeless (`skipCodeGeneration = true`) while it is a prototype,
so it needs no compiled library. After editing `schema.usda`, regenerate it
with `usdGenSchema schema.usda .` from inside `usdClipVolume/`. Then put the
three plugin paths in `plugInfo.json` back to `""`, `"."` and `"."`, because
`usdGenSchema` writes placeholders for a compiled library there.
