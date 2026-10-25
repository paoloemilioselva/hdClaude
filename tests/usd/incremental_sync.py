"""An edit to a live stage renders what a fresh render of the edited stage does.

A viewport edit is not a new stage: Hydra dirties the prims it touched, and a
delegate that keeps what nothing touched has to keep it correctly. This
renders a stage, edits it in place through one engine, and after each edit
compares that engine against a new engine given the same stage -- by picking,
which reports the prim and the surface point under each probe, so a placement
kept from before the edit, or geometry not refined again, shows as a
different hit:

  moved      a subdivided mesh translated: only its placement is dirty, so it
             must not be refined again (`meshesRefined` unchanged)
  hidden     a mesh made invisible, then visible
  reshaped   the subdivided mesh's points changed: that is its geometry, and
             it must be refined again

A control compares the frame from before the move with the fresh render
after it, and must disagree, or the probes cannot see a move.

Usage: python incremental_sync.py <output directory>
Runs in the OpenUSD environment with the hdClaude plugin installed, like
picking.py, and tests what compile.py last installed.
"""

import os
import sys

# Before the engine reads them: picking needs a frame, not a converged one.
os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "4"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux

WIDTH = 240
HEIGHT = 160
GRID = (16, 10)      # probes across and down the frame
POINT_TOLERANCE = 1e-3

CUBE_POINTS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
               (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
CUBE_COUNTS = [4] * 6
CUBE_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]


def make_stage(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)

    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 6))
    camera.CreateFocalLengthAttr(35.0)
    camera.CreateHorizontalApertureAttr(36.0)
    camera.CreateVerticalApertureAttr(24.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))

    UsdLux.DistantLight.Define(stage, "/World/Sun")

    # Catmull-Clark, the scheme a UsdGeomMesh has unless it says otherwise,
    # so it is refined at the default level: the cube rounds into a blob.
    blob = UsdGeom.Mesh.Define(stage, "/World/Blob")
    blob.CreatePointsAttr(CUBE_POINTS)
    blob.CreateFaceVertexCountsAttr(CUBE_COUNTS)
    blob.CreateFaceVertexIndicesAttr(CUBE_INDICES)
    blob.AddTranslateOp().Set(Gf.Vec3d(-2.0, 0.0, 0.0))

    box = UsdGeom.Mesh.Define(stage, "/World/Box")
    box.CreatePointsAttr([(0.7 * x, 0.7 * y, 0.7 * z)
                          for x, y, z in CUBE_POINTS])
    box.CreateFaceVertexCountsAttr(CUBE_COUNTS)
    box.CreateFaceVertexIndicesAttr(CUBE_INDICES)
    box.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    box.AddTranslateOp().Set(Gf.Vec3d(2.2, 0.0, 0.0))
    box.AddRotateXYZOp().Set(Gf.Vec3f(25, 35, 0))
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


class View:
    """One engine on the stage, rendered until converged and probed."""

    def __init__(self, stage):
        self.stage = stage
        self.engine = UsdImagingGL.Engine()
        if not self.engine.SetRendererPlugin("HdClaudeRendererPlugin"):
            raise SystemExit("FAIL: the hdClaude renderer plugin did not load")
        self.engine.SetCameraPath(Sdf.Path("/camera"))
        self.engine.SetRenderViewport(Gf.Vec4d(0, 0, WIDTH, HEIGHT))
        self.params = UsdImagingGL.RenderParams()
        self.params.enableLighting = True

    def render(self):
        root = self.stage.GetPseudoRoot()
        for _ in range(200):
            self.engine.Render(root, self.params)
            if self.engine.IsConverged():
                break

    def refined(self):
        return int(self.engine.GetRenderStats().get("meshesRefined", 0))

    def probes(self):
        """(prim path, hit point) under each probe, or None for a miss."""
        self.render()
        frustum = UsdGeom.Camera(self.stage.GetPrimAtPath("/camera")).GetCamera(
            Usd.TimeCode.Default()).frustum
        root = self.stage.GetPseudoRoot()
        hits = []
        for j in range(GRID[1]):
            for i in range(GRID[0]):
                ndc = Gf.Vec2d(-0.9 + 1.8 * (i + 0.5) / GRID[0],
                               -0.9 + 1.8 * (j + 0.5) / GRID[1])
                narrowed = frustum.ComputeNarrowedFrustum(
                    ndc, Gf.Vec2d(1.0 / WIDTH, 1.0 / HEIGHT))
                result = self.engine.TestIntersection(
                    narrowed.ComputeViewMatrix(),
                    narrowed.ComputeProjectionMatrix(), root, self.params)
                if not result or not result[2] or result[2].isEmpty:
                    hits.append(None)
                else:
                    hits.append((str(result[2]), Gf.Vec3d(result[1])))
        return hits


def disagreements(a, b):
    count = 0
    for x, y in zip(a, b):
        if (x is None) != (y is None):
            count += 1
        elif x is not None and (
                x[0] != y[0] or (x[1] - y[1]).GetLength() > POINT_TOLERANCE):
            count += 1
    return count


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "incremental_sync.usda")
    if os.path.exists(path):
        os.remove(path)
    stage = make_stage(path)
    context = gl_context()

    live = View(stage)
    before = live.probes()
    covered = sum(1 for hit in before if hit is not None)
    print("  %d of %d probes on a surface" % (covered, len(before)))
    failures = 0 if covered >= len(before) // 6 else 1
    if failures:
        print("FAIL: too few probes on a surface to check anything")

    def check(label, expect_refined):
        nonlocal failures
        refined_before = live.refined()
        edited = live.probes()
        refined = live.refined() - refined_before
        fresh = View(stage)
        reference = fresh.probes()
        del fresh
        differing = disagreements(edited, reference)
        ok = differing == 0 and (refined > 0) == expect_refined
        failures += 0 if ok else 1
        print("%s  %-10s %d probes differ from a fresh render; %d mesh(es) "
              "refined again (expected %s)"
              % ("ok  " if ok else "FAIL", label, differing, refined,
                 "some" if expect_refined else "none"))
        return reference

    blob = UsdGeom.Mesh(stage.GetPrimAtPath("/World/Blob"))
    blob.GetOrderedXformOps()[0].Set(Gf.Vec3d(-1.2, 0.6, 0.5))
    moved = check("moved", expect_refined=False)

    control = disagreements(before, moved)
    print("%s  control: %d probes see the move"
          % ("ok  " if control else "FAIL", control))
    failures += 0 if control else 1

    box = UsdGeom.Imageable(stage.GetPrimAtPath("/World/Box"))
    box.MakeInvisible()
    check("hidden", expect_refined=False)
    box.MakeVisible()
    check("shown", expect_refined=False)

    blob.GetPointsAttr().Set([(1.3 * x, 0.8 * y, z) for x, y, z in CUBE_POINTS])
    check("reshaped", expect_refined=True)

    del live
    del context
    print("usd incremental sync: " +
          ("passed" if failures == 0 else "%d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
