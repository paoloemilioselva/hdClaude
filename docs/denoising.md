# Denoising

hdClaude denoises a reference render with Intel Open Image Denoise (OIDN). It is
off by default, and the film is never touched: the denoised image is an
estimate of the converged one, written to the colour AOV only when the
`Denoiser` setting asks for it.

## 1. What it is for, and what it is not

A path-traced image converges as one over the square root of its samples, so
halving the noise costs four times the render. A denoiser estimates the
converged image from a noisy one, guided by the albedo and normal of what each
pixel sees: texture and edges are kept, noise is removed.

DLSS is not this. Super Resolution and DLAA anti-alias and upscale a frame of a
few samples; Ray Reconstruction denoises, but temporally, for a moving
interactive view. OIDN takes one frame at any sample count and is meant for the
final one. It runs on the CPU everywhere and on NVIDIA, AMD and Intel GPUs, on
Windows and Linux, which DLSS does not -- and an NVIDIA-only requirement in the
default build is a non-goal (docs/architecture.md 9). So the two are separate:
reconstruction is an interactive render, denoising a reference one, and asking
for both denoises nothing and says why.

| Setting | Variable | Values |
|---|---|---|
| Denoiser | `HDCLAUDE_DENOISER` | `off` (default), `oidn` |
| Denoiser quality | `HDCLAUDE_DENOISER_QUALITY` | `high` (default), `balanced`, `fast` |
| Denoiser device | `HDCLAUDE_DENOISER_DEVICE` | `default` (OIDN chooses), `cpu`, `cuda`, `hip`, `sycl` |
| Denoise from sample | `HDCLAUDE_DENOISER_START` | `16` (default); `0`, the finished frame alone |
| -- | `HDCLAUDE_OIDN_DIR` | a directory holding OIDN, put in front of the others |

**When.** The finished frame is always denoised. Before it, a preview at
`Denoise from sample` samples and at each doubling after -- 16, 32, 64, ... --
so an accumulation of N samples is denoised about log2(N) times however long it
runs. Between two previews the colour AOV keeps the last denoised image rather
than going back to the noisy film. A batch render keeps only the finished
frame, so `0` saves it the previews.

The stats report counts `denoisedFrames` and `denoiseMilliseconds`, and the
first denoised frame says which OIDN, on which device, from which file.

## 2. Which OIDN is loaded

Nothing links OIDN. `src/denoise/denoiser.cpp` loads the library when a frame
is first denoised and resolves its C API by name -- 16 entry points, every one
in the 2.x API since 2.0 -- so the plugin loads on a machine without it, and a
denoiser that cannot run leaves the noisy image in place and says why. In
order:

1. **One already in the process.** Houdini ships OIDN (2.3.3 in 22.0.469) and
   Karma and the compositor load it into the same process. A second library of
   the same name resolves its own dependencies against the first one's, by
   name, and crashes in whichever half disagrees; so the host's copy is used,
   even over `HDCLAUDE_OIDN_DIR`.
2. **`HDCLAUDE_OIDN_DIR`**, to put a different OIDN in front of the renderer.
3. **The copy installed with hdClaude**, in `hdClaude/oidn` beside the plugin's
   `hdClaude/resources`. A standalone build -- usdview, the Linux rez package
   -- copies the pinned release's runtime there (`cmake/OpenImageDenoise.cmake`):
   the library, its core, every device module, and what they load -- TBB, and
   the SYCL and Level Zero runtime for Intel GPUs -- with their licences.
4. **The platform's search for the bare name.** Inside Houdini that finds
   `$HFS/bin`. A Houdini build installs no copy of its own, for the reason in 1.

The package is fetched at configure time by URL and SHA-256, like every other
dependency, and never committed. Without it the build compiles the denoiser's
stubs, and the setting answers "this build has no Open Image Denoise headers".

## 3. The guides

OIDN asks for the albedo and normal averaged over the same samples as the
colour: a guide sharper than its image puts back the aliasing the samples
averaged away. The renderer's reconstruction guides are the last sample's, for
DLSS, which wants them that way; so the film kernel also sums each sample's
guides into a buffer of their own (`denoiseGuides`, binding 40, two vec4 a
pixel), only while a denoiser is on, and they are read back only on a frame
that is denoised. Allocated the first time and kept, 32 bytes a pixel: 66 MB at
1920x1080.

- **Albedo** is the closures' diffuse and specular albedo together, clamped to
  one -- OIDN's definition, the reflectivity under uniform white light. A
  conductor reports its reflectivity; a clear dielectric its transmission tint,
  about one, which is what OIDN asks for a delta surface. A miss keeps the
  guides' clear value, one half.
- **Normal** is the closures' shading normal, where a normal map lands, in world
  space and not renormalised, as OIDN accepts. A miss has none.

Both are cleaned by OIDN before the image is denoised against them, with
`cleanAux` set -- its own prefiltering, which keeps the noise in an averaged
guide (an edge, depth of field, rough transmission) out of the result.

## 4. What it does, measured

On an RTX 5060 Ti, OIDN 2.5.1 on its CUDA device, against a 4096-sample
reference:

| Scene | Samples | rms, raw | rms, denoised | Light kept, denoised |
|---|---:|---:|---:|---:|
| Cornell box, 512 px | 16 | 0.174 | 0.0146 | 99.47% |
| Cornell box, 512 px | 64 | 0.086 | 0.0127 | 99.80% |
| Gold shader ball, 768 px | 16 | 117 | 9.99 | 89.6% |

Sixteen samples denoised on the Cornell box are nearer the reference than about
2,300 would be raw. A denoise took 290 to 450 ms in these renders, each a new
process, so that includes OIDN creating its device.

The shader ball's 10% is fireflies. Even its 4096-sample reference has 91
pixels above 100 where the image's mean is 0.45 -- caustic paths through the
gold that have not converged -- and OIDN removes isolated outliers, which is
what it is for. A denoised frame of a scene with unconverged caustics is
therefore darker than the integral it estimates, and nearer the eventual
converged picture than the film is. What it also loses there is fine detail
seen in a mirror -- the small labels the ball reflects -- because a reflection
has neither albedo nor normal of its own to anchor it, and at 16 samples it is
only noise in the colour.

`tests/usd/denoising.py` holds the Cornell box at 160 pixels to four times
nearer the reference than raw and within 2% of its light, and checks the
preview schedule and the refusal in an interactive render;
`tests/denoise_tests.cpp` checks the library alone, on the CPU and on whatever
device OIDN chooses, against an image whose answer is known: an HDR radiance of
fifty must come back as fifty.

## 5. Not done

- **The denoise blocks the render pass**, in the frame it runs in. Overlapping
  it with the next frame's trace is the frame-overlap work of phase 9.
- **No GPU sharing.** The images go to OIDN through host memory, 12 bytes a
  pixel each way; OIDN's own device then copies them to the GPU. Sharing the
  Vulkan buffers with CUDA or HIP directly would save the round trip.
- **No noisy colour AOV.** With the denoiser on, the colour AOV is the denoised
  image; the film as traced is not published beside it.
- **Delta surfaces' albedo** is the surface's own (one for clear glass, the
  reflectivity for a mirror), not the albedo of the first non-delta surface
  behind it, which OIDN names as the better choice. Doing that needs the path
  to carry the guide past specular bounces.
