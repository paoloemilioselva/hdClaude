"""Lights deliver the power or illuminance the draft UsdLux photometric and
radiometric APIs author.

The draft physical-lighting proposal (docs/usd-proposals.md) lets a light be
authored by what it delivers instead of the luminance it emits: an area light
by its power in lumens or watts, or by the illuminance it puts on a surface
facing it at a distance; a distant or dome light by its illuminance. Each case
here renders one light authored that way and measures what it delivers,
against the authored number and nothing hdClaude computes:

  power          The emitter seen directly: an unshaped emitter of power P and
                 area A has luminance P / (pi A). The rect is authored in
                 centimetres, so the metersPerUnit setting is what makes it
                 right; its intensity, exposure and normalize are authored too,
                 and the power overrides all three.
  radiometric    Watts through a custom illuminant: lumens are watts over the
                 illuminant's radiance per nit, integrated independently here.
  illuminance    A Lambertian floor of albedo 0.5 reads 0.5 E / pi. The spot is
                 a soft cone with focus, so the cone and focus the host
                 integrates have to be the ones the shader emits; the distant
                 light has a wide angle; a radiometric one takes the "white"
                 illuminant the API implies.
  dome           A plain dome, and a dome with a lopsided map on a Z-up stage,
                 turned so the map's pole is +Z: the map's integral over the
                 upper hemisphere is about the stage's up axis, which a Y-up
                 integral would miss by a factor of two.
  reported       A power on a dome, an illuminance with no distance, and both
                 APIs on one light are each reported by name.

Usage: python physical_brightness.py <output directory>
Runs in the OpenUSD environment and tests the plugin HDCLAUDE_PLUGIN_DIR names
-- the build tree's, under ctest -- or else what compile.py last installed.
"""

import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import cornell_box as box  # noqa: E402

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt  # noqa: E402

if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

TOLERANCE = 0.015
ALBEDO = 0.5


# --- Independent photometry --------------------------------------------------

def radiant_per_luminous(wavelengths, values):
    """W/(m^2 sr) per nit of a piecewise-linear spectrum: its radiant integral
    over its own range, over 683 times its luminous one through the film's
    colour matching functions."""
    radiant = sum(0.5 * (values[i] + values[i + 1]) * (wavelengths[i + 1] - wavelengths[i])
                  for i in range(len(wavelengths) - 1))
    luminous = 0.0
    lam = wavelengths[0] + 0.05
    while lam < wavelengths[-1]:
        luminous += box.tabulated((wavelengths, values), lam) * box.cie_xyz_bar(lam)[1] * 0.1
        lam += 0.1
    return radiant / (683.0 * luminous)


# --- Scenes ----------------------------------------------------------------

def new_stage(path, up="Y", meters=1.0, aperture=10.0, height=5.0):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z if up == "Z" else UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", meters)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
    camera.CreateHorizontalApertureAttr(aperture)
    camera.CreateVerticalApertureAttr(aperture)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.01, 1000.0))
    return stage, camera


def look_down(camera, up, height):
    """Above the origin, looking down the up axis at the floor."""
    if up == "Z":
        camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, height))
    else:
        camera.AddTranslateOp().Set(Gf.Vec3d(0.0, height, 0.0))
        camera.AddRotateXOp().Set(-90.0)


def floor(stage, up="Y"):
    """A Lambertian floor of albedo 0.5 through the origin, facing up."""
    mesh = UsdGeom.Mesh.Define(stage, "/floor")
    s = 50.0
    if up == "Z":
        points = [(-s, -s, 0), (s, -s, 0), (s, s, 0), (-s, s, 0)]
        normal = (0, 0, 1)
    else:
        points = [(-s, 0, s), (s, 0, s), (s, 0, -s), (-s, 0, -s)]
        normal = (0, 1, 0)
    mesh.CreatePointsAttr([Gf.Vec3f(*p) for p in points])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateNormalsAttr([Gf.Vec3f(*normal)] * 4)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    material = UsdShade.Material.Define(stage, "/looks/grey")
    shader = UsdShade.Shader.Define(stage, "/looks/grey/surface")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(ALBEDO))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
    shader.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(0.0)
    shader.CreateInput("ior", Sdf.ValueTypeNames.Float).Set(1.0)
    shader.CreateInput("specularColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.0))
    shader.CreateInput("useSpecularWorkflow", Sdf.ValueTypeNames.Int).Set(1)
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)


def author(prim, values, schemas):
    for schema in schemas:
        prim.AddAppliedSchema(schema)
    for name, (type_name, value) in values.items():
        prim.CreateAttribute(name, type_name).Set(value)


F = Sdf.ValueTypeNames.Float
T = Sdf.ValueTypeNames.Token
F2 = Sdf.ValueTypeNames.Float2Array

FLAT = ([400.0 + 5.0 * i for i in range(61)], [1.0] * 61)


def write_hdr(path, width, height, pixel):
    """A Radiance RGBE image, uncompressed, top row first."""
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
        f.write(f"-Y {height} +X {width}\n".encode())
        for j in range(height):
            for i in range(width):
                rgb = pixel(i, j)
                v = max(rgb)
                if v < 1e-32:
                    f.write(b"\0\0\0\0")
                    continue
                m, e = math.frexp(v)
                scale = m * 256.0 / v
                f.write(struct.pack("4B", *[min(255, int(c * scale)) for c in rgb], e + 128))


def lopsided_sky(i, j, width=64, height=32):
    """Sky over the map's upper half, a bright patch low on one side, and
    black below the horizon."""
    if j >= height // 2:
        return (0.0, 0.0, 0.0)
    if j >= height // 2 - 4 and width // 5 <= i < width // 5 + 6:
        return (24.0, 20.0, 16.0)
    return (0.9, 1.0, 1.2)


def case_power(path):
    stage, camera = new_stage(path, meters=0.01)
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 5.0))
    light = UsdLux.RectLight.Define(stage, "/light")
    light.CreateWidthAttr(40.0)
    light.CreateHeightAttr(40.0)
    light.CreateIntensityAttr(7.0)
    light.CreateExposureAttr(1.0)
    light.CreateNormalizeAttr(True)
    light.AddRotateYOp().Set(180.0)
    author(light.GetPrim(), {"photometric:power": (F, 800.0),
                             "radiometric:power": (F, 3.0)},
           ["PhotometricAreaLightAPI", "RadiometricAreaLightAPI"])
    stage.GetRootLayer().Save()
    return 800.0 / (math.pi * 0.4 * 0.4)


def case_radiometric(path):
    stage, camera = new_stage(path)
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 5.0))
    light = UsdLux.RectLight.Define(stage, "/light")
    light.CreateWidthAttr(4.0)
    light.CreateHeightAttr(4.0)
    light.AddRotateYOp().Set(180.0)
    author(light.GetPrim(), {
        "radiometric:power": (F, 2.0),
        "physical:illuminant": (T, "custom"),
        "physical:customIlluminant": (F2, Vt.Vec2fArray([Gf.Vec2f(l, v) for l, v in zip(*FLAT)])),
    }, ["RadiometricAreaLightAPI"])
    stage.GetRootLayer().Save()
    lumens = 2.0 / radiant_per_luminous(*FLAT)
    return lumens / (math.pi * 16.0)


def case_sphere(path):
    stage, camera = new_stage(path)
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 5.0))
    light = UsdLux.SphereLight.Define(stage, "/light")
    light.CreateRadiusAttr(1.0)
    author(light.GetPrim(), {"photometric:power": (F, 100.0),
                             "photometric:illuminance": (F, 5.0)},
           ["PhotometricAreaLightAPI"])
    stage.GetRootLayer().Save()
    return 100.0 / (math.pi * 4.0 * math.pi)


def case_spot(path):
    stage, camera = new_stage(path, aperture=1.0)
    look_down(camera, "Y", 5.0)
    floor(stage)
    light = UsdLux.RectLight.Define(stage, "/light")
    light.CreateWidthAttr(0.5)
    light.CreateHeightAttr(0.5)
    light.AddTranslateOp().Set(Gf.Vec3d(0.0, 2.0, 0.0))
    light.AddRotateXOp().Set(-90.0)   # emit along -Y, at the floor
    shaping = UsdLux.ShapingAPI.Apply(light.GetPrim())
    shaping.CreateShapingConeAngleAttr(30.0)
    shaping.CreateShapingConeSoftnessAttr(0.5)
    shaping.CreateShapingFocusAttr(2.0)
    author(light.GetPrim(), {"photometric:illuminance": (F, 50.0),
                             "photometric:illuminance:distance": (F, 2.0)},
           ["PhotometricAreaLightAPI"])
    stage.GetRootLayer().Save()
    return ALBEDO * 50.0 / math.pi


def case_distant(path):
    stage, camera = new_stage(path, aperture=1.0)
    look_down(camera, "Y", 5.0)
    floor(stage)
    light = UsdLux.DistantLight.Define(stage, "/light")
    light.CreateAngleAttr(10.0)
    light.AddRotateXOp().Set(-90.0)
    author(light.GetPrim(), {"photometric:illuminance": (F, 1000.0)},
           ["PhotometricDistantLightAPI"])
    stage.GetRootLayer().Save()
    return ALBEDO * 1000.0 / math.pi


def case_distant_radiometric(path):
    stage, camera = new_stage(path, aperture=1.0)
    look_down(camera, "Y", 5.0)
    floor(stage)
    light = UsdLux.DistantLight.Define(stage, "/light")
    light.AddRotateXOp().Set(-90.0)
    author(light.GetPrim(), {"radiometric:irradiance": (F, 5.0)},
           ["RadiometricDistantLightAPI"])
    stage.GetRootLayer().Save()
    white = [380.0 + 5.0 * i for i in range(81)]
    lux = 5.0 / radiant_per_luminous(white, [box.d65(l) for l in white])
    return ALBEDO * lux / math.pi


def case_dome(path):
    stage, camera = new_stage(path, aperture=1.0)
    look_down(camera, "Y", 5.0)
    floor(stage)
    dome = UsdLux.DomeLight.Define(stage, "/sky")
    dome.CreateIntensityAttr(3.0)
    dome.CreateColorAttr(Gf.Vec3f(0.6, 0.8, 1.0))
    author(dome.GetPrim(), {"photometric:illuminance": (F, 2000.0),
                            "photometric:power": (F, 10.0)},
           ["PhotometricDomeLightAPI"])
    stage.GetRootLayer().Save()
    return ALBEDO * 2000.0 / math.pi


def case_dome_map(path):
    stage, camera = new_stage(path, up="Z", aperture=1.0)
    look_down(camera, "Z", 5.0)
    floor(stage, up="Z")
    sky = os.path.join(os.path.dirname(path), "lopsided_sky.hdr")
    write_hdr(sky, 64, 32, lopsided_sky)
    dome = UsdLux.DomeLight.Define(stage, "/sky")
    dome.CreateTextureFileAttr(Sdf.AssetPath("lopsided_sky.hdr"))
    dome.AddRotateXOp().Set(90.0)   # the map's pole, its +Y, to the stage's +Z
    author(dome.GetPrim(), {"photometric:illuminance": (F, 1500.0)},
           ["PhotometricDomeLightAPI"])
    stage.GetRootLayer().Save()
    return ALBEDO * 1500.0 / math.pi


CASES = [
    # name, builder, settings, reports expected
    ("power", case_power, {"HDCLAUDE_METERS_PER_UNIT": "0.01"},
     ["radiometric API is not used"]),
    ("radiometric", case_radiometric, {"HDCLAUDE_METERS_PER_UNIT": "1"}, []),
    ("sphere", case_sphere, {"HDCLAUDE_METERS_PER_UNIT": "1"},
     ["photometric:illuminance has no effect while"]),
    ("spot", case_spot, {"HDCLAUDE_METERS_PER_UNIT": "1", "HDCLAUDE_LIGHT_GEOMETRY": "0"}, []),
    ("distant", case_distant, {}, []),
    ("distant_radiometric", case_distant_radiometric, {}, []),
    ("dome", case_dome, {}, ["photometric:power applies to area lights only"]),
    ("dome_map_zup", case_dome_map, {"HDCLAUDE_UP_AXIS": "Z"}, []),
]


def render(stage_path, output, settings):
    env = dict(os.environ)
    env["HDCLAUDE_SAMPLES_PER_PIXEL"] = "1024"
    env["HDCLAUDE_SAMPLES_PER_FRAME"] = "64"
    env["HDCLAUDE_LIGHT_GEOMETRY"] = "1"
    env["HDCLAUDE_TRACE"] = "1"
    env.update(settings)
    if os.path.exists(output):
        os.remove(output)
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1", "--imageWidth", "32",
         stage_path, output.replace(".0001.", ".####.")],
        env=env, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(output):
        raise SystemExit(f"FAIL: render of {stage_path} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def mean_luminance(image):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, "--window", image, "0.25", "0.75", "0.25", "0.75"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: could not read {image}: {result.stderr}")
    green, red, blue = (float(x) for x in result.stdout.split()[:3])
    return box.linear_srgb_to_xyz(red, green, blue)[1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    failures = []
    print(f"{'case':<20} {'measured':>10} {'expected':>10} {'ratio':>8}")
    for name, builder, settings, reports in CASES:
        stage_path = os.path.join(out, f"{name}.usda")
        expected = builder(stage_path)
        image = os.path.join(out, f"{name}.0001.exr")
        log = render(stage_path, image, settings)
        measured = mean_luminance(image)
        ratio = measured / expected
        print(f"{name:<20} {measured:10.4f} {expected:10.4f} {ratio:8.4f}")
        if abs(ratio - 1.0) > TOLERANCE:
            failures.append(f"{name}: luminance {measured:.4f}, not {expected:.4f}")
        for report in reports:
            if not any(report in line for line in log.splitlines()):
                failures.append(f"{name}: '{report}' was not reported")
        if not reports:
            unexpected = [line for line in log.splitlines()
                          if "photometric" in line and ("not used" in line or "no effect" in line
                                                        or "cannot be met" in line)]
            if unexpected:
                failures.append(f"{name}: reported unexpectedly: {unexpected[0]}")

    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("PASS: every light delivers the power or illuminance it authors")


if __name__ == "__main__":
    main()
