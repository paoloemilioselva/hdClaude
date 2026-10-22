"""The depth AOV is the depth a Hydra host expects, through its projection.

Hydra's depth AOV is window depth in (0, 1): the hit taken through the host's
own view and projection, NDC z mapped from [-1, 1]. That is how hdEmbree and
Storm write it, how usdview decodes it (HdxPickFromRenderBufferTask, depth
range (0, 1)), and what Houdini's `depthstyle "opengl"` declares.

This reads it back the way usdview does. `TestIntersection` on a delegate
other than Storm takes the primId and depth AOVs of the last frame and
unprojects the pixel nearest the pick through the render's camera; the point
it returns is the depth AOV, decoded. Each probe compares that with the
analytic scene -- a tilted panel, a ground plane, a far wall, all flat, so
hdClaude draws them exactly -- for two cameras:

  perspective    off-centre, with a near plane of 0.05 and a far one of 500,
                 so the depth is far from linear
  orthographic   which has no eye: rays start on the near plane and are
                 parallel, and depth is linear in distance

The decoded depth must lie within the range the analytic surface takes
across that pixel, since the frame traces jittered points inside it. Pixels
that straddle two surfaces are left out; so are probes Hydra resolves to a
pixel whose footprint is not all one surface.

Usage: python depth.py <output directory>
Runs in the OpenUSD environment with the hdClaude plugin installed, like
picking.py, and tests what compile.py last installed.
"""

import math
import os
import sys

# Before the engine reads them: depth needs a frame, not a converged one.
os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "4"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux

WIDTH = 320
HEIGHT = 200
STEP = 15            # probe spacing, pixels
# The panel: three units square, centred above the origin and turned to face
# the cameras obliquely, so its depth varies across it in both directions.
PANEL_CENTRE = Gf.Vec3d(0.0, 0.5, 0.0)
PANEL_HALF = 1.5
PANEL_ROTATION = Gf.Matrix4d().SetRotate(
    Gf.Rotation(Gf.Vec3d(1, 0, 0), 20.0) * Gf.Rotation(Gf.Vec3d(0, 1, 0), 35.0))
MINIMUM_PROBES = 100  # per camera, so the test cannot pass by checking nothing
# Window depth is a float32 AOV; a few units in its last place near 1.
TOLERANCE = 1e-6


def make_stage(path):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

    persp = UsdGeom.Camera.Define(stage, "/persp")
    persp.AddTranslateOp().Set(Gf.Vec3d(1.0, 2.0, 9.0))
    persp.AddRotateXOp().Set(-10.0)
    persp.CreateFocalLengthAttr(30.0)
    persp.CreateHorizontalApertureAttr(36.0)
    persp.CreateVerticalApertureAttr(22.5)
    persp.CreateHorizontalApertureOffsetAttr(-4.0)
    persp.CreateVerticalApertureOffsetAttr(2.0)
    persp.CreateClippingRangeAttr(Gf.Vec2f(0.05, 500.0))

    ortho = UsdGeom.Camera.Define(stage, "/ortho")
    ortho.AddTranslateOp().Set(Gf.Vec3d(0.5, 1.0, 20.0))
    ortho.AddRotateXOp().Set(-15.0)
    ortho.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
    # Tenths of a scene unit: a view 12 units wide and 7.5 tall.
    ortho.CreateHorizontalApertureAttr(120.0)
    ortho.CreateVerticalApertureAttr(75.0)
    ortho.CreateClippingRangeAttr(Gf.Vec2f(1.0, 100.0))

    UsdLux.DomeLight.Define(stage, "/sky")
    panel = [PANEL_CENTRE + PANEL_ROTATION.TransformDir(
                 Gf.Vec3d(x * PANEL_HALF, y * PANEL_HALF, 0.0))
             for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    quads = (("Panel", panel),("Ground", [(-40, -1, 40), (40, -1, 40), (40, -1, -40),
                         (-40, -1, -40)]),
             ("Wall", [(-60, -5, -30), (60, -5, -30), (60, 40, -30),
                       (-60, 40, -30)]))
    for name, points in quads:
        mesh = UsdGeom.Mesh.Define(stage, "/" + name)
        mesh.CreatePointsAttr([Gf.Vec3f(*p) for p in points])
        mesh.CreateFaceVertexCountsAttr([4])
        mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.CreateDoubleSidedAttr(True)
    stage.GetRootLayer().Save()
    return stage


def nearest_hit(ray):
    """The analytic scene's nearest hit along `ray`: (distance, prim path)."""
    o, d = ray.startPoint, ray.direction
    best = (math.inf, None)
    normal = PANEL_ROTATION.TransformDir(Gf.Vec3d(0, 0, 1))
    if abs(Gf.Dot(d, normal)) > 1e-12:
        t = Gf.Dot(PANEL_CENTRE - o, normal) / Gf.Dot(d, normal)
        local = PANEL_ROTATION.GetInverse().TransformDir(o + d * t - PANEL_CENTRE)
        if t > 0.0 and abs(local[0]) < PANEL_HALF and abs(local[1]) < PANEL_HALF:
            best = min(best, (t, "/Panel"))
    if abs(d[1]) > 1e-12:
        t = (-1.0 - o[1]) / d[1]
        p = o + d * t
        if t > 0.0 and abs(p[0]) < 40.0 and abs(p[2]) < 40.0:
            best = min(best, (t, "/Ground"))
    if abs(d[2]) > 1e-12:
        t = (-30.0 - o[2]) / d[2]
        p = o + d * t
        if t > 0.0 and abs(p[0]) < 60.0 and -5.0 < p[1] < 40.0:
            best = min(best, (t, "/Wall"))
    return best


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


def check_camera(stage, camera_path):
    """Probes the depth one camera's frame leaves; returns the failure count."""
    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin("HdClaudeRendererPlugin"):
        print("FAIL: the hdClaude renderer plugin did not load")
        return 1
    engine.SetCameraPath(Sdf.Path(camera_path))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, WIDTH, HEIGHT))
    params = UsdImagingGL.RenderParams()
    params.enableLighting = True
    root = stage.GetPseudoRoot()
    for _ in range(200):
        engine.Render(root, params)
        if engine.IsConverged():
            break

    frustum = UsdGeom.Camera(stage.GetPrimAtPath(camera_path)).GetCamera(
        Usd.TimeCode.Default()).frustum
    world_to_clip = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

    def surface_at(x, y):
        """Analytic (prim, window depth) at window position x, y in pixels."""
        ray = frustum.ComputePickRay(Gf.Vec2d(2.0 * x / WIDTH - 1.0,
                                              2.0 * y / HEIGHT - 1.0))
        t, prim = nearest_hit(ray)
        if prim is None:
            return None, None
        clip = world_to_clip.Transform(ray.startPoint + ray.direction * t)
        return prim, (clip[2] + 1.0) * 0.5

    failures = checked = 0
    for py in range(STEP // 2, HEIGHT, STEP):
        for px in range(STEP // 2, WIDTH, STEP):
            prim, _ = surface_at(px + 0.5, py + 0.5)
            if prim is None:
                continue
            ndc = Gf.Vec2d(2.0 * (px + 0.5) / WIDTH - 1.0,
                           2.0 * (py + 0.5) / HEIGHT - 1.0)
            narrowed = frustum.ComputeNarrowedFrustum(
                ndc, Gf.Vec2d(1.0 / WIDTH, 1.0 / HEIGHT))
            result = engine.TestIntersection(
                narrowed.ComputeViewMatrix(),
                narrowed.ComputeProjectionMatrix(), root, params)
            if not result or not result[2] or result[2].isEmpty:
                failures += 1
                print("FAIL  %s pixel (%d, %d): nothing picked, expected %s"
                      % (camera_path, px, py, prim))
                continue

            # The pixel Hydra read: it unprojects at the pixel's lower-left
            # corner (HdxPickResult), which is where the point projects back.
            clip = world_to_clip.Transform(Gf.Vec3d(result[0]))
            x = round((clip[0] + 1.0) * 0.5 * WIDTH)
            y = round((clip[1] + 1.0) * 0.5 * HEIGHT)
            depth = (clip[2] + 1.0) * 0.5

            footprint = [surface_at(x + u, y + v) for u, v in
                         ((0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0.5))]
            prims = {p for p, _ in footprint}
            if len(prims) != 1:
                continue
            expected = prims.pop()
            checked += 1
            low = min(z for _, z in footprint) - TOLERANCE
            high = max(z for _, z in footprint) + TOLERANCE
            picked = str(result[2])
            if picked != expected or not low <= depth <= high:
                failures += 1
                print("FAIL  %s pixel (%d, %d): picked %s at depth %.8f, "
                      "expected %s in [%.8f, %.8f]"
                      % (camera_path, x, y, picked, depth, expected, low, high))

    del engine
    if checked < MINIMUM_PROBES:
        failures += 1
        print("FAIL  %s: only %d probes checked, expected at least %d"
              % (camera_path, checked, MINIMUM_PROBES))
    print("%s  %-14s %d probes, %d failed"
          % ("ok  " if not failures else "FAIL", camera_path, checked, failures))
    return failures


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    stage = make_stage(os.path.join(out, "depth.usda"))
    context = gl_context()
    failures = sum(check_camera(stage, path) for path in ("/persp", "/ortho"))
    del context
    print("depth: %s" % ("%d failed" % failures if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
