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

The same checker is then written as a one-channel greyscale image. It must
cost a quarter of the RGB image's bytes at every quality -- a one-channel map
is kept as one channel rather than widened to four -- and render
pixel-identically to it, because the sampler swizzles it back to grey. Both are
declared sRGB, so the comparison goes through the one-channel sRGB format and
its decode as well as the swizzle.

Last, the setting is given the way a host gives it -- through the imaging
engine, before the first frame -- rather than through the environment. The
render pass applies it before it compiles materials and so before any image
is decoded, and the stats' decoded bytes say whether that still holds: an
image decoded at full size and reduced afterwards would count its full size.
Switching to `high` mid-session must then publish the full-size image again.

And a texture that does not fit is left out rather than requested: with the
texture memory limit at 1 MB the 4 MB checker is not uploaded, the stats count
it, the render says so, and the frame still renders -- the placeholder where
the checker would be.

Usage: python texture_quality.py <output directory>
Runs in the OpenUSD environment, and
tests the plugin HDCLAUDE_PLUGIN_DIR names -- the build tree's, under
ctest -- or else what compile.py last installed.
"""

import os
import subprocess
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# The render script for this platform; both fix the same usdrecord flags.
if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

EDGE = 1024
#: Cell size in texels. Small enough that two halvings genuinely lose it.
CELL = 4


def write_checker(path, channels=3):
    """A checkerboard, as an uncompressed TGA: RGB, or one-channel greyscale.

    Two greys rather than black and white, so that a filter that averages
    sRGB-encoded values instead of decoding them first shifts the mean
    measurably rather than by a rounding step.
    """
    kind = 2 if channels == 3 else 3   # true-colour, or greyscale
    header = bytes([0, 0, kind, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    EDGE & 0xFF, EDGE >> 8, EDGE & 0xFF, EDGE >> 8,
                    8 * channels, 0])
    dark = bytes([60] * channels)
    light = bytes([200] * channels)
    rows = bytearray()
    for y in range(EDGE):
        row = bytearray()
        for x in range(EDGE):
            row += light if ((x // CELL) + (y // CELL)) % 2 == 0 else dark
        rows += row
    with open(path, "wb") as image:
        image.write(header + bytes(rows))


def build_stage(path, texture, color_space=None):
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
    if color_space:
        sampler.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set(
            color_space)
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


def render(stage_path, output, quality, stats, limit=None, log=None):
    environment = dict(os.environ)
    if limit is not None:
        environment["HDCLAUDE_TEXTURE_MEMORY_LIMIT"] = str(limit)
    environment["HDCLAUDE_SAMPLES_PER_PIXEL"] = "64"
    environment["HDCLAUDE_RECONSTRUCTION"] = "off"
    environment["HDCLAUDE_TEXTURE_QUALITY"] = quality
    environment["HDCLAUDE_STATS_REPORT"] = stats
    result = subprocess.run(
        [*RENDER, "--camera", "/camera", "--frames", "1",
         "--imageWidth", "320", stage_path, output.replace("0001", "####")],
        capture_output=True, text=True, env=environment)
    if result.returncode != 0 or not os.path.exists(output):
        print(result.stdout[-2000:], result.stderr[-2000:])
        raise SystemExit(f"FAIL: could not render at quality {quality}")
    if log is not None:
        log.append(result.stdout + result.stderr)
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


def one_channel(out):
    """A greyscale image is held in one channel and renders as its RGB twin."""
    failures = 0
    renders = {}
    held = {}
    for channels in (3, 1):
        texture = os.path.join(out, f"checker{channels}.tga")
        write_checker(texture, channels)
        stage = os.path.join(out, f"card{channels}.usda")
        build_stage(stage, texture, "sRGB")
        for quality in ("high", "low"):
            image = os.path.join(out, f"c{channels}_{quality}.0001.exr")
            if os.path.exists(image):
                os.remove(image)
            stats = render(stage, image, quality,
                           os.path.join(out, f"c{channels}_{quality}.stats"))
            renders[(channels, quality)] = image
            held[(channels, quality)] = int(stats["textureBytes"])
    for quality, edge in (("high", EDGE), ("low", 256)):
        rgb, grey = held[(3, quality)], held[(1, quality)]
        print(f"  {quality}: RGB {rgb} bytes, one channel {grey} bytes")
        if rgb != edge * edge * 4 or grey != edge * edge:
            print(f"FAIL: at {quality} the RGB image holds {rgb} bytes and the "
                  f"one-channel image {grey}; expected {edge * edge * 4} and "
                  f"{edge * edge}")
            failures += 1
        status, report = diff(renders[(3, quality)], renders[(1, quality)])
        print(f"  {quality}, RGB against one channel: {report}")
        if status != 0 or not report.startswith("rms 0 ") or " worst 0 " not in report:
            print(f"FAIL: at {quality} the one-channel texture does not render "
                  "as its RGB twin")
            failures += 1
    return failures


def host_setting(stage_path):
    """A texture quality set by the host applies before images are decoded."""
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext, QSurfaceFormat
    from PySide6.QtOpenGL import (QOpenGLFramebufferObject,
                                  QOpenGLFramebufferObjectFormat)
    from PySide6.QtWidgets import QApplication
    from pxr import UsdImagingGL

    application = QApplication.instance() or QApplication(sys.argv)
    surface = QOffscreenSurface()
    surface.setFormat(QSurfaceFormat())
    surface.create()
    context = QOpenGLContext()
    context.setFormat(QSurfaceFormat())
    context.create()
    context.makeCurrent(surface)
    fbo = QOpenGLFramebufferObject(QSize(1, 1), QOpenGLFramebufferObjectFormat())
    fbo.bind()

    os.environ["HDCLAUDE_SAMPLES_PER_PIXEL"] = "1"
    os.environ.pop("HDCLAUDE_TEXTURE_QUALITY", None)
    stage = Usd.Stage.Open(stage_path)
    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin("HdClaudeRendererPlugin"):
        print("FAIL: the hdClaude renderer plugin did not load")
        return 1
    engine.SetCameraPath(Sdf.Path("/camera"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, 64, 64))
    params = UsdImagingGL.RenderParams()
    params.enableLighting = True

    def frame():
        for _ in range(50):
            engine.Render(stage.GetPseudoRoot(), params)
            if engine.IsConverged():
                break
        return engine.GetRenderStats()

    failures = 0
    engine.SetRendererSetting("textureQuality", "low")
    stats = frame()
    decoded = int(stats.get("textureBytes", -1))
    uploaded = int(stats.get("textureBytesUploaded", -1))
    print(f"  host set low first: {decoded} bytes decoded, {uploaded} uploaded")
    if decoded != 256 * 256 * 4 or uploaded != 256 * 256 * 4:
        print(f"FAIL: with low set by the host before the first frame, "
              f"{decoded} bytes were decoded and {uploaded} uploaded; expected "
              f"{256 * 256 * 4} each, so the image was decoded before the cap "
              "applied")
        failures += 1

    engine.SetRendererSetting("textureQuality", "high")
    uploaded = int(frame().get("textureBytesUploaded", -1))
    print(f"  host switched to high: {uploaded} bytes uploaded")
    if uploaded != EDGE * EDGE * 4:
        print(f"FAIL: after switching to high, {uploaded} bytes were uploaded; "
              f"expected {EDGE * EDGE * 4}, the full-size image")
        failures += 1
    del engine
    fbo.release()
    context.doneCurrent()
    return failures


def over_budget(out, stage, full):
    """A texture over the memory limit is left out, counted and reported."""
    image = os.path.join(out, "limited.0001.exr")
    if os.path.exists(image):
        os.remove(image)
    log = []
    stats = render(stage, image, "high", os.path.join(out, "limited.stats"),
                   limit=1, log=log)
    left = int(stats.get("texturesOverBudget", -1))
    needed = int(stats.get("textureBytesOverBudget", -1))
    uploaded = int(stats.get("textureBytesUploaded", -1))
    reported = "did not fit" in log[0]
    print(f"  limit 1 MB: {left} left out ({needed} bytes), {uploaded} uploaded, "
          f"reported: {reported}")
    failures = 0
    if left != 1 or needed != EDGE * EDGE * 4 or uploaded != 0:
        print(f"FAIL: under a 1 MB limit the {EDGE * EDGE * 4}-byte checker "
              f"should be left out alone; {left} texture(s) were, needing "
              f"{needed} bytes, and {uploaded} bytes were uploaded")
        failures += 1
    if not reported:
        print("FAIL: a texture left out for memory was not reported")
        failures += 1
    status, _ = diff(full, image)
    if status == 0:
        print("FAIL: the frame without its texture matches the frame with it, "
              "so nothing was left out")
        failures += 1
    return failures


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

    failures += one_channel(out)
    failures += host_setting(stage)
    failures += over_budget(out, stage, images["high"])
    print("texture quality: " + ("passed" if failures == 0 else f"{failures} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
