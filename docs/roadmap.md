# hdClaude roadmap and phase tracker

Status: live. Last revised 2026-09-07.

This is the durable progress record. Update the tracker, the evidence column,
and the decision log **in the same commit** as the work they describe. A phase
is not complete because its API exists; it is complete when its exit gate passes
and the evidence is recorded here.

## How to resume

1. Read the decision log and the phase tracker below.
2. Inspect the worktree and recent commits. **Never assume this document is
   newer than the code.**
3. Run `compile.bat` and the full test suite before changing renderer
   behaviour, and `render_gallery.bat` after.
4. Work one reviewable phase boundary at a time.
5. Re-render and commit the full gallery after every render-affecting change.
6. Record timing, image, validation, and hardware evidence in the phase entry.

## Phase tracker

Allowed states: `Not started`, `In progress`, `Blocked`, `Complete`, `Deferred`.
A blocked phase must name what blocks it.

| # | Phase | State | Evidence |
| --- | --- | --- | --- |
| 0 | Repository, build system, environment, documentation | Complete | core-only preset builds and tests from clean; env script verified; 10 gallery stages open |
| 1 | Core library: hashing, shader cache, spectral tables, display transform | In progress | hashing, shader cache, spectral sampling and sensor; the shader cache misses on every component of its key. RGB-to-spectrum upsampling: Jakob-Hanika sigmoid for reflectance, whose range is (0, 1) for any coefficients at all so no fit can produce an albedo that creates energy, and a bounded chromaticity plus a carried magnitude for emission, which is unbounded. The round-trip gate is met -- upsample and integrate back within 1e-3 dE2000 -- at 4.7e-5 worst case over the primaries, secondaries, a grey ramp and a spread of muted reflectances, with CIEDE2000 itself checked against published pairs. The integral is adapted so a perfect reflector produces the colour space's white: the CMF fit and the illuminant table are each approximations whose product lands a fraction of a per cent off D65, which every colour but white can absorb into its fit. 3232 checks pass. Remaining: nothing known |
| 2 | Vulkan context, memory, resource rules, validation gate | In progress | context, allocator, buffers, images, generations, device-lost latch, compute pipelines and direct and indirect dispatch; core *and* synchronisation validation clean on RTX 5060 Ti. Acceleration-structure scratch addresses are aligned to `minAccelerationStructureScratchOffsetAlignment`, which is a build requirement no allocator enforces and whose violation is a lost device |
| 3 | Geometry pipeline: meshes, BLAS/TLAS, instancing, subdivision | In progress | prototypes, fingerprinted BLAS reuse, TLAS instancing, ray-query traversal verified by hit pattern on GPU. per-triangle materials from GeomSubsets; deformation through ExtComputation; uniform OpenSubdiv refinement carrying subsets through the parent-face chain; an instance transform shades identically to the same tilt baked into the prototype, which is what catches a basis used where its transpose belongs; refinement carries texture coordinates through the same weights as the positions, and structure reuse is keyed on the normals, UVs and per-triangle materials the structure owns as well as on the geometry it is built from. Authored normals are read from the primvar descriptors in whatever interpolation they were authored in -- which is what finds both spellings USD allows, `normals` and `primvars:normals`, and what stops a face-varying or uniform set being dropped for not being one per vertex; a crease can only be authored face-varying, since its two sides need different normals at the same vertex, and 195 of the OpenPBR Playground's meshes author exactly that. Subdivision tags -- creases, corners, the boundary rule and the face-varying interpolation rule -- are read from the scene delegate and set on the topology, which `GetMeshTopology` does not carry: without that call the refiner silently used OpenSubdiv's defaults, which disagree with USD's on every one of them, so a creased edge refined smooth and an open boundary floated inward instead of being pinned. Remaining: adaptive refinement, limit-surface normals, and a test that could have caught this -- the suite is deliberately USD-free, so the adapter path has no coverage at all and this gate rested entirely on a gallery baseline that had recorded the broken result as correct |
| 4 | `genglsl_pt` MaterialX target: eval, sample, pdf, combinators | In progress | all 22 overrides; `standard_surface` and `open_pbr_surface` compile; energy and probability-mass validated on GPU for 10 closures and combinators; the generated geometry setter assigns every member it declares, so object-space patterns and texture coordinates reach a material instead of reading undefined memory; screen-space derivatives are defined as zero, which is a path tracer's actual filter width, so an inherited node that asks for one generates instead of failing to compile; and the bitangent is taken from the kernel rather than rebuilt as `cross(N, T)`, because its sign belongs to the parameterisation and a mirrored UV island -- how the second half of a symmetric asset is laid out -- inverts every normal-mapped detail without it. Remaining: chi-squared agreement |
| 5 | Wavefront integrator: queues, sort, per-material dispatch | In progress | raygen, extend, environment, per-material shade, shadow, film; compaction between bounces; the counting sort by material and indirect dispatch throughout, asserted against the GPU-written counts (2336 camera hits split 1169/1167 across two materials, matching the shaded pixels exactly); the gate's scaling measurement, at 512x512 with a 32-node fractal graph -- 8.7 ms with neither quad heavy, 18.6 ms with one, 29.1 ms with both, so one heavy material costs 0.45 of what two cost rather than nearly all of it; images verified for direct lighting, per-material dispatch and cast shadows; a sampled direction is evaluated by the closure whose side it left on, so a refraction reaches the transmission branch instead of being terminated as impossible. The tangent frame handed to a material is solved from the triangle's texture coordinates rather than taken from an edge, which is the frame a tangent-space normal map is actually defined against -- asserted by a material whose albedo is its own tangent, on a quad whose `u` runs across its edges -- and per-corner normals are indexed by primitive, so a crease stays a crease. Remaining: sort granularity, once there is a scene that makes the choice matter |
| 6 | Spectral transport, lights, MIS, film | In progress | the analytic UsdLux set -- rect, disk, sphere, cylinder, distant -- sampled by NEE, with cone and focus shaping and colour temperature; a dome light supplies a constant or textured environment, in USD's own latitude-longitude orientation; exposure control; film accumulates progressively. The environment is sampled as one more emitter by next-event estimation and weighted against BSDF sampling by the balance heuristic -- the analytic lights deliberately are not, since nothing can hit them -- and a white furnace confirms the estimator: a 0.8 Lambertian under a unit sky renders 0.811 at 256 samples, where the closed form is 0.800. The environment is sampled from its own luminance -- a marginal CDF over the map's rows and a conditional CDF per row, each texel weighted by luminance times `sin(theta)` so a pole's rows do not take samples the solid angle does not justify -- and the density stays a function of direction alone, which is the only kind an MIS weight can be when the kernel that needs it has a miss and no surface. Two furnace tests stand behind it: a uniform dome, whose (u, v) density is proportional to `sin(theta)` and so catches a missing Jacobian, renders 0.818 against 0.800; and a sky lit only for `u < 0.5` -- the half-space x > 0 -- renders 0.402 against the 0.400 the cosine weight's symmetry requires, which is what pins the reconstruction's orientation rather than only its measure. Transport is spectral: a path carries four hero wavelengths drawn at ray generation, throughput and radiance are per lane, and RGB survives only where an asset authors one and where the film hands an image back. A closure's response upsamples as a reflectance and a light's colour as an emission -- the same spectrum times the illuminant its RGB was authored against, since a white light emits D65 -- and the film divides by that illuminant's luminous integral, so a white surface under a white light resolves to white. The colour matching functions, the illuminant and the chromaticity table are sampled on the host and uploaded rather than re-derived in GLSL, because a second closed form is a second thing to keep in agreement with the fit its round-trip gate is written against. A furnace confirms the estimator end to end: a 0.8 grey under a unit sky renders 0.804, 0.801, 0.804. A colour temperature is transported as a spectrum rather than resolved to a tint on the host: a light or dome with `enableColorTemperature` is referred to its own Planck spectrum *in place of* D65 -- a 2700 K light does not emit daylight through an amber filter -- scaled so the blackbody carries the same luminous power as the illuminant it replaces, which is what makes the control change hue and not brightness. The spectrum is pinned against the published Planckian locus at 2000, 3000, 4000, 6500 and 10000 K, within 0.0025 in x and y, which checks Planck's law and the colour matching integration together rather than either alone; and on the GPU a lit quad's red-to-blue ratio moves from 0.998 neutral to 10.011 at 2700 K and 0.673 at 9000 K while its luminance holds at 0.2550 in all three, which is the pair of claims -- it tints, it does not brighten -- that a peak-normalised blackbody would fail. Eight of the ten gallery scenes re-render byte-identical, since none of them enables the control. The analytic lights are emitters a scattered ray can reach: they stay out of the acceleration structure and are intersected in closed form, which is what a rect and a sphere already are, so a light occludes what is behind it and appears in a reflection. Next-event estimation and BSDF sampling therefore both reach them and are weighed by the balance heuristic. The partition is checked against a closed form rather than against a previous render: a 0.8 Lambertian under a rect emitter of radiance 3 at unit half-extents and height 2 renders 0.5716, where the point-to-rectangle configuration factor gives 0.5747 -- a total that double counting overshoots and an unmatched weight undershoots. A light seen directly reads the radiance it was given, 0.4967 against 0.50. A scene of lights and no geometry now builds a valid empty top-level structure rather than a null descriptor. Remaining: the upsampling happens at the closure boundary rather than per texel inside the MaterialX graph, so a material's own colour arithmetic is still RGB; and hero-wavelength MIS for dispersion, light-power selection, IES profiles, light filters, geometry lights |
| 7 | Hydra delegate: adapters, render pass, AOVs, settings | In progress | plugin discovered as "Claude GPU Path Tracer"; `UsdPreviewSurface` networks shade through MaterialX's own `ND_UsdPreviewSurface_surfaceshader` rather than falling back to `displayColor`, which is 137 of Intel Sponza's materials; mesh, material, light, camera, instancer and render-buffer adapters -- point-instanced and natively instanced geometry is placed by hdClaude's own `HdInstancer`, since the stock one computes nothing and `GetInstancerTransforms` returns the instancer's transform rather than its instances, and an instanced rprim is placed by *both* its own transform and its instancer's -- the first takes the mesh into the prototype, the second the prototype into the world, and using the second alone collapses every mesh of a model onto that model's origin, which is what had scattered Pixar's Kitchen Set and read as a refrigerator, a stove and a table that were simply absent; progressive render pass; all three `gallery/shader_ball_*.usda` scenes render lit and materially distinct; textures load through Hio and bind bindlessly, and the interpolated texture coordinate reaches the material -- asserted by a material whose albedo is its own UV -- including a face-varying one, read from the prim's primvar descriptors and triangulated per corner, which is how a UV seam and a textured quad are both authored. textures arrive the way round they were decoded, asserted by a four-quadrant image whose corners must land where they are named; an image node with no file reads its declared default rather than the failure placeholder; and a material reading UVs through `geompropvalue` gets them. a `<UDIM>` token expands to the first tile the resolver finds, and every component type Hio decodes is accepted, so a 16-bit mask and a set authored on tile 1003 both load; an input whose name or type the linked MaterialX does not declare is dropped with a warning rather than costing the material; and a `UsdPrimvarReader` is rewritten into the `geompropvalue` it wraps, whose `geomprop` MaterialX otherwise cannot read through the nodegraph interface. Face-varying texture coordinates survive refinement, through an OpenSubdiv face-varying channel rather than as vertex data, which is what keeps a seam a seam. A float or half source keeps its range in `R16G16B16A16_SFLOAT` rather than being quantised into eight bits: quantising an integer source loses precision and is a fair trade, while quantising a float source *clamps* it, and an HDRI's whole contribution as a light is the window and the sun above 1.0 that clamping removes. A texture's colour space comes from the material rather than from the file format: `HioImage` answers from the bytes alone and calls every 8-bit three-channel image sRGB, which is right for a colour map and wrong for the normal, roughness and metalness maps shipped in the same JPEGs, so the `colorspace` attribute MaterialX authors, the `sourceColorSpace` input `UsdUVTexture` authors, and failing both the image node's own output type are each read, and the decision is cached alongside the path. Remaining: real UDIM tile selection (the tile is `1001 + floor(u) + 10*floor(v)`, so a set spanning more than one tile is currently shaded with the first that exists), a second UV set, primvar geomprops beyond position, normal, tangent and UV, per-texture wrap and filter modes, depth and id AOVs, and the type mismatches that prune Intel Sponza's normal, roughness and metalness maps, which are *not* hdClaude's to coerce: MaterialX's specification allows a connection only between the same type (its one documented exception is `string` to `filename`), so dropping the input with a warning is the spec-correct behaviour and stays. The mismatches are upstream defects and are recorded as such in [implementation-notes.md](implementation-notes.md): hdMtlx's USD-to-MaterialX type table has no entry for `normal3f`, `vector3f` or `point3f`, so an authored `normal3f` value becomes a *string*; a connected input takes its type from the upstream output, and MaterialX's own `ND_UsdUVTexture` declares `rgb` as `color3` while its own `ND_UsdPreviewSurface_surfaceshader` declares `normal` as `vector3`, so the wiring the USD specification prescribes for a normal map is a type error by MaterialX's own rule; and that table maps USD `float4` to `vector4` where the same nodedef declares `scale` and `bias` as `color4` |
| 8 | Gallery parity with hdCodex baselines | In progress | `render_gallery.bat` renders, times, display-transforms and gates every scene, and rewrites the timing table; `hdClaudeDisplayTransform` and `hdClaudeImageDiff` are the two tools behind it, and the gate fails on RMS, worst pixel, failed-pixel count, a black image, or a non-finite sample. All ten scenes render, with baselines, timings and the machine record ([gallery.md](../gallery.md)); each names what its image still gets wrong. Parity itself is untouched -- no scene matches hdCodex yet, and Sponza in particular is far darker. Remaining: a renderer-owned scene that checks instance placement by construction -- a prototype whose meshes are deliberately offset from its root, instanced several times -- because the gate compares each render against its own committed baseline and so cannot catch a baseline that was already wrong; the Kitchen Set's was, from the day it was adopted, and only hdCodex's image of the same stage showed it |
| 9 | Interactive contract: frame identity, per-slot resources, real overlap | Not started | - |
| 10 | Temporal foundation: jitter, motion vectors, guides, history reset | Not started | - |
| 11 | Renderer-native reconstruction fallback (any Vulkan device) | Not started | - |
| 12 | NVIDIA NGX bootstrap and runtime support query | Not started | - |
| 13 | DLSS Super Resolution and DLAA | Blocked | Requires 10, 12 |
| 14 | DLSS Ray Reconstruction | Blocked | Requires 4, 10, 12, 13 |
| 15 | ~~SER in `extend`~~ | Deferred | Not possible: SER builtins do not exist on the compute stage, and the per-material sort already provides the coherence. See [implementation-notes.md](implementation-notes.md) |
| 16 | GPU displacement through the generated MaterialX program | Not started | - |
| 17 | External-memory AOV interop for Hgi hosts | Not started | - |

Phases 13 and 14 are the two hdCodex never reached. They are deliberately late:
each depends on guide buffers that are only meaningful once phases 4-10 are
correct, and feeding a reconstructor from a broken estimator is explicitly a
non-goal.

## Exit gates

Each gate is a claim that can fail. "The code exists" is never a gate.

**Phase 0.** `compile.bat` configures and builds the core-only preset from a
clean clone with no manual dependency steps. `setup_usd_env.bat` reports a clear,
actionable failure for each missing dependency rather than failing later inside a
tool. All committed documents cross-reference consistently.

**Phase 1.** Unit tests pass with no GPU and no OpenUSD present. Spectral round
trip: sRGB primaries and a set of Munsell reflectances upsample and re-integrate
to within 1e-3 dE2000. The shader cache is proven to miss on every component of
its key.

**Phase 2.** A validation-enabled run of the full suite reports zero validation
errors **and the test fails if the count is nonzero** (rule R8). Validation here
means core *and* synchronisation validation: core validation checks that each
command is legal on its own and says nothing about whether what one kernel wrote
is visible to the next, which in a wavefront integrator is most of what can be
wrong. Device-loss
simulation leaves the latch set, every subsequent entry point refuses, and the
destructor completes without issuing a wait.

**Phase 3.** Prototype BLAS reuse, in-place TLAS update, and in-place BLAS update
under stable-topology deformation are each asserted by reading back GPU state,
not a CPU shadow (rule R7). Subdivision preserves creases, corners, holes,
orientation, face-varying seams, and material subsets against a committed
baseline -- and the baseline is not enough on its own. From 2026-09-06 to
2026-09-07 this gate passed against a baseline in which the creased cube was
fully rounded, because a baseline can only prove that nothing changed. The
gate is met when the claim is also asserted by something that does not
compare the renderer against itself.

**Phase 4.** All 22 pbrlib overrides present (the set is all-or-nothing; see
[materialx-codegen.md](materialx-codegen.md) §3), and the five acceptance items
in §8 of that document: every gallery material compiles with no fallback, white
furnace within 0.5%, chi-squared agreement between sampling and the reported
density, a finite nonzero density at every sampled direction, and combinator
mixture densities that integrate to one — for every closure **and every
combinator**. The last item is the one that catches a combinator reporting the
selected child's density instead of the mixture.

**Phase 5.** Per-material dispatch is observed to scale: adding a large
procedural nodegraph to one object changes that object's shading cost and not
the frame's. Queue counters are GPU-written and never read back by the CPU
during a frame.

**Phase 6.** Furnace test at scene scale. Analytic light comparisons for each
`UsdLux` type. Spectral: a dispersion scene matches an analytic reference; a
metamer pair renders as distinct under two illuminants.

**Phase 7.** A render-pass-level test drives move -> move -> stop -> move against
a stub scene and asserts a frame reaches the AOV on every moving frame (rule R7;
this is the test whose absence hid hdCodex A2/A1/N1/N8). Failure paths assert
`IsConverged()` stays false and the revision is retried.

**Phase 8.** Every gallery scene renders, and the image gate in the gallery
script passes against a committed baseline (rule R9). Timings, GPU, driver, and
TDR values recorded (rule R11).

**Phase 9.** GPU timestamps prove frame N+1's trace overlaps frame N's readback —
measured across a *queue*, not within one command buffer, since a whole-buffer
barrier's first synchronisation scope includes everything already submitted to
the queue. This is the exact claim hdCodex could not make (finding N3); it is
achievable here only because of Rule 1.

**Phase 10.** Jitter sequence, motion vectors, and history reset are validated
against a synthetic scene with known motion. Motion vectors are correct under
instancing and deformation, or the phase does not close.

**Phase 11.** The native fallback improves a fixed-sample interactive image
measurably (SSIM against the converged reference) on a device with no DLSS.

**Phase 12.** Runtime support query returns a correct answer on a supported and
an unsupported device, and the renderer runs unchanged when DLSS is absent.

**Phase 13.** DLAA at native resolution is compared against the converged
reference by SSIM and by a temporal-stability metric. Reference mode output is
bit-identical with DLSS present and absent.

**Phase 14.** Ray Reconstruction fed only by guides produced by MaterialX
closures (never by a surface-model name). Reference mode remains unaffected.

## Decision log

Decisions are recorded when made, with the reason, and are not silently
reversed.

| Date | Decision | Reason |
| --- | --- | --- |
| 2026-09-05 | Wavefront integrator, not a megakernel | A megakernel must contain the union of all material code; with real compiled MaterialX programs that destroys occupancy frame-wide ([architecture.md](architecture.md) §2) |
| 2026-09-05 | `VK_KHR_ray_query` in compute, not `VK_KHR_ray_tracing_pipeline` | In a wavefront design the SBT buys nothing already gained by separating shading from traversal, and costs portability plus a second shader-compilation path |
| 2026-09-05 | MaterialX `genglsl_pt` derived target, not a lowered closure ABI | Full MaterialX fidelity including arbitrary pattern and procedural graphs; the documented extension mechanism (`targetdef inherit`) |
| 2026-09-05 | Four hero wavelengths, not three | `vec4` is the natural GPU register width; better hero MIS; and the interactive preview must not change estimator (hdCodex N9) |
| 2026-09-05 | Separate reference and interactive storage | Sharing one buffer caused four hdCodex findings at once ([lessons](lessons-from-hdcodex.md) C1) |
| 2026-09-05 | Single `BeginFrame(FrameDescription)` entry point | Independent setters caused five hdCodex findings ([lessons](lessons-from-hdcodex.md) C2) |
| 2026-09-05 | CPU readback is the Hydra AOV adapter; interop is a later phase | No public OpenUSD API crosses a device boundary for a `VkImage`. Decided now because phases 11 and 17 both depend on the answer (hdCodex D1, which was never decided) |
| 2026-09-05 | DLSS via NGX directly, not Streamline | A delegate does not own presentation, so Frame Generation and Reflex — Streamline's reason to exist — are out of scope |
| 2026-09-05 | Film and shadow accumulation use no atomics, by invariant | Paths map one-to-one onto pixels, so exactly one invocation writes each entry. Tracing several paths per pixel would need `VK_EXT_shader_atomic_float`; the invariant is stated in the shader so the requirement is not discovered by a race |
| 2026-09-05 | Acceleration-structure reuse is keyed on a geometry fingerprint | Not on prototype index or name: a publication that reorders or renames prototypes must reuse everything, and one that changes a vertex must rebuild only that prototype. The opacity class is part of the fingerprint because it changes the build flags |
| 2026-09-05 | Traversal is asserted by hit pattern, not hit count | A structure built at the wrong scale, or with a transposed instance transform, still produces hits. Coverage fractions and per-pixel instance identity are what distinguish those from correct traversal |
| 2026-09-05 | The density invariant includes discarded probability mass | A visible-normal sampler can produce directions below the horizon, which the closure cannot scatter into. The reported density integrates to one *minus* that mass, and asserting a bare integral of one would have driven a real bug into correct closures ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Shader and surface nodes thread `closureData` too | Replacing the surface node's calling convention means a shader *nodegraph* must pass it through, or its generated function references an undeclared identifier ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Subsurface and volume closures publish parameters; the integrator transports | A random walk and volumetric absorption happen along paths inside a medium, not at a surface. Mirrors MaterialX's own OSL target, which emits a closure and leaves transport to the renderer |
| 2026-09-05 | MaterialX 1.39.3 is the version of record | It is what OpenUSD 26.03 ships and what hdClaude links; 1.39.6 differs in headers, closure signatures, throughput semantics, and helpers ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | `ClosureData` stays byte-compatible; extra state in globals | 1.39.3 constructs it inline with a fixed argument list, so adding fields breaks every construction site hdClaude does not replace. A global is per-invocation in a compute shader, so it costs nothing |
| 2026-09-05 | The implementation declarations are generated from the stock ones | Nodedef names do not follow file names, and a wrong one fails silently by falling back to the stock implementation. Deriving them removes the class of error |
| 2026-09-05 | Screen-space derivatives are never used | Neighbouring lanes in a `shade` dispatch are unrelated paths, so `fwidth` measures nothing. Footprints come from ray differentials |
| 2026-09-05 | The validation layer is built from source, not assumed installed | Validation is a gate the GPU tests fail without, so it is a dependency rather than an optional local install. Pinned to the same SDK tag as the headers, loader, and compiler ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | Dependencies by pinned CMake `FetchContent`, never committed | A fresh clone reproduces the tree; the repository stays small and carries no third-party or proprietary code |
| 2026-09-05 | Closure sampling returns a direction only; densities are written by the evaluation closure types | MaterialX evaluates a combinator's children *before* the combinator, so a density returned by sampling belongs to that child's own direction and cannot be mixed. Moving densities to evaluation lets every combinator mix them with the weights it already mixes responses with ([materialx-codegen.md](materialx-codegen.md) 2) |
| 2026-09-05 | The `surface` node gets its own genglsl_pt implementation | `HwSurfaceNode` constructs `ClosureData` inside a rasteriser light loop and an environment lookup. A path tracer supplies its own direction and light sample, so the calling convention has to be replaced alongside the closures ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | SER is not used | Its builtins exist only on ray-tracing-pipeline stages, not compute, and the per-material sort already delivers the coherence SER recovers ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-05 | On device loss, skip the wait but still destroy objects | `vkDestroyDevice` requires its children to be destroyed first, and destruction stays valid after loss; skipping it faults. Only the unchecked wait contaminates diagnostics |
| 2026-09-06 | The environment is an emitter, and the only one MIS applies to | A scattered ray can reach the environment and cannot reach an analytic light, so the environment needs a weight against BSDF sampling and the lights would be halved by one. Its density is uniform over the sphere because the environment kernel has to recompute it from a direction with no surface in hand ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | `UsdPreviewSurface` is shaded through MaterialX, not refused | MaterialX declares and implements it as a nodegraph, so generating it is the same path as every other material and involves no extractor. Refusing it was applying the no-approximation rule to a case that is not an approximation |
| 2026-09-06 | Screen-space derivatives are defined as zero, not forbidden | The rule is unchanged -- a derivative between two unrelated paths measures nothing -- but a path tracer's filter width at a shading point genuinely is zero, so an inherited node that asks for one should evaluate rather than fail to compile. `mx_subsurface_scattering_approx` stays deleted, so the case that is a real approximation still fails loudly ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | An asset's version drift is tolerated; its shading is not | Refusing to approximate a material is about shading fidelity. An input whose name or type the linked MaterialX does not declare is a version difference in the asset, and dropping it with a warning costs one input rather than the whole scene |
| 2026-09-06 | The closure type follows the sampled direction's side | The integrator, not the closure, knows whether a sampled direction crossed the surface. Evaluating a refraction with the reflection closure asks about a direction below its horizon, and the zero density that comes back is indistinguishable from an impossible sample ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | A structure's identity is what it owns, not what the build reads | `BottomLevelStructure` serves the normal and UV buffers to the shading kernel, so two prototypes with identical geometry and different shading data are not interchangeable however identical their builds would be ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | An image node with no file reads its default, not the placeholder | The magenta placeholder means "this asset names a file that cannot be read". A node with no file is an ordinary authoring choice with a defined MaterialX answer, and rendering it as a glaring failure colour misreports the asset |
| 2026-09-06 | The generated geometry setter assigns every member, never itself | A member left unassigned is undefined, not "filled later by the kernel": the struct is a global and nothing else writes it. The self-assignment that stood for "the kernel knows this one" made every object-space pattern and every texture coordinate read undefined memory ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | Instance transforms are read through one helper, not inline | They are stored row-major 3x4 for Vulkan's instance structure, and GLSL indexes a `mat3x4` by column, so the obvious construction yields the transpose. One `hdclaude_linear` keeps that reasoning in a single place instead of at each use, where it was wrong twice |
| 2026-09-06 | Synchronisation validation is part of the validation gate | Core validation passed a whole frame whose counter reset raced the copy that read those counters. The class of defect the gate exists to catch is exactly this one, so the layer setting is enabled in the context rather than left to whoever remembers an environment variable ([implementation-notes.md](implementation-notes.md)) |
| 2026-09-06 | The prefix sum runs in one invocation, not a parallel scan | The key range is the scene's distinct compiled materials -- tens -- and a serial scan of tens of integers costs less than the workgroup barriers a parallel scan needs to be correct. It shares a kernel with the indirect-argument writes, which read the same counts |
| 2026-09-07 | The analytic lights are hittable, and intersected in closed form rather than tessellated | A light nothing can hit cannot appear in a mirror or a glass ball at any sample count, because a delta closure skips next-event estimation entirely and has no second way to find one. Closed form rather than triangles because a rect and a sphere *are* closed forms: tessellating them would add a discretisation of a shape already known exactly, plus a build, plus a question about how finely to divide a light nobody is looking at. The cost is a loop over the lights per ray, which is right for the handful a scene authors and wrong for a thousand; the density and the emission do not change when that day comes, only where the intersection comes from |
| 2026-09-07 | Subdivision tags are read from the scene delegate, not assumed | `GetMeshTopology` returns the cage and says nothing about how to refine it; the tags are a separate `GetSubdivTags` call. Skipping it does not fail loudly -- the refiner falls back to OpenSubdiv's defaults, which disagree with USD's on the boundary rule (`edgeAndCorner` versus none), on creases and corners (present versus absent), and on face-varying interpolation (`cornersPlus1` versus fully linear). One missing call read as three unrelated bugs: shells split open along closed seams, creased models rounded off, and textures moved at every UV seam |
| 2026-09-07 | The stand-in sun lights a stage that lights itself in no way at all, and is aimed by a declared up axis | A dome light is not an entry in the light table -- it supplies the environment -- so counting the table alone called a dome-lit stage lightless, and the Open Chess Set had been rendering under its own HDRI *plus* a second key light nobody authored. And elevation is measured about an up axis that a Hydra scene delegate is never told: usdImaging passes the world as authored and there is no token for it, so it is a render setting whose default is Y. Getting it wrong does not dim a scene, it points the sun sideways, which is what had been happening to the Z-up Kitchen Set |
| 2026-09-07 | Invalid or mismatched scene input is reported, never coerced | A renderer that silently patches malformed input hides the defect from the person who can fix it and makes its own image untrustworthy, because you can no longer tell which pixels came from the asset and which from the workaround. The limit of the rule is documentation: where a specification says two things are interchangeable, supporting the equivalence is correctness rather than magic -- and MaterialX's specification says the opposite here, allowing a connection only between the same type with one exception, `string` to `filename`. `tools/check_usd_materials.py` is the reporting half of the decision, checking a stage at the USD level so a finding is about the asset rather than about any renderer |
| 2026-09-07 | A colour temperature is transported as a spectrum, and replaces the illuminant rather than multiplying it | An RGB tint is a metamer of the blackbody, so it lights a surface whose reflectance varies across the spectrum -- every real one -- differently from the light it stands for, and `enableColorTemperature` is the input where transporting four lanes most obviously earns them. The two illuminants are equated on their luminous integrals rather than their peaks, because a 2700 K blackbody peaks well outside the visible range and matching peaks would make a warm light a dim one |
| 2026-09-05 | The pbrlib override set is all-or-nothing | MaterialX resolves `#include` relative to the including file, so mixing one upstream closure with one hdClaude closure emits `struct ClosureData` twice. The set is exactly the 22 pbrlib files that include `mx_closure_type.glsl`; no stdlib file does |

## Open questions

Tracked here rather than decided prematurely.

1. **Sort granularity.** Counting sort per bounce over all active paths, or a
   partial sort that only separates the few most expensive materials? Decide
   with a measurement in phase 5, not before.
2. **Shadow-ray batching depth.** One shadow queue flushed per bounce, or a
   deeper queue flushed per frame? Interacts with memory footprint at 4K.
3. **Displacement residency.** GPU displacement (phase 16) multiplies vertex
   memory by the refinement level. Whether to cache displaced positions or
   re-evaluate per BLAS build is a memory/time trade to measure.
4. **Volume rendering.** MaterialX VDFs are declared and generated; hdClaude
   currently plans homogeneous interior media only. Heterogeneous volumes
   (`UsdVol`) are not scheduled.
