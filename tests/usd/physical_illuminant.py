"""Lights emit the illuminant the draft UsdLux physical-lighting proposal names.

`PhysicalLightIlluminantAPI` (docs/usd-proposals.md) gives a light an
illuminant -- white, blackbody, illuminant D or a custom table -- that its
colour is referred to. Each check here looks straight at the emitter, a rect
light filling an orthographic frame or a dome seen past nothing, so no
transport stands between the authored spectrum and the film:

  luminance      Y is the light's intensity whatever the illuminant: the
                 spectrum is scaled to D65's luminous power, as the proposal
                 defines a light's luminance.
  chromaticity   x, y are the illuminant's, against the CIE's published
                 values -- D65, D50, and the Planckian locus at 2700 K and
                 3000 K -- rather than against anything hdClaude computes. The
                 film integrates through Wyman's fit of the colour matching
                 functions, which is good to about 1e-3 in chromaticity, so the
                 bound is 2.5e-3: wide enough for the fit, a tenth of the gap
                 between any two illuminants checked.
  custom         The Cornell box's measured light, resampled to 5 nm by the
                 proposal's rule, against the same resampling done
                 independently in Python.
  dome           A plain-coloured dome takes its illuminant too: the proposal's
                 `physical:illuminant`, and UsdLux's own colour temperature,
                 which a dome without a map used to ignore.
  reported       An unusable illuminant -- daylight below 4000 K, an unknown
                 token -- is reported by name, and the light emits its colour
                 against D65.

Usage: python physical_illuminant.py <output directory>
Runs in the OpenUSD environment and tests the plugin HDCLAUDE_PLUGIN_DIR names
-- the build tree's, under ctest -- or else what compile.py last installed.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import cornell_box as box  # noqa: E402

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, Vt  # noqa: E402

if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

INTENSITY = 3.0
CHROMATICITY_TOLERANCE = 2.5e-3
LUMINANCE_TOLERANCE = 0.01

# CIE 1931 2-degree chromaticities, published: CIE 015 for D65 and D50, and
# the Planckian locus (c2 = 1.4388e-2 m K).
D65 = (0.31271, 0.32902)
D50 = (0.34567, 0.35850)
PLANCK_2700 = (0.45986, 0.41060)
PLANCK_3000 = (0.43695, 0.40404)


def new_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", 1.0)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
    camera.CreateHorizontalApertureAttr(10.0)
    camera.CreateVerticalApertureAttr(10.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 5.0))
    return stage


def illuminant(prim, kind, kelvin=None, custom=None, legacy_kelvin=None):
    if kind is not None:
        prim.AddAppliedSchema("PhysicalLightIlluminantAPI")
        prim.CreateAttribute("physical:illuminant", Sdf.ValueTypeNames.Token).Set(kind)
    if kelvin is not None:
        prim.CreateAttribute("inputs:colorTemperature", Sdf.ValueTypeNames.Float).Set(kelvin)
    if custom is not None:
        prim.CreateAttribute("physical:customIlluminant", Sdf.ValueTypeNames.Float2Array).Set(
            Vt.Vec2fArray([Gf.Vec2f(l, v) for l, v in zip(*custom)]))
    if legacy_kelvin is not None:
        prim.CreateAttribute("inputs:enableColorTemperature", Sdf.ValueTypeNames.Bool).Set(True)
        prim.CreateAttribute("inputs:colorTemperature", Sdf.ValueTypeNames.Float).Set(legacy_kelvin)


def rect_stage(path, **kwargs):
    """A rect light facing the camera and filling its frame."""
    stage = new_stage(path)
    light = UsdLux.RectLight.Define(stage, "/light")
    light.CreateWidthAttr(40.0)
    light.CreateHeightAttr(40.0)
    light.CreateIntensityAttr(INTENSITY)
    light.CreateColorAttr(Gf.Vec3f(1.0, 1.0, 1.0))
    # A RectLight emits along its -z; turned to face +z, toward the camera.
    light.AddRotateYOp().Set(180.0)
    illuminant(light.GetPrim(), **kwargs)
    stage.GetRootLayer().Save()


def dome_stage(path, **kwargs):
    """A plain-coloured dome and nothing else: the camera sees the sky."""
    stage = new_stage(path)
    dome = UsdLux.DomeLight.Define(stage, "/sky")
    dome.CreateIntensityAttr(INTENSITY)
    dome.CreateColorAttr(Gf.Vec3f(1.0, 1.0, 1.0))
    illuminant(dome.GetPrim(), **kwargs)
    stage.GetRootLayer().Save()


def render(stage_path, output):
    env = dict(os.environ)
    env["HDCLAUDE_SAMPLES_PER_PIXEL"] = "1024"
    env["HDCLAUDE_SAMPLES_PER_FRAME"] = "64"
    env["HDCLAUDE_LIGHT_GEOMETRY"] = "1"
    # A light's report reaches a terminal through the trace (light.cpp), as
    # well as GetRenderStats, which usdrecord does not print.
    env["HDCLAUDE_TRACE"] = "1"
    if os.path.exists(output):
        os.remove(output)
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1", "--imageWidth", "32",
         stage_path, output.replace(".0001.", ".####.")],
        env=env, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(output):
        raise SystemExit(f"FAIL: render of {stage_path} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def mean_rgb(image):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, "--window", image, "0.25", "0.75", "0.25", "0.75"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: could not read {image}: {result.stderr}")
    green, red, blue = (float(x) for x in result.stdout.split()[:3])
    return red, green, blue


def luminance_and_xy(rgb):
    x, y, z = box.linear_srgb_to_xyz(*rgb)
    s = x + y + z
    return y, (x / s, y / s)


def custom_xy(spectrum):
    """The chromaticity of a table through the film's colour matching functions."""
    table = box.resample_to_5nm(spectrum)
    X = Y = Z = 0.0
    lam = 360.0
    while lam <= 830.0:
        knot = 360.0 + 5.0 * int((lam - 360.0) // 5.0)
        t = (lam - knot) / 5.0
        a, b = box.cie_xyz_bar(knot), box.cie_xyz_bar(min(knot + 5.0, 830.0))
        row = [a[c] * (1 - t) + b[c] * t for c in range(3)]
        v = box.tabulated(table, lam)
        X += row[0] * v
        Y += row[1] * v
        Z += row[2] * v
        lam += 1.0
    s = X + Y + Z
    return (X / s, Y / s)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    cornell = box.LIGHT_EMISSION
    cases = [
        # name, builder, arguments, expected xy, report expected
        ("default", rect_stage, {"kind": None}, D65, None),
        ("white", rect_stage, {"kind": "white"}, D65, None),
        ("blackbody", rect_stage, {"kind": "blackbody", "kelvin": 2700.0}, PLANCK_2700, None),
        ("daylight", rect_stage, {"kind": "illuminant D", "kelvin": 5003.0}, D50, None),
        ("daylight65", rect_stage, {"kind": "illuminant D", "kelvin": 6504.0}, D65, None),
        ("legacy", rect_stage, {"kind": None, "legacy_kelvin": 2700.0}, PLANCK_2700, None),
        ("custom", rect_stage, {"kind": "custom", "custom": cornell}, custom_xy(cornell), None),
        ("cold_daylight", rect_stage, {"kind": "illuminant D", "kelvin": 3000.0}, D65,
         "outside the 4000-25000 K"),
        ("unknown", rect_stage, {"kind": "tungsten"}, D65, "is not one of white"),
        ("dome_blackbody", dome_stage, {"kind": "blackbody", "kelvin": 3000.0}, PLANCK_3000, None),
        ("dome_legacy", dome_stage, {"kind": None, "legacy_kelvin": 3000.0}, PLANCK_3000, None),
    ]

    failures = []
    print(f"{'case':<15} {'Y':>8} {'x':>8} {'y':>8} {'expected x, y':>18}")
    for name, builder, arguments, expected, report in cases:
        stage_path = os.path.join(out, f"{name}.usda")
        builder(stage_path, **arguments)
        image = os.path.join(out, f"{name}.0001.exr")
        log = render(stage_path, image)
        luminance, xy = luminance_and_xy(mean_rgb(image))
        print(f"{name:<15} {luminance:8.4f} {xy[0]:8.5f} {xy[1]:8.5f} "
              f"({expected[0]:.5f}, {expected[1]:.5f})")
        if abs(luminance / INTENSITY - 1.0) > LUMINANCE_TOLERANCE:
            failures.append(f"{name}: luminance {luminance:.4f}, not {INTENSITY}")
        if max(abs(xy[0] - expected[0]), abs(xy[1] - expected[1])) > CHROMATICITY_TOLERANCE:
            failures.append(f"{name}: chromaticity {xy[0]:.5f}, {xy[1]:.5f}, not "
                            f"{expected[0]:.5f}, {expected[1]:.5f}")
        reported = [line for line in log.splitlines() if "physical:illuminant" in line]
        if report is not None and not any(report in line for line in reported):
            failures.append(f"{name}: the unusable illuminant was not reported "
                            f"(expected '{report}')")
        if report is None and reported:
            failures.append(f"{name}: an illuminant was reported unexpectedly: {reported[0]}")

    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("PASS: every light emits its illuminant at its intensity")


if __name__ == "__main__":
    main()
