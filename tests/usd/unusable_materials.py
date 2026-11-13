"""A prim bound to a material hdClaude cannot use is shaded as an unbound one.

hdClaude shades through MaterialX alone. A material it cannot use -- one with
only a glslfx or a RenderMan surface, so that Hydra hands over no network for
the "mtlx" context, or one whose surface is not a MaterialX node -- is in
effect no material, and what binds it falls back to what an unbound prim gets:
its displayColor. Before, it fell back to a flat grey, and Disney's Moana
island, whose 17,343 materials are all RenderMan and glslfx, rendered as one
grey stage under orange lights.

Five cubes, each with a red displayColor:

  unbound        the reference
  glslfx         bound to a material with only outputs:glslfx:surface
  ri             bound to a material with only outputs:ri:surface
  not MaterialX  bound to a material whose universal surface is a PxrDisneyBsdf
  MaterialX      bound to a blue standard_surface, which must stay blue

The three unusable materials must render the unbound cube's colour.

Usage: python unusable_materials.py <output directory>
Runs in the OpenUSD environment, like material_edits.py.
"""

import os
import sys

from pxr import Gf, Sdf, Usd, UsdAppUtils, UsdGeom, UsdLux, UsdShade

from material_edits import gl_context

# After that import, which sets its own: the engine reads these when made.
os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "64"
os.environ["HDCLAUDE_RECONSTRUCTION"] = "off"

WIDTH = 240
HEIGHT = 160
TOLERANCE = 12       # per 8-bit channel, of a region's mean
RED = Gf.Vec3f(0.8, 0.1, 0.1)
# Name, x position, and what the cube binds.
CUBES = [("Unbound", -4.0), ("Glslfx", -2.0), ("Ri", 0.0),
         ("NotMaterialX", 2.0), ("MaterialX", 4.0)]


def make_stage(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    camera = UsdGeom.Camera.Define(stage, "/camera")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 8))
    camera.CreateFocalLengthAttr(20.0)
    camera.CreateHorizontalApertureAttr(36.0)
    camera.CreateVerticalApertureAttr(24.0)
    # A uniform white dome: an unoccluded diffuse face renders its albedo.
    UsdLux.DomeLight.Define(stage, "/World/Sky")

    def shader(path, identifier):
        node = UsdShade.Shader.Define(stage, path)
        node.CreateIdAttr(identifier)
        return node

    glslfx = UsdShade.Material.Define(stage, "/World/Looks/Glslfx")
    preview = shader("/World/Looks/Glslfx/Surface", "UsdPreviewSurface")
    preview.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.1, 0.8, 0.1))
    glslfx.CreateSurfaceOutput("glslfx").ConnectToSource(
        preview.CreateOutput("surface", Sdf.ValueTypeNames.Token))

    ri = UsdShade.Material.Define(stage, "/World/Looks/Ri")
    disney = shader("/World/Looks/Ri/Surface", "PxrDisneyBsdf")
    disney.CreateInput("baseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.1, 0.8, 0.1))
    ri.CreateSurfaceOutput("ri").ConnectToSource(
        disney.CreateOutput("bxdf_out", Sdf.ValueTypeNames.Token))

    universal = UsdShade.Material.Define(stage, "/World/Looks/NotMaterialX")
    foreign = shader("/World/Looks/NotMaterialX/Surface", "PxrDisneyBsdf")
    foreign.CreateInput("baseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.1, 0.8, 0.1))
    universal.CreateSurfaceOutput().ConnectToSource(
        foreign.CreateOutput("out", Sdf.ValueTypeNames.Token))

    mtlx = UsdShade.Material.Define(stage, "/World/Looks/MaterialX")
    standard = shader("/World/Looks/MaterialX/Surface",
                      "ND_standard_surface_surfaceshader")
    standard.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.1, 0.1, 0.8))
    standard.CreateInput("specular", Sdf.ValueTypeNames.Float).Set(0.0)
    mtlx.CreateSurfaceOutput("mtlx").ConnectToSource(
        standard.CreateOutput("out", Sdf.ValueTypeNames.Token))

    materials = {"Glslfx": glslfx, "Ri": ri, "NotMaterialX": universal,
                 "MaterialX": mtlx}
    for name, x in CUBES:
        cube = UsdGeom.Cube.Define(stage, "/World/" + name)
        cube.CreateSizeAttr(1.2)
        cube.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
        cube.CreateDisplayColorAttr([RED])
        if name in materials:
            UsdShade.MaterialBindingAPI.Apply(cube.GetPrim()).Bind(
                materials[name])
    stage.Save()
    return stage


def colours(stage, directory):
    """The mean colour of the middle of each cube's front face."""
    from PySide6.QtGui import QImage
    path = os.path.join(directory, "unusable_materials.png")
    recorder = UsdAppUtils.FrameRecorder("HdClaudeRendererPlugin", True)
    recorder.SetImageWidth(WIDTH)
    recorder.SetColorCorrectionMode("disabled")
    recorder.SetIncludedPurposes(["default", "render"])
    recorder.SetCameraLightEnabled(False)
    camera = UsdGeom.Camera(stage.GetPrimAtPath("/camera"))
    if not recorder.Record(stage, camera, Usd.TimeCode.Default(), path):
        raise SystemExit("FAIL: the frame was not recorded")
    image = QImage(path)
    if image.width() != WIDTH or image.height() != HEIGHT:
        raise SystemExit("FAIL: the frame is %dx%d" %
                         (image.width(), image.height()))

    # The front faces are at z = 0.6, 7.4 from the camera, where the frame
    # spans 2 * 7.4 * 18 / 20 = 13.32 units: 18 pixels a unit, so a face is
    # 21 pixels across and the 14 in its middle are all face.
    def mean(x):
        cx, cy = round(WIDTH / 2 + x * WIDTH / 13.32), HEIGHT // 2
        total = [0, 0, 0]
        for y in range(cy - 7, cy + 7):
            for px in range(cx - 7, cx + 7):
                colour = image.pixelColor(px, y)
                total[0] += colour.red()
                total[1] += colour.green()
                total[2] += colour.blue()
        return tuple(round(c / 196) for c in total)

    return {name: mean(x) for name, x in CUBES}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "unusable_materials.usda")
    if os.path.exists(path):
        os.remove(path)
    context = gl_context()
    stage = make_stage(path)
    found = colours(stage, out)

    reference = found["Unbound"]
    failures = 0

    def report(ok, label, detail):
        nonlocal failures
        failures += 0 if ok else 1
        print("%s  %-14s %s" % ("ok  " if ok else "FAIL", label, detail))

    r, g, b = reference
    report(r > 2 * g and r > 2 * b and r > 64, "unbound",
           "%s, red from its displayColor" % (reference,))
    for name in ("Glslfx", "Ri", "NotMaterialX"):
        colour = found[name]
        difference = max(abs(x - y) for x, y in zip(colour, reference))
        report(difference <= TOLERANCE, name,
               "%s, the unbound cube's %s" % (colour, reference))
    r, g, b = found["MaterialX"]
    report(b > 2 * r and b > 2 * g, "MaterialX",
           "%s, blue from its own material" % (found["MaterialX"],))

    del context
    print("usd unusable materials: " +
          ("passed" if failures == 0 else "%d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
