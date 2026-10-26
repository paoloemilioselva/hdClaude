"""What a live engine draws follows the purposes and the root it is given.

Hydra syncs only the prims a render pass includes, by render tag and by
collection, and a prim excluded later -- a purpose turned off in a viewer's
menu, a narrower root -- is simply never synced again. Its last publication
stays with the delegate, so a delegate that draws everything it holds goes on
drawing it: proxy and render geometry stayed on screen after both were turned
off (docs/code-review-2026-10-26.md, I1).

One engine is taken through a sequence of purpose and root selections, and
after each its picks are compared with a new engine given the same selection:

  every purpose      default, render, proxy and guide geometry all drawn
  proxy, guide off   both gone
  render off         render geometry gone too
  every purpose      all of it back -- from what the delegate kept, since
                     nothing re-synced it
  left root          only the prims under /World/Left, by collection

Usage: python purposes.py <output directory>
Runs in the OpenUSD environment, like picking.py, and
tests the plugin HDCLAUDE_PLUGIN_DIR names -- the build tree's, under
ctest -- or else what compile.py last installed.
"""

import os
import sys

# Before the engine reads them: picking needs a frame, not a converged one.
os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "4"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux

from incremental_sync import gl_context

WIDTH = 240
HEIGHT = 160
PRIMS = [("/World/Left/Default", -3.0, None),
         ("/World/Left/Render", -1.0, "render"),
         ("/World/Right/Proxy", 1.0, "proxy"),
         ("/World/Right/Guide", 3.0, "guide")]


def make_stage(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 8))
    camera.CreateFocalLengthAttr(20.0)
    camera.CreateHorizontalApertureAttr(36.0)
    camera.CreateVerticalApertureAttr(24.0)
    UsdLux.DistantLight.Define(stage, "/World/Sun")
    for path, x, purpose in PRIMS:
        cube = UsdGeom.Cube.Define(stage, path)
        cube.CreateSizeAttr(1.2)
        cube.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
        if purpose:
            cube.CreatePurposeAttr(purpose)
    return stage


class View:
    def __init__(self, stage):
        self.stage = stage
        self.engine = UsdImagingGL.Engine()
        if not self.engine.SetRendererPlugin("HdClaudeRendererPlugin"):
            raise SystemExit("FAIL: the hdClaude renderer plugin did not load")
        self.engine.SetCameraPath(Sdf.Path("/camera"))
        self.engine.SetRenderViewport(Gf.Vec4d(0, 0, WIDTH, HEIGHT))
        self.params = UsdImagingGL.RenderParams()
        self.params.enableLighting = True

    def picks(self, render, proxy, guide, root):
        self.params.showRender = render
        self.params.showProxy = proxy
        self.params.showGuides = guide
        prim = self.stage.GetPrimAtPath(root)
        for _ in range(200):
            self.engine.Render(prim, self.params)
            if self.engine.IsConverged():
                break
        frustum = UsdGeom.Camera(self.stage.GetPrimAtPath("/camera")).GetCamera(
            Usd.TimeCode.Default()).frustum
        to_ndc = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()
        found = []
        for _, x, _ in PRIMS:
            ndc = to_ndc.Transform(Gf.Vec3d(x, 0, 0.6))
            narrowed = frustum.ComputeNarrowedFrustum(
                Gf.Vec2d(ndc[0], ndc[1]), Gf.Vec2d(1.0 / WIDTH, 1.0 / HEIGHT))
            result = self.engine.TestIntersection(
                narrowed.ComputeViewMatrix(),
                narrowed.ComputeProjectionMatrix(), prim, self.params)
            found.append(str(result[2]) if result and result[2] and
                         not result[2].isEmpty else None)
        return found


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "purposes.usda")
    if os.path.exists(path):
        os.remove(path)
    stage = make_stage(path)
    context = gl_context()

    everything = [p for p, _, _ in PRIMS]
    sequence = [
        ("every purpose", (True, True, True, "/"), everything),
        ("proxy, guide off", (True, False, False, "/"), everything[:2] + [None, None]),
        ("render off", (False, False, False, "/"), everything[:1] + [None] * 3),
        ("every purpose", (True, True, True, "/"), everything),
        ("left root", (True, True, True, "/World/Left"), everything[:2] + [None, None]),
    ]

    live = View(stage)
    failures = 0
    for label, selection, expected in sequence:
        got = live.picks(*selection)
        fresh = View(stage)
        reference = fresh.picks(*selection)
        del fresh
        ok = got == reference == expected
        failures += 0 if ok else 1
        print("%s  %-17s live %s" % ("ok  " if ok else "FAIL", label, got))
        if not ok:
            print("      fresh %s, expected %s" % (reference, expected))

    del live
    del context
    print("usd purposes: " +
          ("passed" if failures == 0 else "%d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
