"""Render stats describe the stage they were taken from.

The stats are how a host shows what a scene costs -- Houdini's render-stats
panel and viewport, usdview's HUD and the hdClaude usdview panel all read
`GetRenderStats()` -- and a figure that is plausible and wrong is worse than
none: it is the one that decides whether flattened instancing is still cheap
enough on a stage (docs/render-stats.md).

So the stage is built with every count known by construction: a cube placed by
a PointInstancer that is itself a prototype of another PointInstancer, three
outer placements times four inner ones, plus one plain mesh. That is 13
placements of 2 prototypes, the cube rprim alone instanced, two instancers
deep, and the top-level structure built over 13 instances of 64 bytes. The
memory figures cannot be predicted to the byte, so they are held to what they
must be: present, positive where the scene has something in them, and
consistent with each other. The names usdview and Houdini display are held to
the hdClaude figures they alias.

Usage: python render_stats.py <output directory>
Runs in the OpenUSD environment, against the plugin HDCLAUDE_PLUGIN_DIR names
(the build tree's, under ctest) or else what compile.py last installed.
"""

import os
import sys

os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "4"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, Vt

HERE = os.path.dirname(os.path.abspath(__file__))

WIDTH = 160
HEIGHT = 120
OUTER = 3
INNER = 4

CUBE_POINTS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
               (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
CUBE_COUNTS = [4] * 6
CUBE_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]


def cube(stage, path, scale):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr([Gf.Vec3f(*p) * scale for p in CUBE_POINTS])
    mesh.CreateFaceVertexCountsAttr(CUBE_COUNTS)
    mesh.CreateFaceVertexIndicesAttr(CUBE_INDICES)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    return mesh


def make_stage(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.DefinePrim("/World", "Xform")

    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 2, 14))
    camera.CreateFocalLengthAttr(30)
    UsdLux.DomeLight.Define(stage, "/World/dome").CreateIntensityAttr(1.0)

    # The inner instancer, four cubes in a row, and the outer one placing it
    # three times. The inner instancer lives under the outer's prototypes, so
    # Hydra sees it as an instancer instanced by another: depth 2.
    outer = UsdGeom.PointInstancer.Define(stage, "/World/outer")
    inner = UsdGeom.PointInstancer.Define(stage, "/World/outer/prototypes/inner")
    cube(stage, "/World/outer/prototypes/inner/prototypes/cube", 0.3)
    inner.CreatePrototypesRel().SetTargets(
        [Sdf.Path("/World/outer/prototypes/inner/prototypes/cube")])
    inner.CreateProtoIndicesAttr(Vt.IntArray([0] * INNER))
    inner.CreatePositionsAttr(Vt.Vec3fArray(
        [Gf.Vec3f(1.0 * i - 1.5, 0, 0) for i in range(INNER)]))
    outer.CreatePrototypesRel().SetTargets(
        [Sdf.Path("/World/outer/prototypes/inner")])
    outer.CreateProtoIndicesAttr(Vt.IntArray([0] * OUTER))
    outer.CreatePositionsAttr(Vt.Vec3fArray(
        [Gf.Vec3f(0, 1.2 * j, 0) for j in range(OUTER)]))
    # Prototypes are not drawn where they are defined, only where placed.
    stage.GetPrimAtPath("/World/outer/prototypes").SetSpecifier(Sdf.SpecifierOver)

    plain = cube(stage, "/World/plain", 0.5)
    UsdGeom.XformCommonAPI(plain).SetTranslate(Gf.Vec3d(0, -1.5, 0))
    stage.SetDefaultPrim(stage.GetPrimAtPath("/World"))
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
    path = os.path.join(out, "render_stats.usda")
    if os.path.exists(path):
        os.remove(path)
    stage = make_stage(path)
    context = gl_context()

    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin("HdClaudeRendererPlugin"):
        raise SystemExit("FAIL: the hdClaude renderer plugin did not load")
    engine.SetCameraPath(Sdf.Path("/camera"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, WIDTH, HEIGHT))
    params = UsdImagingGL.RenderParams()
    params.enableLighting = True
    root = stage.GetPseudoRoot()
    for _ in range(200):
        engine.Render(root, params)
        if engine.IsConverged():
            break
    stats = engine.GetRenderStats()

    failures = []

    def expect(label, ok, detail=""):
        print("  %-5s %s%s" % ("ok" if ok else "FAIL", label,
                                (": " + detail) if detail else ""))
        if not ok:
            failures.append(label)

    def number(key):
        value = stats.get(key)
        return float(value) if isinstance(value, (int, float)) else None

    placements = OUTER * INNER + 1
    expect("instances", number("instances") == placements,
           "%s, expected %d" % (number("instances"), placements))
    expect("prototypes", number("prototypes") == 2, str(number("prototypes")))
    expect("placementsPerPrototype",
           number("placementsPerPrototype") == placements / 2.0,
           str(number("placementsPerPrototype")))
    expect("instancedRprims", number("instancedRprims") == 1,
           str(number("instancedRprims")))
    expect("instancingDepth", number("instancingDepth") == 2,
           str(number("instancingDepth")))
    expect("largestPlacementCount",
           number("largestPlacementCount") == OUTER * INNER,
           str(number("largestPlacementCount")))
    # The rprim's Hydra path, which for a prototype under a point instancer is
    # the USD path with a `ForInstancer<hash>` element UsdImaging appends.
    largest = str(stats.get("largestPlacementPrim", ""))
    expect("largestPlacementPrim",
           largest.startswith("/World/outer/prototypes/inner/prototypes/cube"),
           largest)

    expect("tlasChunks", number("tlasChunks") == 1, str(number("tlasChunks")))
    expect("tlasInstanceBytes", number("tlasInstanceBytes") == 64 * placements,
           "%s, expected %d" % (number("tlasInstanceBytes"), 64 * placements))
    for key in ("tlasBytes", "tlasScratchBytes", "blasBytes",
                "placementTableBytes", "vertexBytes", "indexBytes",
                "imageBytes", "bytesPerPlacement"):
        value = number(key)
        expect(key, value is not None and value > 0, str(value))
    for key in ("tlasBuildMs", "instancingMs"):
        value = number(key)
        expect(key, value is not None and value >= 0, str(value))
    per = number("bytesPerPlacement") or 0.0
    whole = ((number("tlasBytes") or 0) + (number("tlasInstanceBytes") or 0) +
             (number("placementTableBytes") or 0)) / placements
    expect("bytesPerPlacement is the per-placement total",
           abs(per - whole) < 1e-6, "%g against %g" % (per, whole))

    # The names usdview and Houdini display, held to what they alias.
    expect("primvar is vertexBytes", number("primvar") == number("vertexBytes"))
    expect("topology is indexBytes", number("topology") == number("indexBytes"))
    expect("textureMemory is imageBytes",
           number("textureMemory") == number("imageBytes"))
    expect("gpuMemoryUsed", (number("gpuMemoryUsed") or 0) > 0,
           str(number("gpuMemoryUsed")))
    expect("numCompletedSamples is samplesCompleted",
           number("numCompletedSamples") == number("samplesCompleted"),
           "%s against %s" % (number("numCompletedSamples"),
                              number("samplesCompleted")))
    expect("percentDone at convergence", number("percentDone") == 100.0,
           str(number("percentDone")))
    expect("rendererStage at convergence",
           stats.get("rendererStage") == "Converged",
           str(stats.get("rendererStage")))
    expect("totalClockTime", (number("totalClockTime") or 0) > 0,
           str(number("totalClockTime")))

    # Every line UsdRenderers.json puts on Houdini's viewport names a key the
    # dictionary has. A path to nothing draws an empty line, and nobody reads
    # an empty line as a missing stat.
    import json
    with open(os.path.join(HERE, "..", "..", "resources", "houdini",
                           "UsdRenderers.json"), encoding="utf-8") as f:
        entry = json.load(f)["HdClaudeRendererPlugin"]
    for line in entry["viewstats"]:
        node = stats
        for part in entry["statsdatapaths"].get(line, "").split("."):
            node = dict(node).get(part) if hasattr(node, "get") else None
        expect("viewport line %s has a value" % line,
               isinstance(node, str) and node != "", repr(node))
    expect("Textures line on a stage with no textures",
           dict(stats.get("hud", {})).get("textures") == "0 on device",
           repr(dict(stats.get("hud", {})).get("textures")))

    # The usdview panel formats this same dictionary; run its formatter on it
    # from the source tree, so a key it reads that went missing or changed
    # type fails here rather than in a viewer nobody is watching.
    sys.path.insert(0, os.path.join(HERE, "..", "..", "resources", "usdview"))
    try:
        from hdClaudeUsdview.stats_panel import _render
        page = _render(dict(stats))
        expect("usdview panel renders the stats",
               "Placements" in page and "Top-level structure" in page)
    except ImportError as error:
        # Qt is what usdview brings; an environment without it cannot run
        # usdview either, and says so rather than failing.
        print("  skip  usdview panel: %s" % error)

    del engine
    del context
    if failures:
        print("FAIL: %d render stat(s) wrong: %s" % (len(failures),
                                                     ", ".join(failures)))
        return 1
    print("PASS: render stats agree with the stage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
