"""Instanced geometry renders exactly where USD says it is.

The gallery gate compares each render against its own committed baseline, so it
cannot catch a baseline that was wrong the day it was committed -- and the
Kitchen Set's was: every mesh inside a native instance was placed by the
instance's transform alone, collapsing each model onto its root, and the image
looked like missing furniture rather than a defect
(docs/implementation-notes.md, 2026-09-06). The render suites build scenes
directly and never reach the Hydra adapters at all.

This is a check by construction instead of by baseline. One model of several
meshes, each offset, turned and scaled inside it, is placed three ways that
exercise every transform hdClaude composes: native instancing, a PointInstancer,
and a PointInstancer inside a native instance. A second stage holds the same
geometry with no instancing at all, every point taken to world space by USD's
own API -- UsdGeomXformCache for the xform stack and
UsdGeomPointInstancer.ComputeInstanceTransformsAtTime for the instancer -- so
nothing about it depends on hdClaude's composition. Both are rendered by
hdClaude and must agree.

And a third stage is built the wrong way on purpose, placing each mesh by its
instance alone as the Kitchen Set defect did. It must *disagree*, which is what
shows the comparison is able to see the defect it exists for.

Usage: python instancing_equivalence.py <output directory>
Runs in the OpenUSD environment, with the hdClaude plugin installed and
render_claude.bat reachable, and hdClaudeImageDiff on PATH or named by
HDCLAUDE_IMAGE_DIFF. It renders through the *installed* plugin, so it tests
what compile.bat last installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, Vt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "render_claude.bat")

# One box, unit size, centred on its own origin.
BOX_POINTS = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
              (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
BOX_COUNTS = [4] * 6
BOX_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]

# The model's parts: name, colour, and a placement inside the model that is
# never the identity, because the identity is exactly what hides the defect.
PARTS = [
    ("seat", (0.8, 0.2, 0.2), Gf.Vec3d(0.0, 0.5, 0.0), Gf.Vec3d(0, 0, 0), Gf.Vec3d(1.2, 0.2, 1.2)),
    ("back", (0.2, 0.7, 0.2), Gf.Vec3d(0.0, 1.2, -0.55), Gf.Vec3d(12, 0, 0), Gf.Vec3d(1.2, 1.2, 0.15)),
    ("leg", (0.2, 0.3, 0.9), Gf.Vec3d(0.45, 0.0, 0.45), Gf.Vec3d(0, 30, 0), Gf.Vec3d(0.15, 0.9, 0.15)),
    ("lamp", (0.9, 0.8, 0.2), Gf.Vec3d(-0.5, 1.9, 0.3), Gf.Vec3d(0, 45, 20), Gf.Vec3d(0.3, 0.3, 0.3)),
]


def define_box(stage, path, colour):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr(BOX_POINTS)
    mesh.CreateFaceVertexCountsAttr(BOX_COUNTS)
    mesh.CreateFaceVertexIndicesAttr(BOX_INDICES)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    mesh.CreateDisplayColorAttr([colour])
    return mesh


def place(xformable, translate, rotate, scale):
    xformable.AddTranslateOp().Set(translate)
    xformable.AddRotateXYZOp().Set(Gf.Vec3f(*rotate))
    xformable.AddScaleOp().Set(Gf.Vec3f(*scale))


def define_model(stage, root):
    model = UsdGeom.Xform.Define(stage, root)
    # The model's own root is offset as well, so a root transform dropped or
    # doubled moves everything.
    place(model, Gf.Vec3d(0.1, 0.0, -0.2), (0, 5, 0), (1, 1, 1))
    for name, colour, translate, rotate, scale in PARTS:
        part = define_box(stage, f"{root}/{name}", colour)
        place(part, translate, rotate, scale)
    return model


def camera(stage):
    cam = UsdGeom.Camera.Define(stage, "/camera")
    cam.AddTranslateOp().Set(Gf.Vec3d(4.0, 5.5, 16.0))
    cam.AddRotateXYZOp().Set(Gf.Vec3f(-18, 12, 0))
    cam.CreateFocalLengthAttr(30.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))


def build_instanced(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", 1.0)
    camera(stage)

    # The model, defined once under a class prim, which USD never renders:
    # abstract prims exist to be referenced.
    stage.CreateClassPrim("/Library")
    define_model(stage, "/Library/Chair")

    # Native instancing: instanceable references, each placed differently.
    for i, (translate, rotate, scale) in enumerate([
            (Gf.Vec3d(-5, 0, 0), (0, 0, 0), (1, 1, 1)),
            (Gf.Vec3d(-2, 0, 1), (0, 70, 0), (1.3, 1.3, 1.3)),
            (Gf.Vec3d(1, 0, -1), (0, -40, 10), (0.8, 1.1, 0.9))]):
        prim = UsdGeom.Xform.Define(stage, f"/World/Native{i}")
        place(prim, translate, rotate, scale)
        prim.GetPrim().GetReferences().AddInternalReference("/Library/Chair")
        prim.GetPrim().SetInstanceable(True)

    # A PointInstancer whose prototype is the same model.
    instancer = UsdGeom.PointInstancer.Define(stage, "/World/Instancer")
    place(instancer, Gf.Vec3d(3, 0, 2), (0, 15, 0), (1, 1, 1))
    proto = define_model(stage, "/World/Instancer/Prototypes/Chair")
    instancer.CreatePrototypesRel().SetTargets([proto.GetPath()])
    instancer.CreateProtoIndicesAttr([0, 0, 0])
    instancer.CreatePositionsAttr([(0, 0, 0), (2.5, 0, -1), (0.5, 0, -3.5)])
    instancer.CreateOrientationsAttr(Vt.QuathArray([
        Gf.Quath(1, 0, 0, 0),
        Gf.Quath(Gf.Rotation(Gf.Vec3d(0, 1, 0), 60).GetQuat()),
        Gf.Quath(Gf.Rotation(Gf.Vec3d(1, 0, 1), 25).GetQuat())]))
    instancer.CreateScalesAttr([(1, 1, 1), (0.7, 0.7, 0.7), (1.2, 1, 1.2)])

    # A PointInstancer inside a native instance: both composed.
    UsdGeom.Xform.Define(stage, "/Library/Pair")
    inner = UsdGeom.PointInstancer.Define(stage, "/Library/Pair/Instancer")
    place(inner, Gf.Vec3d(0, 0, 0.5), (0, -10, 0), (1, 1, 1))
    inner_proto = define_model(stage, "/Library/Pair/Instancer/Prototypes/Chair")
    inner.CreatePrototypesRel().SetTargets([inner_proto.GetPath()])
    inner.CreateProtoIndicesAttr([0, 0])
    inner.CreatePositionsAttr([(0, 0, 0), (1.8, 0, 0.4)])
    for i, translate in enumerate([Gf.Vec3d(-6, 0, -6), Gf.Vec3d(4, 0, -7)]):
        prim = UsdGeom.Xform.Define(stage, f"/World/Pair{i}")
        place(prim, translate, (0, 30 * (i + 1), 0), (1, 1, 1))
        prim.GetPrim().GetReferences().AddInternalReference("/Library/Pair")
        prim.GetPrim().SetInstanceable(True)

    # A floor, uninstanced, so there is a shadow to be in the right place.
    floor = define_box(stage, "/World/Floor", (0.6, 0.6, 0.6))
    place(floor, Gf.Vec3d(0, -0.55, -2), (0, 0, 0), (24, 0.1, 18))

    stage.GetRootLayer().Save()
    return stage


def world_meshes(stage, drop_local=False):
    """Every mesh the instanced stage renders, with its world transform.

    Composed by USD, not by hdClaude: the xform cache walks instance proxies
    exactly as the specification composes them, and a point instancer's
    instances come from its own ComputeInstanceTransformsAtTime. With
    `drop_local`, a mesh inside an instance is placed by that instance alone --
    the Kitchen Set defect -- to build the stage that must not match.
    """
    cache = UsdGeom.XformCache(Usd.TimeCode.Default())
    meshes = []
    predicate = Usd.TraverseInstanceProxies(Usd.PrimDefaultPredicate)

    def instancer_meshes(instancer_prim):
        instancer = UsdGeom.PointInstancer(instancer_prim)
        # Relative to the instancer, and including each prototype root's own
        # transform, which is ComputeInstanceTransformsAtTime's default.
        transforms = instancer.ComputeInstanceTransformsAtTime(
            Usd.TimeCode.Default(), Usd.TimeCode.Default())
        indices = instancer.GetProtoIndicesAttr().Get()
        protos = instancer.GetPrototypesRel().GetForwardedTargets()
        instancer_world = cache.GetLocalToWorldTransform(instancer_prim)
        found = []
        for n, index in enumerate(indices):
            proto_prim = stage.GetPrimAtPath(protos[index])
            proto_world = cache.GetLocalToWorldTransform(proto_prim)
            for prim in Usd.PrimRange(proto_prim, predicate):
                if not prim.IsA(UsdGeom.Mesh):
                    continue
                # The mesh relative to its prototype root, since the root's
                # own transform is already in the instance transform.
                within = cache.GetLocalToWorldTransform(prim) * proto_world.GetInverse()
                if drop_local:
                    within = Gf.Matrix4d(1.0)
                found.append((prim, within * transforms[n] * instancer_world))
        return found

    for prim in Usd.PrimRange(stage.GetPseudoRoot(), predicate):
        path = str(prim.GetPath())
        if not path.startswith("/World"):
            continue
        if prim.IsA(UsdGeom.PointInstancer):
            meshes.extend(instancer_meshes(prim))
            continue
        if not prim.IsA(UsdGeom.Mesh):
            continue
        # Anything under a point instancer's prototypes is placed above.
        if "/Prototypes/" in path:
            continue
        world = cache.GetLocalToWorldTransform(prim)
        if drop_local and prim.IsInstanceProxy():
            # The instance's placement alone: the mesh collapses onto its
            # model's root, as every Kitchen Set mesh did.
            instance = prim
            while instance and not instance.IsInstance():
                instance = instance.GetParent()
            world = cache.GetLocalToWorldTransform(instance)
        meshes.append((prim, world))
    return meshes


def build_flattened(instanced, path, drop_local=False):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", 1.0)
    camera(stage)
    for n, (prim, world) in enumerate(world_meshes(instanced, drop_local)):
        source = UsdGeom.Mesh(prim)
        mesh = UsdGeom.Mesh.Define(stage, f"/World/Mesh{n}")
        points = [world.Transform(Gf.Vec3d(p)) for p in source.GetPointsAttr().Get()]
        mesh.CreatePointsAttr([Gf.Vec3f(p) for p in points])
        mesh.CreateFaceVertexCountsAttr(source.GetFaceVertexCountsAttr().Get())
        # A transform whose determinant is negative mirrors the mesh, and the
        # winding has to follow or every face turns inside out.
        indices = list(source.GetFaceVertexIndicesAttr().Get())
        if world.ExtractRotationMatrix().GetDeterminant() < 0.0:
            flipped = []
            offset = 0
            for count in source.GetFaceVertexCountsAttr().Get():
                face = indices[offset:offset + count]
                flipped.extend([face[0]] + face[1:][::-1])
                offset += count
            indices = flipped
        mesh.CreateFaceVertexIndicesAttr(indices)
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.CreateDisplayColorAttr(source.GetDisplayColorAttr().Get())
    stage.GetRootLayer().Save()
    return stage


def render(stage_path, output):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "64"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    result = subprocess.run(
        ["cmd", "/c", RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", "320", stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render {stage_path}")


def compare(baseline, candidate):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, baseline, candidate],
                            capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    instanced_path = os.path.join(out, "instanced.usda")
    for name in ("instanced.usda", "flattened.usda", "dropped.usda"):
        if os.path.exists(os.path.join(out, name)):
            os.remove(os.path.join(out, name))
    instanced = build_instanced(instanced_path)
    meshes = world_meshes(instanced)
    build_flattened(instanced, os.path.join(out, "flattened.usda"))
    build_flattened(instanced, os.path.join(out, "dropped.usda"), drop_local=True)
    print(f"  {len(meshes)} meshes: {sum(1 for m, _ in meshes if m.IsInstanceProxy())} "
          "through native instances, the rest through point instancers and the floor")

    images = {}
    for name in ("instanced", "flattened", "dropped"):
        images[name] = os.path.join(out, f"{name}.0001.exr")
        render(os.path.join(out, f"{name}.usda"), images[name])

    status, report = compare(images["flattened"], images["instanced"])
    print("  instanced against flattened:\n" + report)
    control, control_report = compare(images["flattened"], images["dropped"])
    print("  mesh transforms dropped against flattened:\n" + control_report)

    failures = 0
    if status != 0:
        print("FAIL: instanced geometry does not render where USD places it")
        failures += 1
    if control == 0:
        print("FAIL: the comparison cannot see a dropped mesh transform, so it "
              "checks nothing")
        failures += 1
    print("usd instancing: " + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
