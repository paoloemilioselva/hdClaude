"""Write hdClaude's Houdini parameter files from the delegate's own settings.

Houdini shows a render delegate's settings from dialog scripts named after the
renderer plugin, found in soho/parameters on HOUDINI_PATH (HDK, "Houdini USD
and Hydra"):

  HdClaudeRendererPlugin_Viewport.ds   the viewport's Display Options
  HdClaudeRendererPlugin_Global.ds     the Render Settings LOP, and so husk

Both are generated rather than written by hand, from the list the delegate
publishes through HdRenderDelegate::GetRenderSettingDescriptors -- names,
keys, types and defaults -- so they cannot drift from the code. What the
descriptors do not carry is here: the folder each setting goes in and the
choices a string setting accepts. A setting this file does not place is an
error, so a new one cannot be published to Houdini unfiled.

The Global file follows Karma's (BRAY_HdKarma_Global.ds): every setting is a
value parameter tagged with its USD type, and a `<key>_control` parameter
choosing whether the Render Settings LOP authors it. The control defaults to
"none", so a new Render Settings LOP authors nothing it was not asked to.

Usage, in the standalone OpenUSD environment with the plugin installed:

  python generate_houdini_parameters.py            write the files
  python generate_houdini_parameters.py --check    fail if they are stale

Defaults are the delegate's built-in ones: every HDCLAUDE_* variable is
cleared first, because the delegate takes its defaults from the environment.
"""

import os
import sys

for _name in [n for n in os.environ if n.startswith("HDCLAUDE_")]:
    del os.environ[_name]

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "resources", "houdini", "soho", "parameters")
PLUGIN = "HdClaudeRendererPlugin"

# Folder, in display order, and the settings in it.
FOLDERS = [
    ("Sampling", ["samplesPerPixel", "samplesPerFrame", "maxBounces"]),
    ("Lighting", ["exposure", "environmentIntensity", "sunIntensity", "upAxis",
                  "lightGeometry"]),
    ("Geometry", ["subdivisionLevel", "adaptiveSubdivision", "perFaceSubdivision",
                  "subdivisionEdgePixels", "subdivisionOffScreenLevel",
                  "subdivisionFaceBudget", "subdivisionFollowsCamera",
                  "retessellate", "curveGeometry", "curveSides",
                  "curveSegmentSamples", "sphereRadial", "sphereAxial",
                  "splatTransport", "cullVolumes"]),
    ("Textures", ["textureQuality"]),
    ("Reconstruction", ["reconstruction", "reconstructionModel",
                        "reconstructionPreset", "reconstructionAutoExposure",
                        "dlssRuntimeDirectory"]),
    ("Output", ["depthStyle"]),
    ("Diagnostics", ["device", "shaderDirectory", "statsReport",
                     "vulkanValidation", "trace", "profileKernels",
                     "poisonPathState", "dumpShaders", "frameLog",
                     "repeatRenders", "republishAt"]),
]

# Folders shown in the viewport's Display Options. Diagnostics are for a
# render someone is investigating, set from the environment or a Render
# Settings LOP, and would bury the controls that matter while looking. Output
# is what the viewport itself reads: the package sets the depth style to the
# one UsdRenderers.json declares, and a viewport control could only break it.
VIEWPORT_FOLDERS = {"Sampling", "Lighting", "Geometry", "Textures",
                    "Reconstruction"}

# The values a string setting accepts, as the delegate parses them.
MENUS = {
    "upAxis": ["Y", "Z"],
    "depthStyle": ["opengl", "linear"],
    "curveGeometry": ["implicit", "swept"],
    "textureQuality": ["high", "medium", "low"],
    "splatTransport": ["coverage", "volume"],
    "reconstruction": ["off", "dlaa", "quality", "balanced", "performance",
                       "ultraperformance"],
    "reconstructionModel": ["super-resolution", "ray-reconstruction"],
    "reconstructionPreset": ["default", "stable", "transformer",
                             "transformer-alt"],
}


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


def descriptors():
    """(label, key, kind, default) for each setting, in the delegate's order."""
    from pxr import UsdImagingGL

    context = gl_context()
    engine = UsdImagingGL.Engine()
    if not engine.SetRendererPlugin(PLUGIN):
        raise SystemExit("the hdClaude renderer plugin did not load")
    kinds = {
        UsdImagingGL.RendererSettingType.FLAG: "bool",
        UsdImagingGL.RendererSettingType.INT: "int",
        UsdImagingGL.RendererSettingType.FLOAT: "float",
        UsdImagingGL.RendererSettingType.STRING: "string",
    }
    result = [(s.name, s.key, kinds[s.type], s.defValue)
              for s in engine.GetRendererSettingsList()]
    del engine
    del context
    return result


def quote(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def default_text(kind, value):
    if kind == "bool":
        return "1" if value else "0"
    if kind == "int":
        return str(int(value))
    if kind == "float":
        return repr(float(value))
    return quote(value)


PARM_TYPE = {"bool": "toggle", "int": "int", "float": "float",
             "string": "string"}


def value_parm(label, key, kind, default, folder, uiscope, control):
    lines = [
        "    parm {",
        "        name    %s" % quote(key),
        "        label   %s" % quote(label),
        "        type    %s" % PARM_TYPE[kind],
        "        size    1",
        "        default { %s }" % default_text(kind, default),
        "        parmtag { \"spare_category\" %s }" % quote(folder),
    ]
    if uiscope:
        lines.append("        parmtag { \"uiscope\" %s }" % quote(uiscope))
    lines.append("        parmtag { \"usdvaluetype\" %s }" % quote(kind))
    if key in MENUS:
        lines.append("        menu {")
        for item in MENUS[key]:
            lines.append("            %s %s" % (quote(item), quote(item)))
        lines.append("        }")
    if control:
        lines.append(
            "        disablewhen \"{ %s_control == block } { %s_control == none }\""
            % (key, key))
    lines.append("    }")
    return lines


def control_parm(label, key, kind):
    return [
        "    parm {",
        "        name    %s" % quote(key + "_control"),
        "        label   %s" % quote(label),
        "        type    string",
        "        default { \"none\" }",
        "        menujoin {",
        "            [ \"import loputils\" ]",
        "            [ \"return loputils.createEditPropertiesControlMenu(kwargs, '%s')\" ]"
        % kind,
        "            language python",
        "        }",
        "        parmtag { \"sidefx::look\" \"icon\" }",
        "    }",
    ]


HEADER = """\
/// hdClaude's settings for Houdini. Generated by
/// scripts/generate_houdini_parameters.py from the delegate's render setting
/// descriptors; edit that script or the delegate, not this file.
///
/// %s

#include "$HFS/houdini/soho/parameters/CommonMacros.ds"
{
    name    "hdClaude"
    label   "Claude GPU Path Tracer"
    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }
    parmtag { spare_classtags "render" }
"""


def write_file(settings, viewport):
    by_key = {s[1]: s for s in settings}
    placed = [key for _, keys in FOLDERS for key in keys]
    unplaced = [key for key in by_key if key not in placed]
    missing = [key for key in placed if key not in by_key]
    if unplaced or missing:
        raise SystemExit(
            "generate_houdini_parameters.py is out of step with the delegate: "
            "settings not placed in a folder: %s; placed but not published: %s"
            % (unplaced or "none", missing or "none"))

    purpose = ("The viewport's Display Options (uiscope viewport)."
               if viewport else
               "The Render Settings LOP, which authors each setting on the "
               "RenderSettings prim when its control says so.")
    lines = [HEADER % purpose]
    for folder, keys in FOLDERS:
        if viewport and folder not in VIEWPORT_FOLDERS:
            continue
        for key in keys:
            label, _, kind, default = by_key[key]
            if not viewport:
                lines += control_parm(label, key, kind)
            lines += value_parm(label, key, kind, default, folder,
                                "viewport" if viewport else None,
                                control=not viewport)
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(arguments):
    check = "--check" in arguments
    settings = descriptors()
    files = {
        PLUGIN + "_Viewport.ds": write_file(settings, viewport=True),
        PLUGIN + "_Global.ds": write_file(settings, viewport=False),
    }
    stale = []
    for name, text in files.items():
        path = os.path.join(OUT, name)
        current = None
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                current = f.read()
        if current == text:
            continue
        if check:
            stale.append(name)
        else:
            os.makedirs(OUT, exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("wrote", path)
    if stale:
        print("stale: %s. Run scripts/generate_houdini_parameters.py."
              % ", ".join(stale))
        return 1
    if check:
        print("Houdini parameter files match the delegate's %d settings"
              % len(settings))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
