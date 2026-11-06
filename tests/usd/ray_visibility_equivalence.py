"""Per-ray-kind visibility renders as RenderMan and Karma specify (phase 20).

A gprim, or an ancestor, can hide it from some kinds of ray through constant
primvars: RenderMan's `ri:attributes:visibility:camera`, `:indirect` and
`:transmission`, and Karma's `karma:object:rendervisibility` pattern. Each
check is a pair of stages that must render the same, rendered through the
plugin, and every visibility that should change the picture is also compared
with a stage it must *not* match, because a comparison that cannot fail is not
evidence.

Where it can, the reference is a feature tested on its own elsewhere: a cull
volume around the block that affects only primary rays hides it from camera
rays, one that affects only secondary rays hides it from indirect and shadow
rays (cull_volume_equivalence.py), and the sun's shadow link excluding the
block lets the light past it (light_linking_equivalence.py). The scene is
cull_volume_equivalence.py's floor, floating block and low sun, with no dome,
so the sun is next-event estimation alone and a shadow link and a hidden
occluder estimate the same thing.

  every kind    hidden from all three kinds of ray, the block renders as the
                stage without it
  camera        hidden from camera rays, the block renders as a primary-only
                cull volume around it, by RenderMan's switch, by Karma's
                `-primary`, inherited from an ancestor Xform, and when Karma
                shows it but RenderMan hides it (a kind is hidden if either
                hides it)
  shadows only  hidden from camera and indirect rays, salta's shadow-casting
                cards: RenderMan's switches and Karma's `shadow` agree, and
                the shadow stays
  shadow        seen from above, where the block is out of frame and its
                shadow is not: hidden from shadow rays it renders as the sun's
                shadow link excluding it, by RenderMan's switch and by Karma's
                `-shadow`; hidden from camera rays the shadow stays; hidden
                from indirect and shadow rays it renders as a secondary-only
                cull volume; and a black block, which reflects nothing, hidden
                from indirect rays alone keeps its shadow and changes nothing
  indirect      over a mirror floor: hidden from indirect and shadow rays the
                block renders as a secondary-only cull volume, without its
                reflection; hidden from indirect rays alone the reflection
                goes too, and Karma's `primary shadow` is RenderMan's
                indirect 0

Usage: python ray_visibility_equivalence.py <output directory>
Runs in the OpenUSD environment, like cull_volume_equivalence.py, and tests
the plugin HDCLAUDE_PLUGIN_DIR names -- the build tree's, under ctest -- or
else what compile.py last installed.
"""

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

# Where the block floats: x and z in [-1, 1], y in [2, 4].
BLOCK_CENTRE = (0.0, 3.0, 0.0)

RI_CAMERA = "ri:attributes:visibility:camera"
RI_INDIRECT = "ri:attributes:visibility:indirect"
RI_TRANSMISSION = "ri:attributes:visibility:transmission"
KARMA = "karma:object:rendervisibility"


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


def mirror(stage, prim):
    """A smooth metal: what the camera sees in it is indirect rays alone."""
    material = UsdShade.Material.Define(stage, "/Looks/Mirror")
    shader = UsdShade.Shader.Define(stage, "/Looks/Mirror/Surface")
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.9, 0.9, 0.9))
    shader.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(1.0)
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.0)
    material.CreateSurfaceOutput().ConnectToSource(
        shader.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(prim).Bind(material)


def cull_volume(stage, primary, secondary):
    """A cube around the block and nothing else, culling the kinds named:
    x and z in [-1.5, 1.5], y in [1.5, 4.5], clear of the floor, since a hit
    on a volume's boundary may go either way."""
    prim = stage.DefinePrim("/Volumes/Cube", "CubeCullVolume")
    if not prim.IsA(Usd.SchemaRegistry.GetTypeFromSchemaTypeName("CullVolume")):
        raise SystemExit("FAIL: CubeCullVolume is not a CullVolume; is the "
                         "usdCullVolume schema plugin installed?")
    xformable = UsdGeom.Xformable(prim)
    xformable.AddTranslateOp().Set(Gf.Vec3d(*BLOCK_CENTRE))
    xformable.AddScaleOp().Set(Gf.Vec3f(3, 3, 3))
    prim.GetAttribute("cullVolume:affectsPrimaryRays").Set(primary)
    prim.GetAttribute("cullVolume:affectsSecondaryRays").Set(secondary)
    prim.GetAttribute("size").Set(1.0)


def constant(prim, name, value):
    if isinstance(value, str):
        kind = Sdf.ValueTypeNames.String
    else:
        kind = Sdf.ValueTypeNames.Int
    primvar = UsdGeom.PrimvarsAPI(prim).CreatePrimvar(
        name, kind, UsdGeom.Tokens.constant)
    primvar.Set(value)


def build(path, view="oblique", floor="diffuse", block=True, on_block=None,
          on_parent=None, cull=None, shadow_linked=False,
          colour=(0.7, 0.3, 0.2)):
    """`on_block` and `on_parent` map primvar names to values authored on the
    block or on the Xform above it; `cull` is (primary, secondary)."""
    stage = new_stage(path)
    camera(stage, view)
    ground = box(stage, "/World/Floor", (0.6, 0.6, 0.6), (0, -0.5, 0), (80, 1, 80))
    if floor == "mirror":
        mirror(stage, ground.GetPrim())
    group = UsdGeom.Xform.Define(stage, "/World/Group")
    if block:
        mesh = box(stage, "/World/Group/Block", colour, BLOCK_CENTRE, (2, 2, 2))
        for name, value in (on_block or {}).items():
            constant(mesh.GetPrim(), name, value)
    for name, value in (on_parent or {}).items():
        constant(group.GetPrim(), name, value)

    sun = UsdLux.DistantLight.Define(stage, "/Lights/Sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-35, 0, 0))
    sun.CreateIntensityAttr(3.0)
    sun.CreateAngleAttr(1.0)
    sun.CreateNormalizeAttr(True)
    if shadow_linked:
        links = UsdLux.LightAPI(sun).GetShadowLinkCollectionAPI()
        links.CreateIncludeRootAttr(True)
        links.CreateExcludesRel().SetTargets([Sdf.Path("/World/Group/Block")])

    if cull is not None:
        cull_volume(stage, *cull)
    stage.GetRootLayer().Save()


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
    # Every pattern here is one hdClaude honours as written.
    if "rendervisibility" in result.stderr or "not an int" in result.stderr:
        print(result.stderr[-2000:])
        raise SystemExit(f"FAIL: {stage_path} reported its visibility primvars")


def compare(baseline, candidate):
    """Whether two renders have the same expected image, within their noise:
    region means against the noise the images show, as the cull volume and
    light tests do."""
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, "--expectation", baseline, candidate],
                            capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    hide_all = {RI_CAMERA: 0, RI_INDIRECT: 0, RI_TRANSMISSION: 0}
    black = (0.0, 0.0, 0.0)
    stages = {
        # Oblique, over a diffuse floor.
        "reference": lambda p: build(p),
        "no_block": lambda p: build(p, block=False),
        "hidden_everywhere": lambda p: build(p, on_block=hide_all),
        "ri_shadows_only": lambda p: build(
            p, on_block={RI_CAMERA: 0, RI_INDIRECT: 0}),
        "karma_shadows_only": lambda p: build(p, on_block={KARMA: "shadow"}),
        "primary_cull": lambda p: build(p, cull=(True, False)),
        "ri_camera": lambda p: build(p, on_block={RI_CAMERA: 0}),
        "karma_no_primary": lambda p: build(p, on_block={KARMA: "-primary"}),
        "ri_camera_inherited": lambda p: build(p, on_parent={RI_CAMERA: 0}),
        "ri_hides_karma_shows": lambda p: build(
            p, on_block={RI_CAMERA: 0, KARMA: "*"}),
        # From above: the shadow without the block.
        "top_reference": lambda p: build(p, view="top"),
        "top_shadow_linked": lambda p: build(p, view="top", shadow_linked=True),
        "top_ri_transmission": lambda p: build(
            p, view="top", on_block={RI_TRANSMISSION: 0}),
        "top_karma_no_shadow": lambda p: build(
            p, view="top", on_block={KARMA: "-shadow"}),
        "top_ri_camera": lambda p: build(p, view="top", on_block={RI_CAMERA: 0}),
        "top_secondary_cull": lambda p: build(p, view="top", cull=(False, True)),
        "top_ri_secondary": lambda p: build(
            p, view="top", on_block={RI_INDIRECT: 0, RI_TRANSMISSION: 0}),
        # A black block reflects nothing, so hiding it from indirect rays
        # alone must change nothing that is seen, and its shadow stays.
        "top_black": lambda p: build(p, view="top", colour=black),
        "top_black_ri_indirect": lambda p: build(
            p, view="top", colour=black, on_block={RI_INDIRECT: 0}),
        # Oblique, over a mirror: the reflection is indirect rays.
        "mirror_reference": lambda p: build(p, floor="mirror"),
        "mirror_secondary_cull": lambda p: build(
            p, floor="mirror", cull=(False, True)),
        "mirror_ri_secondary": lambda p: build(
            p, floor="mirror", on_block={RI_INDIRECT: 0, RI_TRANSMISSION: 0}),
        "mirror_ri_indirect": lambda p: build(
            p, floor="mirror", on_block={RI_INDIRECT: 0}),
        "mirror_karma_no_indirect": lambda p: build(
            p, floor="mirror", on_block={KARMA: "primary shadow"}),
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
        ("hidden from every kind of ray is the stage without the block",
         "no_block", "hidden_everywhere", True),
        ("the block is in the reference",
         "no_block", "reference", False),
        ("RenderMan camera 0 is a primary-only cull volume",
         "primary_cull", "ri_camera", True),
        ("and changes the picture",
         "reference", "ri_camera", False),
        ("Karma -primary is the same",
         "primary_cull", "karma_no_primary", True),
        ("camera 0 on the parent Xform is inherited",
         "primary_cull", "ri_camera_inherited", True),
        ("a kind is hidden if either convention hides it",
         "primary_cull", "ri_hides_karma_shows", True),
        ("Karma shadow is RenderMan camera 0, indirect 0",
         "ri_shadows_only", "karma_shadows_only", True),
        ("a block that only casts shadows still casts one",
         "no_block", "ri_shadows_only", False),
        ("RenderMan transmission 0 is the sun's shadow link excluding it",
         "top_shadow_linked", "top_ri_transmission", True),
        ("Karma -shadow is the same",
         "top_shadow_linked", "top_karma_no_shadow", True),
        ("and the shadow is in the reference",
         "top_reference", "top_ri_transmission", False),
        ("hidden from the camera, the block keeps its shadow",
         "top_reference", "top_ri_camera", True),
        ("indirect 0, transmission 0 is a secondary-only cull volume",
         "top_secondary_cull", "top_ri_secondary", True),
        ("hidden from indirect rays alone, a black block keeps its shadow",
         "top_black", "top_black_ri_indirect", True),
        ("over a mirror, indirect 0, transmission 0 is a secondary-only "
         "cull volume",
         "mirror_secondary_cull", "mirror_ri_secondary", True),
        ("and removes the reflection",
         "mirror_reference", "mirror_ri_secondary", False),
        ("RenderMan indirect 0 alone removes the reflection",
         "mirror_reference", "mirror_ri_indirect", False),
        ("Karma 'primary shadow' is RenderMan indirect 0",
         "mirror_ri_indirect", "mirror_karma_no_indirect", True),
    ]
    failures = 0
    for label, baseline, candidate, must_match in checks:
        status, report = compare(images[baseline], images[candidate])
        matched = status == 0
        ok = matched == must_match
        print(f"  {'ok  ' if ok else 'FAIL'} {label}\n{report}")
        failures += 0 if ok else 1
    print("usd ray visibility: " +
          ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
