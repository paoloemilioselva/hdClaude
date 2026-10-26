"""Picking in a viewer finds what hdClaude drew under the cursor.

usdview picks through `UsdImagingGLEngine::TestIntersection`, which for a
renderer other than Storm reads the `primId`, `instanceId` and `depth` AOVs
the last frame left behind (HdxPickFromRenderBufferTask) and maps the prim id
back to a path through the render index. This drives that same call, through
the plugin, at the projected centre of each object:

  prim          A cube and a sphere pick as themselves.
  instance      Each of a point instancer's three placements picks as the
                prototype, the instancer, and that placement's own index.
  cull volume   A wall inside a cube cull volume that affects camera rays
                picks as the sphere behind it: what is picked is what is
                seen, not what the BVH holds.
  miss          The sky picks nothing.

The camera's frustum is off-centre, so every one of these also checks that
camera rays follow the host's projection.

Usage: python picking.py <output directory>
Runs in the OpenUSD environment, like cull_volume_equivalence.py, and
tests the plugin HDCLAUDE_PLUGIN_DIR names -- the build tree's, under
ctest -- or else what compile.py last installed. Needs
an OpenGL context, which it makes the way usdrecord does.
"""

import os
import sys

# Before the engine reads them: picking needs a frame, not a converged one.
os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "4"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux

WIDTH = 300
HEIGHT = 200


def make_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

    cam = UsdGeom.Camera.Define(stage, "/camera")
    cam.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 10.0))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(24.0)
    # Off-centre, as a film offset or a host's window makes a frustum. Every
    # pick below is placed through USD's own frustum, so it finds its object
    # only if camera rays honour the whole projection rather than a field of
    # view and an aspect, which describe only a centred frustum.
    cam.CreateHorizontalApertureOffsetAttr(-3.0)
    cam.CreateVerticalApertureOffsetAttr(-2.5)

    UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(1.0)

    box = UsdGeom.Cube.Define(stage, "/World/Box")
    box.CreateSizeAttr(1.5)
    box.AddTranslateOp().Set(Gf.Vec3d(-3.0, 0.0, 0.0))

    ball = UsdGeom.Sphere.Define(stage, "/World/Ball")
    ball.CreateRadiusAttr(0.8)

    row = UsdGeom.PointInstancer.Define(stage, "/World/Row")
    tile = UsdGeom.Cube.Define(stage, "/World/Row/Prototypes/Tile")
    tile.CreateSizeAttr(0.6)
    row.CreatePrototypesRel().SetTargets([tile.GetPath()])
    row.CreateProtoIndicesAttr([0, 0, 0])
    row.CreatePositionsAttr([Gf.Vec3f(0.5, -2.2, 0.0), Gf.Vec3f(2.0, -2.2, 0.0),
                             Gf.Vec3f(3.5, -2.2, 0.0)])

    # The wall stands in front of the hidden sphere, both centred on one ray
    # from the camera, and the volume encloses the wall and not the sphere.
    wall = UsdGeom.Cube.Define(stage, "/World/Wall")
    wall.CreateSizeAttr(1.2)
    wall.AddTranslateOp().Set(Gf.Vec3d(3.0, 1.5, 2.0))
    hidden = UsdGeom.Sphere.Define(stage, "/World/Hidden")
    hidden.CreateRadiusAttr(0.8)
    hidden.AddTranslateOp().Set(Gf.Vec3d(4.2, 2.1, -1.2))
    volume = stage.DefinePrim("/World/Cut", "CubeCullVolume")
    if not volume.IsA(UsdGeom.Xformable):
        raise RuntimeError("the usdCullVolume schema is not installed")
    volume.CreateAttribute("size", Sdf.ValueTypeNames.Double).Set(1.6)
    volume.CreateAttribute("cullVolume:affectsSecondaryRays",
                           Sdf.ValueTypeNames.Bool).Set(False)
    UsdGeom.Xformable(volume).AddTranslateOp().Set(Gf.Vec3d(3.0, 1.5, 2.0))

    # Two native instances of one peg. UsdImaging gives them a shared
    # prototype and an instancer, and a pick names the instance proxy.
    peg = UsdGeom.Xform.Define(stage, "/PegAsset")
    stage.GetPrimAtPath("/PegAsset").SetSpecifier(Sdf.SpecifierClass)
    UsdGeom.Cylinder.Define(stage, "/PegAsset/Geom").CreateRadiusAttr(0.35)
    for name, x in (("PegA", -4.4), ("PegB", -2.0)):
        instance = UsdGeom.Xform.Define(stage, "/World/" + name)
        instance.AddTranslateOp().Set(Gf.Vec3d(x, -2.2, 0.0))
        instance.GetPrim().GetReferences().AddInternalReference(peg.GetPath())
        instance.GetPrim().SetInstanceable(True)

    stage.GetRootLayer().Save()
    return stage


def gl_context():
    """An offscreen OpenGL context, as usdrecord makes one."""
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext, QSurfaceFormat
    from PySide6.QtOpenGL import (QOpenGLFramebufferObject,
                                  QOpenGLFramebufferObjectFormat)
    from PySide6.QtWidgets import QApplication

    application = QApplication.instance() or QApplication(sys.argv)
    surface = QOffscreenSurface()
    surface.setFormat(QSurfaceFormat())
    surface.create()
    surface._application = application
    surface._context = QOpenGLContext()
    surface._context.setFormat(QSurfaceFormat())
    surface._context.create()
    surface._context.makeCurrent(surface)
    surface._fbo = QOpenGLFramebufferObject(QSize(1, 1),
                                            QOpenGLFramebufferObjectFormat())
    surface._fbo.bind()
    return surface


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    stage = make_stage(os.path.join(out, "picking.usda"))

    context = gl_context()
    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin("HdClaudeRendererPlugin"):
        print("FAIL: the hdClaude renderer plugin did not load")
        return 1
    engine.SetCameraPath(Sdf.Path("/camera"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, WIDTH, HEIGHT))

    params = UsdImagingGL.RenderParams()
    params.showRender = True
    params.enableLighting = True
    root = stage.GetPseudoRoot()
    for _ in range(200):
        engine.Render(root, params)
        if engine.IsConverged():
            break

    frustum = UsdGeom.Camera(stage.GetPrimAtPath("/camera")).GetCamera(
        Usd.TimeCode.Default()).frustum
    to_ndc = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

    def pick(world):
        ndc = to_ndc.Transform(Gf.Vec3d(*world))
        # A probe off the image would pass a miss and fail a hit for reasons
        # that have nothing to do with picking.
        if abs(ndc[0]) > 0.95 or abs(ndc[1]) > 0.95:
            raise RuntimeError("probe %s is outside the frame (NDC %.3f %.3f)"
                               % (world, ndc[0], ndc[1]))
        narrowed = frustum.ComputeNarrowedFrustum(
            Gf.Vec2d(ndc[0], ndc[1]), Gf.Vec2d(1.0 / WIDTH, 1.0 / HEIGHT))
        result = engine.TestIntersection(narrowed.ComputeViewMatrix(),
                                         narrowed.ComputeProjectionMatrix(),
                                         root, params)
        if not result or not result[2] or result[2].isEmpty:
            return None
        # (point, normal, prim path, instance index, instancer path, ...)
        return (str(result[2]), str(result[4]) if result[4] else "",
                result[3])

    checks = [
        ("prim: cube", (-3.0, 0.0, 0.75), ("/World/Box", "", None)),
        ("prim: sphere", (0.0, 0.0, 0.8), ("/World/Ball", "", None)),
        ("cull volume: the sphere behind the culled wall",
         (3.0, 1.5, 2.0), ("/World/Hidden", "", None)),
        ("miss: sky", (-1.0, 2.2, 0.0), None),
    ]
    for name, x in (("PegA", -4.4), ("PegB", -2.0)):
        checks.append(("native instance " + name, (x, -2.2, 0.35),
                       ("/World/%s/Geom" % name, None, None)))
    for index, x in enumerate((0.5, 2.0, 3.5)):
        checks.append(("instance %d" % index, (x, -2.2, 0.3),
                       ("/World/Row/Prototypes/Tile", "/World/Row", index)))

    failures = 0
    for name, world, expected in checks:
        got = pick(world)
        if expected is None:
            ok = got is None
        elif got is None:
            ok = False
        else:
            ok = got[0] == expected[0] and (
                expected[2] is None or
                (got[1] == expected[1] and got[2] == expected[2]))
        failures += 0 if ok else 1
        print("%s  %-48s got %s" % ("ok  " if ok else "FAIL", name, got))

    del engine
    del context
    print("%d of %d checks failed" % (failures, len(checks)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
