"""Geometry that is not a mesh reaches the renderer.

`UsdGeomSphere`, `Cube`, `Cone`, `Cylinder` and `Capsule` arrive at a render
delegate as their own prim types. A renderer that traces triangles sees nothing
at all unless something converts them, OpenUSD ships
`HdsiImplicitSurfaceSceneIndex` to do it, and it inserts that for no renderer --
so hdClaude rendered an empty frame for any stage made of these, and said
nothing, because nothing had been dropped: the prims were never of a type it
creates. This is the same gap light linking had, found the same way, and it can
only be tested from USD: the render suites build hdClaude scenes directly and so
begin after the conversion that was missing.

What is asserted is coverage rather than shade. Each shape is a dark grey
silhouette against a dome of radiance one, at a known place in the frame, so a
patch at its centre must read far below the background and a patch beside it
must read the background. A renderer that ignores implicit surfaces renders the
dome alone: every centre patch then reads the background and every one of these
fails.

Usage: python implicit_surfaces.py <output directory>
Runs in the OpenUSD environment against what compile.bat last installed.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "render_claude.bat")
STAGE = os.path.join(HERE, "implicit_surfaces.usda")

WIDTH = 320
# The stage puts five shapes at x = -6, -3, 0, 3, 6 under a 50 mm lens on a
# 36 mm aperture at z = 22, so each lands at a fraction of the frame width that
# the camera fixes rather than that this test chooses. Written out as fractions
# because the assertion is about where a shape *is*, and a reader should be able
# to check the arithmetic against the stage.
SHAPES = [
    ("sphere", -6.0),
    ("cube", -3.0),
    ("cone", 0.0),
    ("cylinder", 3.0),
    ("capsule", 6.0),
]
FOCAL_LENGTH = 50.0
APERTURE = 36.0
CAMERA_Z = 22.0


def frame_fraction(x):
    """Where a point on the z = 0 plane lands across the frame, in [0, 1]."""
    # A pinhole: the horizontal half-extent visible at the shapes' depth.
    half_extent = 0.5 * APERTURE * CAMERA_Z / FOCAL_LENGTH
    return 0.5 + 0.5 * x / half_extent


def read_exr_mean(path, u0, u1, v0, v1):
    """The mean of the green channel over a fractional window of an EXR.

    Reads through the renderer's own image tool rather than parsing EXR here:
    `hdClaudeImageDiff --window` reports a region's mean, and a second
    implementation of half-float decoding is a second thing to be wrong.
    """
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run(
        [tool, "--window", path,
         f"{u0:.4f}", f"{u1:.4f}", f"{v0:.4f}", f"{v1:.4f}"],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: could not measure {path}: {result.stderr}")
    for token in result.stdout.split():
        try:
            return float(token)
        except ValueError:
            continue
    raise SystemExit(f"FAIL: no number in '{result.stdout.strip()}'")


def render(stage_path, output):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "64"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    result = subprocess.run(
        ["cmd", "/c", RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", str(WIDTH), stage_path,
         output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    image = os.path.join(out_dir, "implicit_surfaces.0001.exr")
    render(STAGE, image)

    # The dome alone, measured at the top of the frame where no shape reaches.
    background = read_exr_mean(image, 0.02, 0.98, 0.02, 0.10)
    print(f"  background {background:.4f}")
    if background < 0.5:
        raise SystemExit("FAIL: the dome itself did not render")

    failures = 0
    for name, x in SHAPES:
        u = frame_fraction(x)
        centre = read_exr_mean(image, u - 0.02, u + 0.02, 0.45, 0.55)
        # Far below the background, which a shape of albedo 0.18 is and the
        # background is not. The threshold is halfway between them, so it says
        # "a shape is here" rather than "the shade is this".
        ok = centre < 0.5 * background
        print(f"  {'ok  ' if ok else 'FAIL'} {name:9} at u={u:.3f} "
              f"reads {centre:.4f} against a background of {background:.4f}")
        failures += 0 if ok else 1

    print("usd implicit surfaces: "
          + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
