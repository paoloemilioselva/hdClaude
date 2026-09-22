"""Correct a Gaussian splat asset authored in the reference 3DGS convention.

The reference 3D Gaussian splatting implementation computes a particle's colour
as ``0.5 + Y(0,0) * f_dc``. USD specifies ``colour = Y(0,0) * c``, with no
offset -- the schema pins that by saying an absent coefficient array should
behave as "a SH coefficient corresponding to a DC signal of (0.5, 0.5, 0.5),
with degree 0", which with ``Y(0,0) = 1/(2 sqrt(pi))`` means ``c = sqrt(pi)``.

A converter that writes ``f_dc`` straight through therefore leaves every DC
coefficient short by ``sqrt(pi)``, and the asset's mean radiance comes out
negative. hdClaude reports that by name and renders the radiance as authored,
because a renderer that quietly added the offset would make a plausible picture
out of data that says something else -- and would corrupt correctly authored
assets while it was at it.

This script does the correction where it belongs: in the scene. It writes a new
layer that **sublayers the original and overrides nothing else**, so the
download is never edited and the correction can be read, reviewed and thrown
away.

Only the DC coefficient of each particle is offset. The higher bands describe
variation about the mean and are already in the right convention.

    python fix_splat_radiance_convention.py <input.usdc> <output.usda>

It refuses an asset whose radiance is already non-negative, because applying the
offset twice is the same class of error as not applying it once.
"""

import math
import sys

from pxr import Sdf, Usd, UsdVol, Vt


def corrected(stage, layer):
    """Author corrected coefficients for every splat prim, or return False."""
    y00 = 1.0 / (2.0 * math.sqrt(math.pi))
    offset = 0.5 / y00  # sqrt(pi)
    changed = False

    for prim in stage.Traverse():
        if not prim.IsA(UsdVol.ParticleField):
            continue
        splat = UsdVol.ParticleField3DGaussianSplat(prim)
        attribute = splat.GetRadianceSphericalHarmonicsCoefficientsAttr()
        coefficients = attribute.Get() if attribute else None
        if not coefficients:
            print(f"  {prim.GetPath()}: no float coefficients; skipped")
            continue

        positions = splat.GetPositionsAttr().Get()
        count = len(positions) if positions else 0
        if count == 0:
            print(f"  {prim.GetPath()}: no positions; skipped")
            continue
        degree = splat.GetRadianceSphericalHarmonicsDegreeAttr().Get()
        if degree is None:
            degree = 3
        stride = (degree + 1) ** 2
        if len(coefficients) < count * stride:
            print(f"  {prim.GetPath()}: {len(coefficients)} coefficients for "
                  f"{count} particles at degree {degree}; the schema discards a "
                  f"short array, so there is nothing to correct")
            continue

        negative = sum(1 for i in range(count) for c in coefficients[i * stride]
                       if c * y00 < 0.0)
        if negative == 0:
            print(f"  {prim.GetPath()}: no DC channel is negative; this asset "
                  f"does not need the offset and applying it would be the same "
                  f"mistake in the other direction")
            continue

        values = list(coefficients)
        for i in range(count):
            dc = values[i * stride]
            values[i * stride] = type(dc)(dc[0] + offset, dc[1] + offset,
                                          dc[2] + offset)

        with Usd.EditContext(stage, layer):
            over = stage.OverridePrim(prim.GetPath())
            target = over.CreateAttribute(attribute.GetName(),
                                          Sdf.ValueTypeNames.Float3Array)
            target.Set(Vt.Vec3fArray(values))
        print(f"  {prim.GetPath()}: {negative} of {count * 3} DC channels were "
              f"negative; sqrt(pi) folded into {count} DC coefficients")
        changed = True

    return changed


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    source, destination = argv[1], argv[2]

    stage = Usd.Stage.Open(source)
    if not stage:
        print(f"could not open {source}")
        return 1

    # A layer that sublayers the original rather than a copy of it. The download
    # stays exactly as downloaded, and what this adds is visible on its own.
    result = Sdf.Layer.CreateNew(destination)
    result.subLayerPaths.append(
        Sdf.ComputeAssetPathRelativeToLayer(result, source))
    over = Usd.Stage.Open(result)
    over.SetEditTarget(Usd.EditTarget(result))

    print(f"reading {source}")
    if not corrected(over, result):
        print("nothing to correct; no layer written")
        return 1

    default = stage.GetDefaultPrim()
    if default:
        result.defaultPrim = default.GetName()
    result.Save()
    print(f"wrote {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
