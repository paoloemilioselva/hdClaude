"""Curves with normals are ribbons, oriented by their normals.

UsdGeomBasisCurves, "Tubes and Ribbons": curves with only widths are tubes of
radius width / 2, and "curves with both widths and normals are imaged as
ribbons oriented in the direction of the interpolated normal vectors". hdClaude
drew every curve as a tube, so a ribbon authored edge-on to the camera covered
its whole width when it should cover almost nothing, and a crown of palm
fronds authored as ribbons shaded as a cloud of cylinders.

What is asserted is coverage, which is what tells a ribbon from a tube without
reading any shade. Four vertical curves of width 2 stand in a row, dark grey
against a dome of radiance one:

  facing     a ribbon whose normal faces the camera: covers its width
  edge-on    the same ribbon with its normal across the view: covers almost
             nothing beside its centre line
  tube       no normals: covers its width, from any side
  varying    a cubic B-spline ribbon facing the camera, its normals authored
             with varying interpolation -- two values for its one segment,
             the count the schema gives -- so the interpolation reaches the
             renderer from Hydra and is read as authored

Each is measured 0.8 units beside its centre line, inside the half-width of 1:
dark for a surface there, the dome for none. Both ways hdClaude draws curves
are checked, implicit and swept, since a ribbon must be a ribbon in either.

Usage: python ribbon_curves.py <output directory>
Runs in the OpenUSD environment, and tests the plugin HDCLAUDE_PLUGIN_DIR
names -- the build tree's, under ctest -- or else what compile.py last
installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Usd, UsdGeom, UsdLux, Vt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

WIDTH = 320
FOCAL_LENGTH = 50.0
APERTURE = 36.0
CAMERA_Z = 22.0
# Half the width of every curve, and how far beside the centre line coverage
# is read: inside the ribbon, well clear of its edge and of its centre.
HALF_WIDTH = 1.0
PROBE = 0.8

CURVES = [
    # name, x, expected to cover the probe
    ("facing", -6.0, True),
    ("edge-on", -2.0, False),
    ("tube", 2.0, True),
    ("varying", 6.0, True),
]


def frame_fraction(x):
    """Where a point on the z = 0 plane lands across the frame, in [0, 1]."""
    half_extent = 0.5 * APERTURE * CAMERA_Z / FOCAL_LENGTH
    return 0.5 + 0.5 * x / half_extent


def make_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, CAMERA_Z))
    camera.CreateFocalLengthAttr(FOCAL_LENGTH)
    camera.CreateHorizontalApertureAttr(APERTURE)
    camera.CreateVerticalApertureAttr(24.0)

    UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(1.0)

    grey = Vt.Vec3fArray([Gf.Vec3f(0.18, 0.18, 0.18)])
    width = 2.0 * HALF_WIDTH

    def linear(name, x, normal):
        curve = UsdGeom.BasisCurves.Define(stage, "/World/" + name)
        curve.CreateTypeAttr(UsdGeom.Tokens.linear)
        curve.CreateCurveVertexCountsAttr([2])
        curve.CreatePointsAttr([Gf.Vec3f(x, -3.0, 0.0), Gf.Vec3f(x, 3.0, 0.0)])
        curve.CreateWidthsAttr([width])
        curve.SetWidthsInterpolation(UsdGeom.Tokens.constant)
        if normal is not None:
            curve.CreateNormalsAttr([normal, normal])
            curve.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
        curve.CreateDisplayColorAttr(grey)

    linear("facing", CURVES[0][1], Gf.Vec3f(0.0, 0.0, 1.0))
    linear("edgeOn", CURVES[1][1], Gf.Vec3f(1.0, 0.0, 0.0))
    linear("tube", CURVES[2][1], None)

    # One cubic B-spline segment from four collinear control points, which
    # evaluates to a straight span between y = -2 and y = 2. Varying data is
    # one value per segment end: two here.
    x = CURVES[3][1]
    cubic = UsdGeom.BasisCurves.Define(stage, "/World/varying")
    cubic.CreateTypeAttr(UsdGeom.Tokens.cubic)
    cubic.CreateBasisAttr(UsdGeom.Tokens.bspline)
    cubic.CreateCurveVertexCountsAttr([4])
    cubic.CreatePointsAttr([Gf.Vec3f(x, y, 0.0) for y in (-4.0, -2.0, 2.0, 4.0)])
    cubic.CreateWidthsAttr([width])
    cubic.SetWidthsInterpolation(UsdGeom.Tokens.constant)
    cubic.CreateNormalsAttr([Gf.Vec3f(0.0, 0.0, 1.0), Gf.Vec3f(0.0, 0.0, 1.0)])
    cubic.SetNormalsInterpolation(UsdGeom.Tokens.varying)
    cubic.CreateDisplayColorAttr(grey)

    stage.GetRootLayer().Save()


def read_exr_mean(path, u0, u1, v0, v1):
    """The mean over a fractional window, through the renderer's image tool."""
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


def render(stage_path, output, geometry):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "16"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    environment["HDCLAUDE_CURVE_GEOMETRY"] = geometry
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", str(WIDTH), stage_path,
         output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")
    # A ribbon that could not be built is reported by name; none of these
    # should be.
    for line in (result.stdout + result.stderr).splitlines():
        if "were not built as ribbons" in line or "no orientation" in line:
            print("  " + line.strip())
            raise SystemExit("FAIL: a ribbon was reported as unbuildable")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    stage_path = os.path.join(out_dir, "ribbon_curves.usda")
    make_stage(stage_path)

    half_extent = 0.5 * APERTURE * CAMERA_Z / FOCAL_LENGTH
    probe = 0.5 * PROBE / half_extent
    window = 0.004

    failures = 0
    for geometry in ("implicit", "swept"):
        image = os.path.join(out_dir, f"ribbon_curves_{geometry}.0001.exr")
        render(stage_path, image, geometry)
        background = read_exr_mean(image, 0.02, 0.98, 0.02, 0.10)
        if background < 0.5:
            raise SystemExit("FAIL: the dome itself did not render")
        for name, x, covers in CURVES:
            u = frame_fraction(x) + probe
            value = read_exr_mean(image, u - window, u + window, 0.45, 0.55)
            covered = value < 0.5 * background
            ok = covered == covers
            print(f"  {'ok  ' if ok else 'FAIL'} {geometry:8} {name:8} "
                  f"{PROBE} beside its centre reads {value:.4f} against "
                  f"{background:.4f}: {'covered' if covered else 'open'}, "
                  f"expected {'covered' if covers else 'open'}")
            failures += 0 if ok else 1

    print("usd ribbon curves: "
          + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
