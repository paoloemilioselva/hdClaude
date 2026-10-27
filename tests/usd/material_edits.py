"""An edit to a live material network renders what a fresh render does.

hdClaude compiles a material once per distinct network: the material compiler
keeps every program it has built under a key taken from the network's nodes,
parameters, connections and terminals, and the scene store shares one material
index between materials whose keys agree. A key that missed something an edit
changes would serve the old program, and nothing but a picture would show it.

So this renders a stage through one recorder, edits the material that a mesh
and a PointInstancer's prototype share, and after each edit compares that
recorder with a fresh one -- by the colour of each surface -- and with the frame
before, which the edit must change:

  material input     the Material's interface input, which feeds the surface
  image readers      base_color rewired from one image to another, and an
                     image's file swapped and swapped back
  nodegraph          base_color rewired to a NodeGraph output, and that
                     graph's own interface input edited
  multi-output       an input moved between two outputs of one node
  constant           base_color disconnected and given a value

Rewiring the Material's own surface output to another shader is the one edit
that is not checked: OpenUSD 26.03's UsdImaging invalidates a Material for its
`inputs:` and not for its `outputs:`, so no renderer is told -- Storm keeps the
old shader too. It is still made, and reported, so the day UsdImaging delivers
it this says so.

Usage: python material_edits.py <output directory>
Runs in the OpenUSD environment, like incremental_sync.py.
"""

import os
import sys

os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "8"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

from pxr import Gf, Sdf, Usd, UsdAppUtils, UsdGeom, UsdLux, UsdShade

WIDTH = 240
TOLERANCE = 12       # per 8-bit channel, of a region's mean
CHANGE = 12

CUBE_POINTS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
               (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
CUBE_COUNTS = [4] * 6
CUBE_INDICES = [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4,
                2, 3, 7, 6, 1, 2, 6, 5, 3, 0, 4, 7]

# Where each surface is in the 240x160 frame: the plain cube in the middle of
# the bottom half, and the second of the instancer's four along the top.
PLAIN = (110, 105, 130, 125)
INSTANCE = (70, 40, 90, 60)


def cube(stage, path, scale):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr([(scale * x, scale * y, scale * z)
                           for x, y, z in CUBE_POINTS])
    mesh.CreateFaceVertexCountsAttr(CUBE_COUNTS)
    mesh.CreateFaceVertexIndicesAttr(CUBE_INDICES)
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    # Every face covers the whole texture, so an image reads as itself.
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(
        "st", Sdf.ValueTypeNames.TexCoord2fArray,
        UsdGeom.Tokens.faceVarying).Set([(0, 0), (1, 0), (1, 1), (0, 1)] * 6)
    return mesh


def shader(stage, path, identifier):
    node = UsdShade.Shader.Define(stage, path)
    node.CreateIdAttr(identifier)
    return node


def solid_png(directory, name, rgb):
    from PySide6.QtGui import QColor, QImage
    image = QImage(8, 8, QImage.Format_RGB32)
    image.fill(QColor(*rgb))
    path = os.path.join(directory, name)
    image.save(path)
    return path


def make_stage(path, directory):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 9))
    camera.CreateFocalLengthAttr(35.0)
    camera.CreateHorizontalApertureAttr(36.0)
    camera.CreateVerticalApertureAttr(24.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    UsdLux.DomeLight.Define(stage, "/World/Sky").CreateIntensityAttr(1.0)

    material = UsdShade.Material.Define(stage, "/World/Looks/Mx")
    material_input = material.CreateInput("baseColor", Sdf.ValueTypeNames.Color3f)
    material_input.Set(Gf.Vec3f(0.8, 0.8, 0.1))

    def surface(name, colour=None):
        node = shader(stage, "/World/Looks/Mx/" + name,
                      "ND_standard_surface_surfaceshader")
        node.CreateInput("base", Sdf.ValueTypeNames.Float).Set(1.0)
        node.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
        if colour is not None:
            node.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(colour)
        node.CreateOutput("out", Sdf.ValueTypeNames.Token)
        return node

    main = surface("Surface")
    surface("AltSurface", Gf.Vec3f(0.9, 0.9, 0.9))

    pngs = {name: solid_png(directory, name + ".png", rgb) for name, rgb in (
        ("red", (220, 20, 20)), ("green", (20, 220, 20)), ("blue", (20, 20, 220)))}
    for name, png in (("ImageA", pngs["red"]), ("ImageB", pngs["green"])):
        image = shader(stage, "/World/Looks/Mx/" + name, "ND_image_color3")
        image.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(png)
        image.CreateOutput("out", Sdf.ValueTypeNames.Color3f)

    split = shader(stage, "/World/Looks/Mx/Split", "ND_separate3_color3")
    split.CreateInput("in", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        UsdShade.ConnectableAPI(stage.GetPrimAtPath("/World/Looks/Mx/ImageA")),
        "out")
    for name in ("outr", "outg", "outb"):
        split.CreateOutput(name, Sdf.ValueTypeNames.Float)

    graph = UsdShade.NodeGraph.Define(stage, "/World/Looks/Mx/Tinted")
    tint = graph.CreateInput("tint", Sdf.ValueTypeNames.Color3f)
    tint.Set(Gf.Vec3f(1.0, 1.0, 1.0))
    graph_image = shader(stage, "/World/Looks/Mx/Tinted/Image", "ND_image_color3")
    graph_image.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(pngs["blue"])
    graph_image.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
    product = shader(stage, "/World/Looks/Mx/Tinted/Mul", "ND_multiply_color3")
    product.CreateInput("in1", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        graph_image.ConnectableAPI(), "out")
    product.CreateInput("in2", Sdf.ValueTypeNames.Color3f).ConnectToSource(tint)
    product.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
    graph.CreateOutput("out", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        product.ConnectableAPI(), "out")

    main.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        material_input)
    material.CreateSurfaceOutput("mtlx").ConnectToSource(
        main.ConnectableAPI(), "out")

    instancer = UsdGeom.PointInstancer.Define(stage, "/World/Instancer")
    box = cube(stage, "/World/Instancer/Protos/Box", 0.45)
    instancer.CreatePrototypesRel().SetTargets([box.GetPath()])
    instancer.CreatePositionsAttr([(-3.0, 1.2, 0), (-1.0, 1.2, 0),
                                   (1.0, 1.2, 0), (3.0, 1.2, 0)])
    instancer.CreateProtoIndicesAttr([0, 0, 0, 0])
    plain = cube(stage, "/World/Plain", 0.6)
    plain.AddTranslateOp().Set(Gf.Vec3d(0.0, -1.5, 0.0))
    for prim in (box.GetPrim(), plain.GetPrim()):
        UsdShade.MaterialBindingAPI.Apply(prim).Bind(material)
    return stage, pngs


def gl_context():
    """An offscreen OpenGL context, as usdrecord makes one."""
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext, QSurfaceFormat
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
    return surface


class Recorder:
    """A frame recorder, which keeps one imaging engine across its frames."""

    def __init__(self, stage, directory, label):
        self.stage = stage
        self.directory = directory
        self.label = label
        self.recorder = UsdAppUtils.FrameRecorder("HdClaudeRendererPlugin", True)
        self.recorder.SetImageWidth(WIDTH)
        self.recorder.SetColorCorrectionMode("disabled")
        self.recorder.SetIncludedPurposes(["default", "render"])

    def colours(self, step):
        """The mean colour of the plain cube and of one instance."""
        from PySide6.QtGui import QImage
        path = os.path.join(self.directory, "%s_%02d.png" % (self.label, step))
        camera = UsdGeom.Camera(self.stage.GetPrimAtPath("/camera"))
        if not self.recorder.Record(self.stage, camera, Usd.TimeCode.Default(),
                                    path):
            raise SystemExit("FAIL: the frame was not recorded")
        image = QImage(path)

        def mean(box):
            x0, y0, x1, y1 = box
            total = [0, 0, 0]
            for y in range(y0, y1):
                for x in range(x0, x1):
                    colour = image.pixelColor(x, y)
                    total[0] += colour.red()
                    total[1] += colour.green()
                    total[2] += colour.blue()
            count = (x1 - x0) * (y1 - y0)
            return tuple(round(c / count) for c in total)

        return mean(PLAIN) + mean(INSTANCE)


def difference(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "material_edits.usda")
    if os.path.exists(path):
        os.remove(path)
    context = gl_context()
    stage, pngs = make_stage(path, out)
    live = Recorder(stage, out, "live")

    failures = 0
    step = 0
    previous = live.colours(step)

    def check(label, known_upstream=False):
        nonlocal failures, step, previous
        step += 1
        edited = live.colours(step)
        reference = Recorder(stage, out, "fresh").colours(step)
        agrees = difference(edited, reference) <= TOLERANCE
        changed = difference(reference, previous) > CHANGE
        previous = reference
        if known_upstream:
            print("%s  %-30s live %s, fresh %s (UsdImaging does not report "
                  "this edit; not counted)"
                  % ("note" if not agrees else "ok  ", label, edited[:3],
                     reference[:3]))
            return
        ok = agrees and changed
        failures += 0 if ok else 1
        print("%s  %-30s live %s, fresh %s%s"
              % ("ok  " if ok else "FAIL", label, edited[:3], reference[:3],
                 "" if changed else "; the edit changed nothing"))

    material = UsdShade.Material(stage.GetPrimAtPath("/World/Looks/Mx"))
    main_surface = UsdShade.Shader(stage.GetPrimAtPath("/World/Looks/Mx/Surface"))
    base_color = main_surface.GetInput("base_color")
    image_b = UsdShade.Shader(stage.GetPrimAtPath("/World/Looks/Mx/ImageB"))
    split = UsdShade.ConnectableAPI(stage.GetPrimAtPath("/World/Looks/Mx/Split"))
    graph = UsdShade.NodeGraph(stage.GetPrimAtPath("/World/Looks/Mx/Tinted"))

    material.GetInput("baseColor").Set(Gf.Vec3f(0.1, 0.8, 0.8))
    check("material input edited")
    base_color.ConnectToSource(UsdShade.ConnectableAPI(
        stage.GetPrimAtPath("/World/Looks/Mx/ImageA")), "out")
    check("base_color <- image A")
    base_color.ConnectToSource(UsdShade.ConnectableAPI(image_b.GetPrim()), "out")
    check("base_color <- image B")
    image_b.GetInput("file").Set(pngs["blue"])
    check("image B's file swapped")
    image_b.GetInput("file").Set(pngs["green"])
    check("image B's file swapped back")
    base_color.ConnectToSource(graph.GetOutput("out"))
    check("base_color <- nodegraph")
    graph.GetInput("tint").Set(Gf.Vec3f(4.0, 4.0, 0.0))
    check("nodegraph input edited")

    base_color.DisconnectSource()
    base_color.Set(Gf.Vec3f(1.0, 1.0, 1.0))
    check("base_color a constant")
    base = main_surface.GetInput("base")
    base.ConnectToSource(split, "outr")
    check("base <- separate3.outr")
    base.ConnectToSource(split, "outg")
    check("base <- separate3.outg")

    alternative = UsdShade.ConnectableAPI(
        stage.GetPrimAtPath("/World/Looks/Mx/AltSurface"))
    material.GetSurfaceOutput("mtlx").ConnectToSource(alternative, "out")
    check("surface output rewired", known_upstream=True)

    del live
    del context
    print("usd material edits: " +
          ("passed" if failures == 0 else "%d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
