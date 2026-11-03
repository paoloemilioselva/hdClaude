"""Fog volumes transport light as the radiative transfer equation says.

A fog volume is a mesh bound to a material whose only terminal is a MaterialX
`volume` (docs/fog-volumes.md). Every check here has a closed form, because a
participating medium looked at through a cube against a uniform sky is one of
the few things in rendering that does:

  invisible     A volume with nothing in it renders as no volume at all: the
                boundary is not a surface.
  absorption    Absorption alone attenuates by exp(-sigma_a L), grey and per
                channel.
  falloff       A density that varies with object-space height -- the shape a
                ground fog is authored in -- attenuates each row by its own
                exp(-sigma_a(y) L): the medium is evaluated where it is.
  furnace       A medium that scatters and does not absorb, inside a uniform
                sky, is invisible: it redirects light and loses none. This is
                the one that tests everything at once -- spectral tracking, the
                phase lobes, next-event estimation from inside the medium, the
                fog's transmittance on shadow rays, and the weighting between
                a light sampled and a light found -- homogeneous and with
                height falloff, which exercises the majorant's null
                collisions.
  emission      A medium that only emits adds j L; one that also absorbs adds
                j / sigma_a (1 - exp(-sigma_a L)).

Each is a ratio against the same stage without the fog, through an
orthographic camera, so every ray in a window crosses exactly two units of
medium and the film's normalisation cancels.

Usage: python fog_volumes.py <output directory>
Runs in the OpenUSD environment and tests the plugin HDCLAUDE_PLUGIN_DIR names
-- the build tree's, under ctest -- or else what compile.py last installed.
"""

import math
import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

BOX_POINTS = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
              (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
BOX_COUNTS = [4] * 6
BOX_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]

# The cube spans [-1, 1] on every axis, so a ray along z crosses two units.
LENGTH = 2.0
# The orthographic frame spans [-1.5, 1.5]: aperture in tenths of a unit.
FRAME = 3.0


def new_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", 1.0)
    return stage


def shader(stage, path, identifier, **inputs):
    node = UsdShade.Shader.Define(stage, path)
    node.CreateIdAttr(identifier)
    for name, (type_name, value) in inputs.items():
        attribute = node.CreateInput(name, type_name)
        if isinstance(value, UsdShade.Shader):
            attribute.ConnectToSource(value.ConnectableAPI(), "out")
        else:
            attribute.Set(value)
    return node


def coefficient(stage, material, name, value, falloff):
    """A vector3 coefficient: constant, or `value * exp(-falloff (y + 0.5))`
    with y the object-space height, as a ground fog is authored."""
    if not falloff:
        return Gf.Vec3f(*value)
    base = material + "/" + name
    position = shader(stage, base + "Position", "ND_position_vector3",
                      space=(Sdf.ValueTypeNames.String, "object"))
    height = shader(stage, base + "Height", "ND_extract_vector3",
                    **{"in": (Sdf.ValueTypeNames.Float3, position),
                       "index": (Sdf.ValueTypeNames.Int, 1)})
    floor = shader(stage, base + "FromFloor", "ND_add_float",
                   in1=(Sdf.ValueTypeNames.Float, height),
                   in2=(Sdf.ValueTypeNames.Float, 0.5))
    exponent = shader(stage, base + "Exponent", "ND_multiply_float",
                      in1=(Sdf.ValueTypeNames.Float, floor),
                      in2=(Sdf.ValueTypeNames.Float, -falloff))
    profile = shader(stage, base + "Profile", "ND_exp_float",
                     **{"in": (Sdf.ValueTypeNames.Float, exponent)})
    return shader(stage, base, "ND_multiply_vector3FA",
                  in1=(Sdf.ValueTypeNames.Float3, Gf.Vec3f(*value)),
                  in2=(Sdf.ValueTypeNames.Float, profile))


def fog_material(stage, path, absorption=(0, 0, 0), scattering=(0, 0, 0),
                 emission=None, falloff=0.0):
    material = UsdShade.Material.Define(stage, path)
    absorb = coefficient(stage, path, "absorption", absorption, falloff)
    scatter = coefficient(stage, path, "scattering", scattering, falloff)
    # Two lobes mixed, a forward peak and a backward tail, which is how a fog
    # phase function is authored -- and which a furnace must not care about.
    forward = shader(stage, path + "/forward", "ND_anisotropic_vdf",
                     absorption=(Sdf.ValueTypeNames.Float3, absorb),
                     scattering=(Sdf.ValueTypeNames.Float3, scatter),
                     anisotropy=(Sdf.ValueTypeNames.Float, 0.8))
    backward = shader(stage, path + "/backward", "ND_anisotropic_vdf",
                      absorption=(Sdf.ValueTypeNames.Float3, absorb),
                      scattering=(Sdf.ValueTypeNames.Float3, scatter),
                      anisotropy=(Sdf.ValueTypeNames.Float, -0.4))
    lobes = UsdShade.Shader.Define(stage, path + "/lobes")
    lobes.CreateIdAttr("ND_mix_vdf")
    lobes.CreateInput("fg", Sdf.ValueTypeNames.Token).ConnectToSource(
        backward.ConnectableAPI(), "out")
    lobes.CreateInput("bg", Sdf.ValueTypeNames.Token).ConnectToSource(
        forward.ConnectableAPI(), "out")
    lobes.CreateInput("mix", Sdf.ValueTypeNames.Float).Set(0.25)
    lobes.CreateOutput("out", Sdf.ValueTypeNames.Token)

    volume = UsdShade.Shader.Define(stage, path + "/volume")
    volume.CreateIdAttr("ND_volume")
    volume.CreateInput("vdf", Sdf.ValueTypeNames.Token).ConnectToSource(
        lobes.ConnectableAPI(), "out")
    if emission is not None:
        glow = shader(stage, path + "/glow", "ND_uniform_edf",
                      color=(Sdf.ValueTypeNames.Color3f, Gf.Vec3f(*emission)))
        glow.CreateOutput("out", Sdf.ValueTypeNames.Token)
        volume.CreateInput("edf", Sdf.ValueTypeNames.Token).ConnectToSource(
            glow.ConnectableAPI(), "out")
    volume.CreateOutput("out", Sdf.ValueTypeNames.Token)
    material.CreateOutput("mtlx:volume", Sdf.ValueTypeNames.Token).ConnectToSource(
        volume.ConnectableAPI(), "out")
    return material


def build(path, fog=None):
    stage = new_stage(path)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
    camera.CreateHorizontalApertureAttr(FRAME * 10.0)
    camera.CreateVerticalApertureAttr(FRAME * 10.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    camera.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 10.0))

    dome = UsdLux.DomeLight.Define(stage, "/Lights/Sky")
    dome.CreateIntensityAttr(1.0)
    dome.CreateColorAttr(Gf.Vec3f(1.0, 1.0, 1.0))

    if fog is not None:
        mesh = UsdGeom.Mesh.Define(stage, "/World/Fog")
        mesh.CreatePointsAttr(BOX_POINTS)
        mesh.CreateFaceVertexCountsAttr(BOX_COUNTS)
        mesh.CreateFaceVertexIndicesAttr(BOX_INDICES)
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.AddScaleOp().Set(Gf.Vec3f(LENGTH, LENGTH, LENGTH))
        material = fog_material(stage, "/World/Looks/Fog", **fog)
        UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    stage.GetRootLayer().Save()


def render(stage_path, output):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "256"
    environment["HDCLAUDE_MAX_BOUNCES"] = "64"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1", "--imageWidth", "96",
         stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")
    if "volume" in result.stderr and ("not rendered" in result.stderr or
                                      "could not" in result.stderr):
        print(result.stderr[-2000:])
        raise SystemExit(f"FAIL: {stage_path} did not compile its volume")


def window(image, height):
    """Mean RGB of a thin band at world height `height`, across the middle."""
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    # Image rows run top to bottom; the frame spans FRAME units.
    v = 0.5 - height / FRAME
    result = subprocess.run(
        [tool, "--window", image, "0.35", "0.65", f"{v - 0.03:.4f}",
         f"{v + 0.03:.4f}"], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: could not read {image}: {result.stderr}")
    # The tool reports green first, then red, then blue.
    green, red, blue = (float(x) for x in result.stdout.split()[:3])
    return [red, green, blue]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    grey = lambda value: (value, value, value)
    stages = {
        "sky": None,
        "empty": {},
        "absorb": {"absorption": grey(0.5)},
        "absorb_colour": {"absorption": (0.1, 0.4, 0.8)},
        "falloff": {"absorption": grey(1.0), "falloff": 2.0},
        "furnace": {"scattering": grey(1.0)},
        "furnace_falloff": {"scattering": grey(3.0), "falloff": 3.0},
        "glow": {"emission": grey(0.25)},
        "glow_absorb": {"absorption": grey(0.5), "emission": grey(0.25)},
    }
    images = {}
    for name, fog in stages.items():
        stage_path = os.path.join(out, f"{name}.usda")
        build(stage_path, fog)
        images[name] = os.path.join(out, f"{name}.0001.exr")
        if os.path.exists(images[name]):
            os.remove(images[name])
        render(stage_path, images[name])

    def falloff_absorbed(sigma, falloff, height):
        # Object-space y is world y over the cube's scale.
        return math.exp(-sigma * math.exp(-falloff * (height / LENGTH + 0.5)) * LENGTH)

    e = math.exp(-0.5 * LENGTH)
    checks = [
        # (label, stage, height, expected RGB ratio to the sky, tolerance)
        ("an empty volume is invisible", "empty", 0.0, grey(1.0), 0.01),
        ("absorption attenuates by exp(-sigma L)", "absorb", 0.0, grey(e), 0.015),
        ("per channel", "absorb_colour", 0.0,
         tuple(math.exp(-s * LENGTH) for s in (0.1, 0.4, 0.8)), 0.04),
        ("a density falling with height, low", "falloff", -0.5,
         grey(falloff_absorbed(1.0, 2.0, -0.5)), 0.02),
        ("... in the middle", "falloff", 0.0,
         grey(falloff_absorbed(1.0, 2.0, 0.0)), 0.02),
        ("... and high", "falloff", 0.5,
         grey(falloff_absorbed(1.0, 2.0, 0.5)), 0.02),
        ("a scattering medium in a furnace is invisible", "furnace", 0.0,
         grey(1.0), 0.03),
        ("... low in a falloff", "furnace_falloff", -0.5, grey(1.0), 0.03),
        ("... and high", "furnace_falloff", 0.5, grey(1.0), 0.03),
        ("emission adds j L", "glow", 0.0, grey(1.0 + 0.25 * LENGTH), 0.03),
        ("emission and absorption", "glow_absorb", 0.0,
         grey(e + 0.25 / 0.5 * (1.0 - e)), 0.03),
    ]

    failures = 0
    for label, name, height, expected, tolerance in checks:
        sky = window(images["sky"], height)
        fog = window(images[name], height)
        ratio = [f / s if s > 0 else float("nan") for f, s in zip(fog, sky)]
        ok = all(abs(r - x) <= tolerance for r, x in zip(ratio, expected))
        failures += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {label}: "
              f"{' '.join(f'{r:.4f}' for r in ratio)} expected "
              f"{' '.join(f'{x:.4f}' for x in expected)} (+-{tolerance})")
    if failures:
        raise SystemExit(f"FAIL: {failures} of {len(checks)} checks")
    print(f"PASS: {len(checks)} checks")


if __name__ == "__main__":
    main()
