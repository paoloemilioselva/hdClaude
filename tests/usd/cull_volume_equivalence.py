"""Cull volumes render as extensions/cullVolume/README.md specifies.

Each check is a pair of stages that the specification says must render the
same, rendered through the plugin -- the usdCullVolume schema, its
UsdImaging adapter and hdClaude together -- plus pairs that must *not* match,
because a comparison that cannot fail is not evidence.

The scene is a floor, a block floating above it, a low sun and a dome. The
block floats so that a volume can enclose all of it without touching the
floor: a hit exactly on a volume's boundary may go either way (semantics
rule 2), and a test must not depend on which.

  culled        A cube, a sphere or a plane that encloses the block and
                affects every ray must render as the stage without the block.
  kept          An inverted cube around everything, a cube around nothing,
                and a cube whose cullLink includes nothing must all render as
                the stage with no volume at all. The last one traces every
                placement as an exception (docs/cull-volumes.md 2.1).
  mixed         A cube around the block and a post, with the block excluded
                from its collection, must render as the stage without the
                post: regular and exception placements in one ray.
  union         Two overlapping inverted cubes that together cover the scene
                must keep all of it (semantics rule 4).
  ray kinds     Seen from above, where the block is out of frame but its
                shadow is not: culling primary rays only must change nothing,
                and culling secondary rays only must remove the shadow.

Usage: python cull_volume_equivalence.py <output directory>
Runs in the OpenUSD environment, like light_linking_equivalence.py, and
tests the plugin HDCLAUDE_PLUGIN_DIR names -- the build tree's, under
ctest -- or else what compile.py last installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux

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

# Where the block floats: x and z in [-1, 1], y in [2, 4].
BLOCK_CENTRE = (0.0, 3.0, 0.0)


def new_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
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


def camera(stage, view):
    cam = UsdGeom.Camera.Define(stage, "/camera")
    if view == "oblique":
        # The block, the floor around it and the block's shadow.
        cam.AddTranslateOp().Set(Gf.Vec3d(0.0, 5.0, 14.0))
        cam.AddRotateXYZOp().Set(Gf.Vec3f(-15, 0, 0))
    else:
        # Straight down on the floor behind the block, where its shadow
        # falls; the block itself is outside the frustum.
        cam.AddTranslateOp().Set(Gf.Vec3d(0.0, 12.0, -9.0))
        cam.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))
    cam.CreateFocalLengthAttr(24.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100000.0))


def cull_volume(stage, path, shape, translate=(0, 0, 0), scale=(1, 1, 1),
                rotate_x=0.0, invert=False, primary=True, secondary=True,
                size=1.0, radius=1.0, axis="Y"):
    """A cull volume, authored through the schema's own attribute names."""
    prim = stage.DefinePrim(path, shape)
    if not prim.IsA(Usd.SchemaRegistry.GetTypeFromSchemaTypeName("CullVolume")):
        raise SystemExit(f"FAIL: {shape} is not a CullVolume; is the "
                         "usdCullVolume schema plugin installed?")
    xformable = UsdGeom.Xformable(prim)
    xformable.AddTranslateOp().Set(Gf.Vec3d(*translate))
    if rotate_x:
        xformable.AddRotateXOp().Set(rotate_x)
    xformable.AddScaleOp().Set(Gf.Vec3f(*scale))
    prim.GetAttribute("cullVolume:invert").Set(invert)
    prim.GetAttribute("cullVolume:affectsPrimaryRays").Set(primary)
    prim.GetAttribute("cullVolume:affectsSecondaryRays").Set(secondary)
    if shape == "CubeCullVolume":
        prim.GetAttribute("size").Set(size)
    elif shape == "SphereCullVolume":
        prim.GetAttribute("radius").Set(radius)
    elif shape == "PlaneCullVolume":
        prim.GetAttribute("axis").Set(axis)
    return prim


def build(path, view="oblique", block=True, post=False, volumes=()):
    stage = new_stage(path)
    camera(stage, view)
    box(stage, "/World/Floor", (0.6, 0.6, 0.6), (0, -0.5, 0), (80, 1, 80))
    if block:
        box(stage, "/World/Block", (0.7, 0.3, 0.2), BLOCK_CENTRE, (2, 2, 2))
    if post:
        box(stage, "/World/Post", (0.2, 0.4, 0.8), (1.75, 3.0, 0.0), (0.5, 1, 0.5))

    sun = UsdLux.DistantLight.Define(stage, "/Lights/Sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-35, 0, 0))
    sun.CreateIntensityAttr(3.0)
    sun.CreateAngleAttr(1.0)
    sun.CreateNormalizeAttr(True)
    dome = UsdLux.DomeLight.Define(stage, "/Lights/Sky")
    dome.CreateIntensityAttr(0.3)

    for build_volume in volumes:
        build_volume(stage)
    stage.GetRootLayer().Save()


# --- The volumes ---------------------------------------------------------------

def around_block_cube(stage, **kwargs):
    # x and z in [-1.5, 1.5], y in [1.5, 4.5]: the block and nothing else.
    cull_volume(stage, "/Volumes/Cube", "CubeCullVolume", translate=BLOCK_CENTRE,
                scale=(3, 3, 3), **kwargs)


def around_block_sphere(stage):
    # Radius 1.9 encloses the block's corners at 1.73 and stops at y = 1.1.
    cull_volume(stage, "/Volumes/Sphere", "SphereCullVolume",
                translate=BLOCK_CENTRE, radius=1.9)


def above_floor_plane(stage):
    # Turned over so its axis points down: everything above y = 1 is inside.
    cull_volume(stage, "/Volumes/Plane", "PlaneCullVolume", translate=(0, 1, 0),
                rotate_x=180.0, axis="Y")


def around_everything_inverted(stage):
    cull_volume(stage, "/Volumes/World", "CubeCullVolume", size=400.0,
                invert=True)


def around_nothing(stage):
    cull_volume(stage, "/Volumes/Elsewhere", "CubeCullVolume",
                translate=(200, 200, 200))


def linking_nothing(stage):
    around_block_cube(stage)
    links = Usd.CollectionAPI(stage.GetPrimAtPath("/Volumes/Cube"), "cullLink")
    links.CreateIncludeRootAttr(False)


def around_block_and_post_excluding_block(stage):
    cull_volume(stage, "/Volumes/Cube", "CubeCullVolume", translate=BLOCK_CENTRE,
                scale=(5, 3, 3))
    links = Usd.CollectionAPI(stage.GetPrimAtPath("/Volumes/Cube"), "cullLink")
    links.CreateExcludesRel().SetTargets([Sdf.Path("/World/Block")])


def two_inverted_halves(stage):
    # Overlapping in x, together covering everything. Each alone would cull
    # half the scene; their union keeps all of it.
    cull_volume(stage, "/Volumes/West", "CubeCullVolume", translate=(-50, 0, 0),
                scale=(101, 100, 100), invert=True)
    cull_volume(stage, "/Volumes/East", "CubeCullVolume", translate=(50, 0, 0),
                scale=(101, 100, 100), invert=True)


# --- Harness -------------------------------------------------------------------

def render(stage_path, output):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "128"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", "320", stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")
    if "cull volume" in result.stderr and "ignored" in result.stderr:
        print(result.stderr[-2000:])
        raise SystemExit(f"FAIL: {stage_path} reported a cull volume as invalid")


def compare(baseline, candidate):
    """Whether two renders have the same expected image, within their noise.

    Region means against the noise the images show, as the light tests do: a
    ray traced in stretches meets the same surfaces, but nothing here promises
    the same samples.
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

    primary_only = lambda s: around_block_cube(s, secondary=False)
    secondary_only = lambda s: around_block_cube(s, primary=False)
    stages = {
        "reference": lambda p: build(p),
        "no_block": lambda p: build(p, block=False),
        "cube": lambda p: build(p, volumes=[around_block_cube]),
        "sphere": lambda p: build(p, volumes=[around_block_sphere]),
        "plane": lambda p: build(p, volumes=[above_floor_plane]),
        "inverted_everything": lambda p: build(p, volumes=[around_everything_inverted]),
        "around_nothing": lambda p: build(p, volumes=[around_nothing]),
        "linking_nothing": lambda p: build(p, volumes=[linking_nothing]),
        "post_reference": lambda p: build(p, post=False),
        "post_mixed": lambda p: build(
            p, post=True, volumes=[around_block_and_post_excluding_block]),
        "inverted_halves": lambda p: build(p, volumes=[two_inverted_halves]),
        "primary_only": lambda p: build(p, volumes=[primary_only]),
        "top_reference": lambda p: build(p, view="top"),
        "top_no_block": lambda p: build(p, view="top", block=False),
        "top_primary_only": lambda p: build(p, view="top", volumes=[primary_only]),
        "top_secondary_only": lambda p: build(p, view="top", volumes=[secondary_only]),
    }
    images = {}
    for name, build_stage in stages.items():
        stage_path = os.path.join(out, f"{name}.usda")
        build_stage(stage_path)
        images[name] = os.path.join(out, f"{name}.0001.exr")
        if os.path.exists(images[name]):
            os.remove(images[name])
        render(stage_path, images[name])

    checks = [
        # (label, baseline, candidate, must match)
        ("the block changes the image at all", "reference", "no_block", False),
        ("a cube around the block removes it for every ray", "no_block", "cube", True),
        ("so does a sphere", "no_block", "sphere", True),
        ("so does a plane", "no_block", "plane", True),
        ("an inverted cube around everything keeps everything",
         "reference", "inverted_everything", True),
        ("a cube around nothing changes nothing", "reference", "around_nothing", True),
        ("a cube whose cullLink includes nothing changes nothing",
         "reference", "linking_nothing", True),
        ("an excluded block survives while the post beside it is culled",
         "post_reference", "post_mixed", True),
        ("two inverted volumes keep the union of their insides",
         "reference", "inverted_halves", True),
        ("culling primary rays shows what was behind the block",
         "reference", "primary_only", False),
        ("culling primary rays alone leaves the shadow",
         "top_reference", "top_primary_only", True),
        ("culling secondary rays alone removes the shadow",
         "top_no_block", "top_secondary_only", True),
        ("which the block otherwise casts",
         "top_reference", "top_secondary_only", False),
    ]
    failures = 0
    for label, baseline, candidate, must_match in checks:
        status, report = compare(images[baseline], images[candidate])
        matched = status == 0
        ok = matched == must_match
        print(f"  {'ok  ' if ok else 'FAIL'} {label}\n{report}")
        failures += 0 if ok else 1
    print("usd cull volumes: " + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
