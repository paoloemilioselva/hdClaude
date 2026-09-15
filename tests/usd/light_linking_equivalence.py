"""UsdLux light behaviour that only exists at the USD level renders as specified.

The render suites build hdClaude scenes directly, so they check what the
renderer does with a light link and never whether one reaches it. Light linking
is the case where that gap mattered: OpenUSD resolves `collection:shadowLink`
into Hydra categories only if a scene index does it, nothing inserted one for
hdClaude, and ALab's sun and sky were blocked for as long as that went unseen by
the 3.5 km skydome their shadow links exclude. Nothing errored.

Each check is a pair of stages that the specification says must render the
same, rendered through the installed plugin, plus a stage that must *not* match
-- because a comparison that cannot fail is not evidence.

  shadow link   An enclosure, like ALab's skydome, and a native instance that
                the sun's and the dome's shadow links both exclude. Must match
                the same stage with neither the enclosure nor the instance.
                Without the links, it must not.

  normalize     UsdLux: with `normalize` on, a distant light's illuminance on a
                surface facing it is its intensity whatever its angle. A 1
                degree and a 40 degree sun must match. With it off, they must
                not.

  pole axis     `DomeLight_1` with `poleAxis = "Z"` on a Z-up stage is the
                `DomeLight` map turned 90 degrees about X. The two must match.
                An unturned `DomeLight` must not.

Usage: python light_linking_equivalence.py <output directory>
Runs in the OpenUSD environment with the hdClaude plugin installed, like
instancing_equivalence.py, and tests what compile.bat last installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "render_claude.bat")


def write_dome_map(path, width=256, height=128):
    """A latitude-longitude map with no symmetry to hide a rotation behind.

    Red runs with longitude, green with latitude, and blue marks alternate
    thirds of the way round, so every direction has its own colour and none is
    dark. Written as an uncompressed TGA because that needs nothing but the
    standard library to produce and OpenUSD's image plugins read it.
    """
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    width & 0xFF, width >> 8, height & 0xFF, height >> 8, 24, 0])
    rows = bytearray()
    for y in range(height):          # bottom row first, as TGA stores it
        for x in range(width):
            red = 40 + (215 * x) // (width - 1)
            green = 40 + (215 * y) // (height - 1)
            blue = 220 if (3 * x // width) % 2 == 0 else 40
            rows += bytes([blue, green, red])
    with open(path, "wb") as image:
        image.write(header + bytes(rows))

BOX_POINTS = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
              (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
BOX_COUNTS = [4] * 6
BOX_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]


def new_stage(path, up_axis=UsdGeom.Tokens.y):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, up_axis)
    stage.SetMetadata("metersPerUnit", 1.0)
    return stage


def box(stage, path, colour, translate, scale):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr(BOX_POINTS)
    mesh.CreateFaceVertexCountsAttr(BOX_COUNTS)
    mesh.CreateFaceVertexIndicesAttr(BOX_INDICES)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreateDisplayColorAttr([colour])
    mesh.AddTranslateOp().Set(Gf.Vec3d(*translate))
    mesh.AddScaleOp().Set(Gf.Vec3f(*scale))
    return mesh


def camera(stage, translate, rotate):
    cam = UsdGeom.Camera.Define(stage, "/camera")
    cam.AddTranslateOp().Set(Gf.Vec3d(*translate))
    cam.AddRotateXYZOp().Set(Gf.Vec3f(*rotate))
    cam.CreateFocalLengthAttr(24.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100000.0))


# --- Shadow link -------------------------------------------------------------

def build_shadow_link(path, enclosed, linked):
    stage = new_stage(path)
    # Looking down steeply enough that every pixel sees the floor: the sky
    # itself differs between the stages -- a dome in one, the inside of a black
    # box in the other -- and only what it lights is under test.
    camera(stage, (0.0, 6.0, 14.0), (-40, 0, 0))

    box(stage, "/World/Floor", (0.6, 0.6, 0.6), (0, -0.5, 0), (80, 1, 80))
    box(stage, "/World/Block", (0.7, 0.3, 0.2), (0, 1, 0), (2, 2, 2))

    # A native instance, black and out of the camera's view, standing between
    # the sun and the floor in front of the block: it can only cast a shadow.
    UsdGeom.Xform.Define(stage, "/Library")
    stage.GetPrimAtPath("/Library").SetSpecifier(Sdf.SpecifierClass)
    box(stage, "/Library/Shade/Panel", (0.0, 0.0, 0.0), (0, 0, 0), (3, 0.2, 3))

    sun = UsdLux.DistantLight.Define(stage, "/Lights/Sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-50, 0, 0))
    sun.CreateIntensityAttr(3.0)
    sun.CreateAngleAttr(2.0)
    sun.CreateNormalizeAttr(True)
    dome = UsdLux.DomeLight.Define(stage, "/Lights/Sky")
    dome.CreateIntensityAttr(0.3)

    if enclosed:
        # The skydome: a black box around everything, as ALab's is.
        box(stage, "/World/Skydome", (0.0, 0.0, 0.0), (0, 0, 0), (2000, 2000, 2000))
        shade = UsdGeom.Xform.Define(stage, "/World/Shade")
        # Up the sun's direction from the floor in front of the block, above
        # the top of the frame.
        shade.AddTranslateOp().Set(Gf.Vec3d(0, 12, 12))
        shade.GetPrim().GetReferences().AddInternalReference("/Library/Shade")
        shade.GetPrim().SetInstanceable(True)

    if linked:
        for light in (sun, dome):
            excluded = ["/World/Skydome", "/World/Shade"]
            links = UsdLux.LightAPI(light).GetShadowLinkCollectionAPI()
            links.CreateIncludeRootAttr(True)
            links.CreateExcludesRel().SetTargets([Sdf.Path(p) for p in excluded])

    stage.GetRootLayer().Save()


# --- Distant light normalize -------------------------------------------------

def build_normalize(path, angle, normalize):
    stage = new_stage(path)
    camera(stage, (0.0, 6.0, 14.0), (-40, 0, 0))
    box(stage, "/World/Floor", (0.6, 0.6, 0.6), (0, -0.5, 0), (80, 1, 80))
    sun = UsdLux.DistantLight.Define(stage, "/Lights/Sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))
    sun.CreateAngleAttr(angle)
    sun.CreateIntensityAttr(2.0)
    sun.CreateNormalizeAttr(normalize)
    stage.GetRootLayer().Save()


# --- DomeLight_1 pole axis ---------------------------------------------------

def build_pole_axis(path, kind, dome_map):
    stage = new_stage(path, UsdGeom.Tokens.z)
    # Looking along +Y, level with the Z-up horizon, so the map's pole and its
    # longitude are both in view.
    camera(stage, (0.0, 0.0, 0.0), (80, 0, 0))
    if kind == "dome_1":
        dome = UsdLux.DomeLight_1.Define(stage, "/Lights/Sky")
        dome.CreatePoleAxisAttr(UsdLux.Tokens.Z)
    else:
        dome = UsdLux.DomeLight.Define(stage, "/Lights/Sky")
        if kind == "turned":
            dome.AddRotateXOp().Set(90.0)
    dome.CreateTextureFileAttr(dome_map)
    dome.CreateTextureFormatAttr(UsdLux.Tokens.latlong)
    stage.GetRootLayer().Save()


# --- Harness -----------------------------------------------------------------

def render(stage_path, output):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "128"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    result = subprocess.run(
        ["cmd", "/c", RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", "320", stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")


def compare(baseline, candidate):
    """Whether two renders have the same *expected* image.

    Not the gallery's per-pixel gate: the stages in a pair differ in what the
    renderer estimates with -- a shadow-linked light is next-event estimation
    alone, an unlinked one a balance-heuristic combination -- so their noise is
    independent even where their expectation is identical. `--expectation`
    compares region means against the noise the images show.
    """
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, "--expectation", baseline, candidate],
                            capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    dome_map = os.path.join(out, "dome_map.tga")
    write_dome_map(dome_map)

    stages = {
        "shadow_reference": lambda p: build_shadow_link(p, enclosed=False, linked=False),
        "shadow_linked": lambda p: build_shadow_link(p, enclosed=True, linked=True),
        "shadow_unlinked": lambda p: build_shadow_link(p, enclosed=True, linked=False),
        "normalize_narrow": lambda p: build_normalize(p, 1.0, True),
        "normalize_wide": lambda p: build_normalize(p, 40.0, True),
        "unnormalized_wide": lambda p: build_normalize(p, 40.0, False),
        "pole_dome_1": lambda p: build_pole_axis(p, "dome_1", dome_map),
        "pole_turned": lambda p: build_pole_axis(p, "turned", dome_map),
        "pole_unturned": lambda p: build_pole_axis(p, "unturned", dome_map),
    }
    images = {}
    for name, build in stages.items():
        stage_path = os.path.join(out, f"{name}.usda")
        build(stage_path)
        images[name] = os.path.join(out, f"{name}.0001.exr")
        if os.path.exists(images[name]):
            os.remove(images[name])
        render(stage_path, images[name])

    checks = [
        # (label, baseline, candidate, must match)
        ("shadow links let the sun and sky past the skydome and the instance",
         "shadow_reference", "shadow_linked", True),
        ("without its shadow links the skydome blocks them",
         "shadow_reference", "shadow_unlinked", False),
        ("a normalised distant light lights the same at 1 and 40 degrees",
         "normalize_narrow", "normalize_wide", True),
        ("an unnormalised one does not",
         "normalize_narrow", "unnormalized_wide", False),
        ("DomeLight_1 poleAxis Z is the DomeLight map turned onto Z",
         "pole_turned", "pole_dome_1", True),
        ("an unturned DomeLight is not",
         "pole_turned", "pole_unturned", False),
    ]
    failures = 0
    for label, baseline, candidate, must_match in checks:
        status, report = compare(images[baseline], images[candidate])
        matched = status == 0
        ok = matched == must_match
        print(f"  {'ok  ' if ok else 'FAIL'} {label}\n{report}")
        failures += 0 if ok else 1
    print("usd lights: " + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
