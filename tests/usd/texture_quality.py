"""Texture quality caps what a scene's images cost, and keeps their mean.

The setting exists for memory: ALab ships 6,261 images totalling 49.6 GB, more
than the machine this was written on has, and no amount of its geometry comes
near that. So the assertion is about bytes as well as pixels.

A 1024-pixel texture is rendered at each quality. `high` keeps it, `medium`
(a cap of 1024) leaves it alone because it is already within the cap, and `low`
(256) reduces it by two halvings -- a sixteenth of the bytes, which the stats
report states exactly. Each halving is a box filter, so the image's *mean* is
preserved: a reduced texture is a blurrier picture of the same light, and the
render must keep the frame's energy while differing in detail. Both are
checked, because either alone would pass a reduction that quietly darkened
every texture -- filtering sRGB-encoded values without decoding them does
exactly that.

Usage: python texture_quality.py <output directory>
Runs in the OpenUSD environment with the hdClaude plugin installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "render_claude.bat")

EDGE = 1024
#: Cell size in texels. Small enough that two halvings genuinely lose it.
CELL = 4


def write_checker(path):
    """A checkerboard, as an uncompressed TGA.

    Two greys rather than black and white, so that a filter that averages
    sRGB-encoded values instead of decoding them first shifts the mean
    measurably rather than by a rounding step.
    """
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    EDGE & 0xFF, EDGE >> 8, EDGE & 0xFF, EDGE >> 8, 24, 0])
    dark = bytes([60, 60, 60])
    light = bytes([200, 200, 200])
    rows = bytearray()
    for y in range(EDGE):
        row = bytearray()
        for x in range(EDGE):
            row += light if ((x // CELL) + (y // CELL)) % 2 == 0 else dark
        rows += row
    with open(path, "wb") as image:
        image.write(header + bytes(rows))


def build_stage(path, texture):
    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    stage.SetMetadata("metersPerUnit", 1.0)

    cam = UsdGeom.Camera.Define(stage, "/camera")
    cam.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 2.2))
    cam.CreateFocalLengthAttr(24.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))

    # A quad that fills the frame, so every pixel reads the texture.
    mesh = UsdGeom.Mesh.Define(stage, "/World/Card")
    mesh.CreatePointsAttr([(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar(
        "st", Sdf.ValueTypeNames.TexCoord2fArray,
        UsdGeom.Tokens.varying).Set([(0, 0), (1, 0), (1, 1), (0, 1)])

    material = UsdShade.Material.Define(stage, "/World/Material")
    reader = UsdShade.Shader.Define(stage, "/World/Material/st")
    reader.CreateIdAttr("UsdPrimvarReader_float2")
    reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")
    sampler = UsdShade.Shader.Define(stage, "/World/Material/texture")
    sampler.CreateIdAttr("UsdUVTexture")
    sampler.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(texture)
    sampler.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        reader.CreateOutput("result", Sdf.ValueTypeNames.Float2))
    surface = UsdShade.Shader.Define(stage, "/World/Material/surface")
    surface.CreateIdAttr("UsdPreviewSurface")
    surface.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
    surface.CreateInput("specularColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0, 0, 0))
    surface.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        sampler.CreateOutput("rgb", Sdf.ValueTypeNames.Float3))
    material.CreateSurfaceOutput().ConnectToSource(
        surface.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)

    # A plain white dome, so the card is lit evenly and its mean is its own.
    UsdLux.DomeLight.Define(stage, "/Lights/Sky").CreateIntensityAttr(1.0)
    stage.GetRootLayer().Save()


def render(stage_path, output, quality, stats):
    environment = dict(os.environ)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "64"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    environment["HDCLAUDE_TEXTURE_QUALITY"] = quality
    environment["HDCLAUDE_STATS_REPORT"] = stats
    result = subprocess.run(
        ["cmd", "/c", RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", "320", stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render at quality {quality}")
    values = {}
    with open(stats, encoding="utf-8") as report:
        for line in report:
            name, _, value = line.strip().partition(" ")
            values[name] = float(value)
    return values


def diff(*arguments):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    result = subprocess.run([tool, *arguments], capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    texture = os.path.join(out, "checker.tga")
    write_checker(texture)
    stage = os.path.join(out, "card.usda")
    build_stage(stage, texture)

    images = {}
    bytes_held = {}
    for quality in ("high", "medium", "low"):
        images[quality] = os.path.join(out, f"{quality}.0001.exr")
        if os.path.exists(images[quality]):
            os.remove(images[quality])
        stats = render(stage, images[quality], quality,
                       os.path.join(out, f"{quality}.stats"))
        bytes_held[quality] = int(stats["textureBytes"])
        print(f"  {quality}: {bytes_held[quality]} texture bytes")

    failures = 0
    # RGBA8, four bytes a texel, at the authored size and at the cap.
    expected = {"high": EDGE * EDGE * 4, "medium": EDGE * EDGE * 4,
                "low": 256 * 256 * 4}
    for quality, want in expected.items():
        if bytes_held[quality] != want:
            print(f"FAIL: {quality} holds {bytes_held[quality]} texture bytes, "
                  f"expected {want}")
            failures += 1

    # The mean survives the reduction: a box filter preserves it, and decoding
    # sRGB before averaging is what keeps that true of an encoded image.
    status, report = diff("--energy", images["high"], images["low"])
    kept = None
    for line in report.splitlines():
        if "candidate keeps" in line:
            kept = float(line.split("keeps")[1].split("%")[0])
    print(f"  low keeps {kept}% of the light of high")
    if status != 0 or kept is None or abs(kept - 100.0) > 0.5:
        print("FAIL: reducing a texture did not preserve the frame's light")
        failures += 1

    # And it is a different picture: the checker is gone at a sixteenth of the
    # texels, so a comparison that could not see that would be checking nothing.
    control, control_report = diff(images["high"], images["low"])
    print("  high against low:\n" + control_report)
    if control == 0:
        print("FAIL: the low-quality render is pixel-identical to the high one, "
              "so nothing was reduced")
        failures += 1

    print("texture quality: " + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
