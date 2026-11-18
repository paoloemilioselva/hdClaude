# Render stats

What a render cost, shown where the render is being looked at: in Houdini's
Render Stats panel and viewport, in usdview's HUD and a panel of hdClaude's
own, and in `HDCLAUDE_STATS_REPORT` for scripts. All of them read one thing,
`HdRenderDelegate::GetRenderStats()`.

## The rule: showing a figure costs the render nothing

A host polls `GetRenderStats()` on its UI thread, as often as it likes. So the
call only *reads*. It loads atomics that the render thread stored when it did
the work, copies one path string under a lock of its own, and asks the
allocator for the memory budget once. It never reads back from the device,
waits on a queue, or takes a lock the render thread holds while it works.

Every figure is recorded where the work already happens:

| Figure | Recorded by | From |
|---|---|---|
| placements, prototypes | render pass, after the snapshot | the snapshot's lists |
| nesting depth, instanced prims, largest instancer | scene store, while the snapshot walks the rprims | `HdClaudeInstancingDepth`, set at rprim sync |
| TLAS storage, placement array, scratch, chunks, build time | `RecordPublication`, after a publication | the structures' own sizes (`PathTracer::TopLevel`) |
| BLAS, vertex, index, per-placement table bytes | `RecordPublication` | one read of the allocator's breakdown (`DeviceLocalBreakdown`), by debug-name category |
| image bytes | the allocator, lock-free | `VulkanImage` allocation and release |
| samples, target, elapsed | `RecordFrameTiming`, after each frame | the render pass |

Nothing is drawn into the image. Text burned into the colour AOV would corrupt
the EXR that renders are gated on.

## Where they appear

**Houdini.** `resources/houdini/UsdRenderers.json` does three things:

- It names a page for the Render Stats panel: `qmlTemplate`, which Houdini
  resolves relative to that JSON, so the page is
  `resources/houdini/renderstats/HdClaude.qml`. The page uses Houdini's own
  components (`SimpleTile`, `BriefNumber`, `StatTable`, ...), the same ones as
  Karma's page.
- It lists the overlay lines for the viewport in `viewstats`.
- It maps those lines, and Houdini's standard names (`percentDone`,
  `totalClockTime`, `peakMemory`, `totalMemory`, `rendererStage`), to keys in
  hdClaude's dictionary through `statsdatapaths`.

The overlay lines are preformatted strings under `hud`, because a host that
formats a number formats it as a number.

The overlay's Textures line (`hud.textures`) says how many textures are on the
device and, when some did not fit in device memory, how many and the bytes
they needed. Those textures sample the magenta placeholder. The delegate also
reports the shortfall with `TF_WARN`, but Houdini does not print that in its
shell, so the overlay is where a Houdini user sees it.

**usdview.** View > HUD > GPU Stats prints a fixed set of Storm's keys and
nothing else (`Usdviewq/stageView.py`, `drawHUD`). hdClaude publishes those
keys under the meaning Storm gives them:

- `gpuMemoryUsed`: device-local bytes in use.
- `primvar`: vertex data.
- `topology`: index buffers.
- `textureMemory`: images.
- `numCompletedSamples`: samples completed.

Everything else is in **hdClaude > Render Stats**, a dockable panel from the
`hdClaudeUsdview` plugin (`resources/usdview`). It refreshes twice a second
while it is shown and stops its timer when it is hidden. usdview's plugin API
has no route to the engine, so the panel reaches it through the attributes
usdview itself uses, and says so when it cannot. `HDCLAUDE_USDVIEW_STATS=1`
opens the panel when usdview starts.

**Scripts.** `HDCLAUDE_STATS_REPORT` writes the teardown report, and
`engine.GetRenderStats()` returns the dictionary itself.

## Instancing: why these figures exist

hdClaude flattens instancing. A Hydra instancer's placements are composed
through every level of nesting (`HdClaudeInstancer::ComputeInstanceTransforms`)
into one placement per copy, and the GPU sees two levels: one BLAS per
prototype and one TLAS instance per placement. On RTX hardware that is the
fast case for traversal: two levels are what the hardware walks, and the cost
grows with the log of the placement count.

The cost is memory and update time, both linear in the flattened count:

- **`bytesPerPlacement`**: the TLAS storage, the 64-byte placement array, and
  the per-placement tables, divided by the placements. On small stages it is
  dominated by the TLAS's fixed overhead: 384 B on the Kitchen Set, 437 B on
  the chess set. The 5.98 M-placement island measured 179 B of TLAS a
  placement (the measurement is recorded where the TLAS build declines
  compaction, `src/gpu/acceleration_structure.cpp`), so about 260 B in all.
- **`instancingDepth`** and **`largestPlacementCount`** /
  **`largestPlacementPrim`**: how deep the nesting goes and which rprim it
  multiplies the most. The path is Hydra's, which for a point-instanced
  prototype carries UsdImaging's `ForInstancer<hash>` element, and for a native
  instance is under `/UsdNiPropagatedPrototypes`.
- **`tlasBuildMs`** and **`instancingMs`**: what an edit to an outer
  instancer costs. It recomputes everything under it and rebuilds the TLAS.

The alternatives were considered and set aside for now (2026-11-17):

- **OptiX multi-level instancing.** This would mean CUDA beside Vulkan:
  external memory and semaphores, an API switch every bounce, and every
  traversal feature written twice.
- **A Vulkan-only nested level.** Each outer placement becomes a procedural
  box, and a hit on the box starts a second `rayQuery` into an inner TLAS.
  This is the route to take if one is ever needed, opt-in per instancer. It
  is needed when these figures say a real stage's flattened list no longer
  fits.

## The keys

| Key | Meaning |
|---|---|
| `instances`, `prototypes`, `placementsPerPrototype` | mesh placements, prototypes (mesh and splat), and their ratio |
| `splatPlacements` | Gaussian splat placements |
| `instancedRprims`, `instancingDepth` | rprims an instancer places; the deepest chain of instancers |
| `largestPlacementCount`, `largestPlacementPrim` | the rprim with the most placements |
| `instancingMs` | time rprims spent computing placements, summed over sync threads |
| `tlasBytes`, `tlasInstanceBytes`, `tlasScratchBytes`, `tlasChunks`, `tlasBuildMs` | the top-level structures of the last publication |
| `blasBytes`, `vertexBytes`, `indexBytes`, `placementTableBytes`, `imageBytes` | device-local bytes by what holds them |
| `bytesPerPlacement` | (TLAS + placement array + tables) / placements |
| `deviceMemory` | the same bytes as one dictionary, for a breakdown table |
| `targetSamples`, `accumulationSeconds` | where the accumulation stops, and how long it has run |
| `texturesUploaded`, `texturesReused` | textures the last publication uploaded, and kept from the one before; together, the textures on the device |
| `texturesOverBudget`, `textureBytesOverBudget` | textures that did not fit in device memory and sample the placeholder, and the bytes they needed |
| `texturesCompressed`, `texturesNotCompressed` | textures the last publication block-compressed, and those compression could not take |
| `hud` | the viewport overlay's lines, as text |
| `gpuMemoryUsed`, `primvar`, `topology`, `textureMemory`, `numCompletedSamples` | Storm's names, for usdview's HUD |
| `percentDone`, `totalClockTime`, `peakMemory`, `totalMemory`, `rendererStage` | Houdini's names |

`tests/usd/render_stats.py` builds a stage with every count known by
construction and holds the stats to it: two nested point instancers, 3 × 4
placements of one cube, and a plain mesh. It also holds the host aliases to
the figures they alias, and runs the usdview panel's formatter on the result.

Not done: writing the stats into the EXR header through `husk.metadata`. Karma
does this, and husk's support for it with a third-party delegate is untested.
