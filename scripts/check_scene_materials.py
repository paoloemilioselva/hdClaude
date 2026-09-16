"""Report material inputs that cannot mean what they say.

A renderer can only clamp. It cannot tell an asset that the value it authored is
impossible, and it cannot tell *which* asset when the value arrives per pixel
through a texture. So this reads the stage instead, where the graph is still
visible, and names what it finds.

What it looks for today is the class that cost a day of investigation: a colour
used as an **albedo** -- what fraction of the light arriving at a surface leaves
it again -- that exceeds one. The OpenPBR Playground's `paper` drives
`subsurface_color` through a `colorcorrect` node with `gain = 4`, and hdClaude's
thin subsurface lobe multiplies a path by exactly that colour at every scatter:
four per bounce, 4^32 at thirty-two bounces, and a frame of fireflies. hdClaude
clamps it (docs/roadmap.md, 2026-09-16) because an estimator that gains energy
does not converge; the authored value is still wrong, and this says so.

Usage: python check_scene_materials.py <stage.usd> [<stage.usd> ...]
Exit status is 1 when anything is reported, so it can gate a scene.
"""

import sys

from pxr import Usd, UsdShade

#: Inputs whose value is a reflectance, by the shader id that declares them.
#: Each must lie in [0, 1]: it is the fraction of arriving light that leaves.
ALBEDO_INPUTS = {
    "ND_open_pbr_surface_surfaceshader": (
        "base_color", "subsurface_color", "specular_color", "coat_color",
        "fuzz_color", "transmission_color"),
    "ND_standard_surface_surfaceshader": (
        "base_color", "subsurface_color", "specular_color", "coat_color",
        "sheen_color", "transmission_color"),
    "ND_translucent_bsdf": ("color",),
    "ND_subsurface_bsdf": ("color",),
    "ND_oren_nayar_diffuse_bsdf": ("color",),
    "ND_burley_diffuse_bsdf": ("color",),
    "ND_UsdPreviewSurface": ("diffuseColor", "specularColor"),
}

#: Nodes that scale what passes through them, and the input that does it.
GAIN_NODES = {
    "ND_colorcorrect_color3": "gain",
    "ND_colorcorrect_color4": "gain",
    "ND_multiply_color3": "in2",
    "ND_multiply_color3FA": "in2",
    "ND_multiply_color4": "in2",
}


def exceeds_one(value):
    """Whether an authored value has any component above one."""
    if value is None:
        return False
    try:
        return max(float(component) for component in value) > 1.0
    except TypeError:
        return float(value) > 1.0


def gain_above_one(shader):
    """The gain a scaling node applies, if it applies one above unity."""
    identifier = shader.GetIdAttr().Get()
    name = GAIN_NODES.get(identifier)
    if name is None:
        return None
    value = shader.GetInput(name).Get() if shader.GetInput(name) else None
    return value if exceeds_one(value) else None


def report_stage(path):
    stage = Usd.Stage.Open(path)
    if not stage:
        print(f"FAIL: could not open {path}")
        return 1

    findings = []
    for prim in stage.Traverse():
        shader = UsdShade.Shader(prim)
        if not shader:
            continue
        inputs = ALBEDO_INPUTS.get(shader.GetIdAttr().Get())
        if not inputs:
            continue
        for name in inputs:
            port = shader.GetInput(name)
            if not port:
                continue
            if exceeds_one(port.Get()):
                findings.append(
                    f"{prim.GetPath()}.{name} = {port.Get()} is a reflectance "
                    "above one")
                continue
            for source in port.GetConnectedSources()[0]:
                gain = gain_above_one(UsdShade.Shader(source.source.GetPrim()))
                if gain is not None:
                    findings.append(
                        f"{prim.GetPath()}.{name} is driven through "
                        f"{source.source.GetPath().name} with a gain of {gain}, "
                        "so the reflectance it carries can exceed one")

    print(f"{path}: {len(findings)} finding(s)")
    for finding in findings:
        print(f"  {finding}")
    return 1 if findings else 0


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    status = 0
    for path in sys.argv[1:]:
        status |= report_stage(path)
    return status


if __name__ == "__main__":
    sys.exit(main())
