"""The Cornell box, as Cornell measured it, and the USD stage built from it.

Every number below is copied from the Cornell Program of Computer Graphics'
"Cornell Box Data" page, which publishes the physical box's geometry, camera,
light and the measured reflectance spectra of its three paints:

    https://bowers.cornell.edu/computer-graphics/data#cornellbox

(formerly http://www.graphics.cornell.edu/online/box/data.html). Nothing here
is fitted, rounded or tuned; where the page is silent, the choice is named
where it is made:

- Lengths are millimetres. The page states no unit for the geometry, but the
  physical box is 55.9 cm deep and the camera's focal length (0.035) and film
  (0.025 x 0.025) are plainly metres -- a 35 mm lens and a 25 mm square CCD --
  which put the camera 800 mm in front of the box's open side, as the page's
  position (278, 273, -800) says. The stage is authored in millimetres with
  metersPerUnit = 0.001.
- The light's emission is published at four wavelengths with no unit. Its
  shape is what is rendered; its magnitude is the light's `intensity`, and
  hdClaude scales a tabulated emission to D65's luminous power
  (docs/spectral-rendering.md 6), so the unit cancels.
- The light's emission is authored through the draft UsdLux
  physical-lighting proposal's `PhysicalLightIlluminantAPI`
  (`physical:illuminant = "custom"`, `physical:customIlluminant`), which
  resamples it to 5 nm by ASTM E308 -- cubic Lagrange between the published
  four samples rather than straight lines. The paints' reflectances, which no
  proposal covers yet, are hdClaude's `hdclaude:spectrum:*`, linear between
  their samples. Both are zero outside their range; the reflectances stop at
  400 and 700 nm and so does the light, so nothing outside that range is lit.
- The page gives the light a "constant reflectance of 0.78". A UsdLux light is
  not a surface and reflects nothing, so the rendered light reflects nothing;
  `cornell_box_reference.py` measures what the 0.78 would add.
- The "holes" the page lists under the floor and ceiling are for its
  discontinuity mesher, with one exception: the ceiling's hole is where the
  light is, and the ceiling is built around it, since a ceiling through the
  light would be coplanar with it.

The colour matching functions, D65 and the integrals below are the ones
hdClaude's core uses (src/core/spectrum.cpp): Wyman, Sloan and Shirley's fit,
the CIE D65 table at 5 nm, and the same white adaptation. That makes the
authored colours exactly the ones the renderer projects its spectra to, and it
is what lets the reference in tests/usd/cornell_box_reference.py model the same
film the renderer has.

Usage: python cornell_box.py <output.usda>
Runs in the OpenUSD environment.
"""

import math
import os
import sys

# --- The published data ---------------------------------------------------------

CAMERA_POSITION = (278.0, 273.0, -800.0)
CAMERA_DIRECTION = (0.0, 0.0, 1.0)
CAMERA_UP = (0.0, 1.0, 0.0)
FOCAL_LENGTH_MM = 35.0          # published 0.035 m
FILM_MM = 25.0                  # published 0.025 m square

# Each surface is a list of quadrilaterals, vertices in the page's order.
FLOOR = [[(552.8, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 559.2), (549.6, 0.0, 559.2)]]
LIGHT = [(343.0, 548.8, 227.0), (343.0, 548.8, 332.0),
         (213.0, 548.8, 332.0), (213.0, 548.8, 227.0)]
CEILING_OUTLINE = [(556.0, 548.8, 0.0), (556.0, 548.8, 559.2),
                   (0.0, 548.8, 559.2), (0.0, 548.8, 0.0)]
BACK_WALL = [[(549.6, 0.0, 559.2), (0.0, 0.0, 559.2),
              (0.0, 548.8, 559.2), (556.0, 548.8, 559.2)]]
RIGHT_WALL = [[(0.0, 0.0, 559.2), (0.0, 0.0, 0.0),
               (0.0, 548.8, 0.0), (0.0, 548.8, 559.2)]]
LEFT_WALL = [[(552.8, 0.0, 0.0), (549.6, 0.0, 559.2),
              (556.0, 548.8, 559.2), (556.0, 548.8, 0.0)]]
SHORT_BLOCK = [
    [(130.0, 165.0, 65.0), (82.0, 165.0, 225.0), (240.0, 165.0, 272.0), (290.0, 165.0, 114.0)],
    [(290.0, 0.0, 114.0), (290.0, 165.0, 114.0), (240.0, 165.0, 272.0), (240.0, 0.0, 272.0)],
    [(130.0, 0.0, 65.0), (130.0, 165.0, 65.0), (290.0, 165.0, 114.0), (290.0, 0.0, 114.0)],
    [(82.0, 0.0, 225.0), (82.0, 165.0, 225.0), (130.0, 165.0, 65.0), (130.0, 0.0, 65.0)],
    [(240.0, 0.0, 272.0), (240.0, 165.0, 272.0), (82.0, 165.0, 225.0), (82.0, 0.0, 225.0)],
]
TALL_BLOCK = [
    [(423.0, 330.0, 247.0), (265.0, 330.0, 296.0), (314.0, 330.0, 456.0), (472.0, 330.0, 406.0)],
    [(423.0, 0.0, 247.0), (423.0, 330.0, 247.0), (472.0, 330.0, 406.0), (472.0, 0.0, 406.0)],
    [(472.0, 0.0, 406.0), (472.0, 330.0, 406.0), (314.0, 330.0, 456.0), (314.0, 0.0, 456.0)],
    [(314.0, 0.0, 456.0), (314.0, 330.0, 456.0), (265.0, 330.0, 296.0), (265.0, 0.0, 296.0)],
    [(265.0, 0.0, 296.0), (265.0, 330.0, 296.0), (423.0, 330.0, 247.0), (423.0, 0.0, 247.0)],
]

LIGHT_EMISSION = ([400.0, 500.0, 600.0, 700.0], [0.0, 8.0, 15.6, 18.4])
LIGHT_REFLECTANCE = 0.78        # published; not representable by a UsdLux light

# Measured reflectance, 400-700 nm every 4 nm: (wavelength, white, green, red).
_REFLECTANCE_TABLE = """
400 0.343 0.092 0.040
404 0.445 0.096 0.046
408 0.551 0.098 0.048
412 0.624 0.097 0.053
416 0.665 0.098 0.049
420 0.687 0.095 0.050
424 0.708 0.095 0.053
428 0.723 0.097 0.055
432 0.715 0.095 0.057
436 0.710 0.094 0.056
440 0.745 0.097 0.059
444 0.758 0.098 0.057
448 0.739 0.096 0.061
452 0.767 0.101 0.061
456 0.777 0.103 0.060
460 0.765 0.104 0.062
464 0.751 0.107 0.062
468 0.745 0.109 0.062
472 0.748 0.112 0.061
476 0.729 0.115 0.062
480 0.745 0.125 0.060
484 0.757 0.140 0.059
488 0.753 0.160 0.057
492 0.750 0.187 0.058
496 0.746 0.229 0.058
500 0.747 0.285 0.058
504 0.735 0.343 0.056
508 0.732 0.390 0.055
512 0.739 0.435 0.056
516 0.734 0.464 0.059
520 0.725 0.472 0.057
524 0.721 0.476 0.055
528 0.733 0.481 0.059
532 0.725 0.462 0.059
536 0.732 0.447 0.058
540 0.743 0.441 0.059
544 0.744 0.426 0.061
548 0.748 0.406 0.061
552 0.728 0.373 0.063
556 0.716 0.347 0.063
560 0.733 0.337 0.067
564 0.726 0.314 0.068
568 0.713 0.285 0.072
572 0.740 0.277 0.080
576 0.754 0.266 0.090
580 0.764 0.250 0.099
584 0.752 0.230 0.124
588 0.736 0.207 0.154
592 0.734 0.186 0.192
596 0.741 0.171 0.255
600 0.740 0.160 0.287
604 0.732 0.148 0.349
608 0.745 0.141 0.402
612 0.755 0.136 0.443
616 0.751 0.130 0.487
620 0.744 0.126 0.513
624 0.731 0.123 0.558
628 0.733 0.121 0.584
632 0.744 0.122 0.620
636 0.731 0.119 0.606
640 0.712 0.114 0.609
644 0.708 0.115 0.651
648 0.729 0.117 0.612
652 0.730 0.117 0.610
656 0.727 0.118 0.650
660 0.707 0.120 0.638
664 0.703 0.122 0.627
668 0.729 0.128 0.620
672 0.750 0.132 0.630
676 0.760 0.139 0.628
680 0.751 0.144 0.642
684 0.739 0.146 0.639
688 0.724 0.150 0.657
692 0.730 0.152 0.639
696 0.740 0.157 0.635
700 0.737 0.159 0.642
"""


def _reflectances():
    rows = [[float(x) for x in line.split()]
            for line in _REFLECTANCE_TABLE.strip().splitlines()]
    wavelengths = [row[0] for row in rows]
    return {
        "white": (wavelengths, [row[1] for row in rows]),
        "green": (wavelengths, [row[2] for row in rows]),
        "red": (wavelengths, [row[3] for row in rows]),
    }


REFLECTANCE = _reflectances()

# Which paint each surface is, as the page assigns them.
SURFACES = {
    "floor": ("white", FLOOR),
    "ceiling": ("white", None),          # built around the light; see ceiling_quads
    "backWall": ("white", BACK_WALL),
    "rightWall": ("green", RIGHT_WALL),
    "leftWall": ("red", LEFT_WALL),
    "shortBlock": ("white", SHORT_BLOCK),
    "tallBlock": ("white", TALL_BLOCK),
}


def ceiling_quads():
    """The ceiling with the light's rectangle left open: eight quads.

    The ceiling and the light are both rectangles at y = 548.8 aligned with the
    axes, so cutting one from the other is a 3 x 3 grid with its middle cell
    removed. Wound like the published ceiling, so its normal faces down into
    the box as the page's does.
    """
    xs = [556.0, 343.0, 213.0, 0.0]
    zs = [0.0, 227.0, 332.0, 559.2]
    y = 548.8
    quads = []
    for i in range(3):
        for j in range(3):
            if i == 1 and j == 1:
                continue
            x0, x1 = xs[i], xs[i + 1]
            z0, z1 = zs[j], zs[j + 1]
            quads.append([(x0, y, z0), (x0, y, z1), (x1, y, z1), (x1, y, z0)])
    return quads


def surface_quads(name):
    paint, quads = SURFACES[name]
    return paint, (ceiling_quads() if name == "ceiling" else quads)


# --- The spectral model hdClaude's core uses -------------------------------------
#
# Mirrors src/core/spectrum.cpp. A mirror rather than a binding because the
# reference must run without the renderer, and kept to the same grid, table and
# fit so that agreement between the two is agreement about transport.

LAMBDA_MIN = 360.0
LAMBDA_MAX = 830.0
INTEGRATION_STEP = 5.0

_D65_START = 300.0
_D65_STEP = 5.0
_D65 = [
    0.0341, 1.6643, 3.2945, 11.7652, 20.2360, 28.6447, 37.0535,
    38.5011, 39.9488, 42.4302, 44.9117, 45.7750, 46.6383, 49.3637,
    52.0891, 51.0323, 49.9755, 52.3118, 54.6482, 68.7015, 82.7549,
    87.1204, 91.4860, 92.4589, 93.4318, 90.0570, 86.6823, 95.7736,
    104.8650, 110.9360, 117.0080, 117.4100, 117.8120, 116.3360, 114.8610,
    115.3920, 115.9230, 112.3670, 108.8110, 109.0820, 109.3540, 108.5780,
    107.8020, 106.2960, 104.7900, 106.2390, 107.6890, 106.0470, 104.4050,
    104.2250, 104.0460, 102.0230, 100.0000, 98.1671, 96.3342, 96.0611,
    95.7880, 92.2368, 88.6856, 89.3459, 90.0062, 89.8026, 89.5991,
    88.6489, 87.6987, 85.4936, 83.2886, 83.4939, 83.6992, 81.8630,
    80.0268, 80.1207, 80.2146, 81.2462, 82.2778, 80.2810, 78.2842,
    74.0027, 69.7213, 70.6652, 71.6091, 72.9790, 74.3490, 67.9765,
    61.6040, 65.7448, 69.8856, 72.4863, 75.0870, 69.3398, 63.5927,
    55.0054, 46.4182, 56.6118, 66.8054, 65.0941, 63.3828, 63.8434,
    64.3040, 61.8779, 59.4519, 55.7054, 51.9590, 54.6998, 57.4406,
    58.8765, 60.3125,
]


def d65(wavelength):
    position = (wavelength - _D65_START) / _D65_STEP
    if position <= 0.0:
        return _D65[0]
    if position >= len(_D65) - 1:
        return _D65[-1]
    index = int(position)
    t = position - index
    return _D65[index] * (1.0 - t) + _D65[index + 1] * t


def _piecewise_gaussian(x, peak, low, high):
    t = (x - peak) / (low if x < peak else high)
    return math.exp(-0.5 * t * t)


def cie_xyz_bar(wavelength):
    """Wyman, Sloan and Shirley's multi-lobe fit, as CieXyzBar has it."""
    g = _piecewise_gaussian
    x = (1.056 * g(wavelength, 599.8, 37.9, 31.0) + 0.362 * g(wavelength, 442.0, 16.0, 26.7)
         - 0.065 * g(wavelength, 501.1, 20.4, 26.2))
    y = 0.821 * g(wavelength, 568.8, 46.9, 40.5) + 0.286 * g(wavelength, 530.9, 16.3, 31.1)
    z = 1.217 * g(wavelength, 437.0, 11.8, 36.0) + 0.681 * g(wavelength, 459.0, 26.0, 13.8)
    return (x, y, z)


def grid():
    """The 5 nm integration grid of the core: 360, 365, ... 830."""
    count = int(round((LAMBDA_MAX - LAMBDA_MIN) / INTEGRATION_STEP)) + 1
    return [LAMBDA_MIN + i * INTEGRATION_STEP for i in range(count)]


def tabulated(spectrum, wavelength):
    """Linear between samples and zero outside them: EvaluateTabulated."""
    xs, ys = spectrum
    if wavelength < xs[0] or wavelength > xs[-1]:
        return 0.0
    for i in range(1, len(xs)):
        if wavelength < xs[i]:
            t = (wavelength - xs[i - 1]) / (xs[i] - xs[i - 1])
            return ys[i - 1] + (ys[i] - ys[i - 1]) * t
    return ys[-1]


def xyz_to_linear_srgb(x, y, z):
    return (3.2404542 * x - 1.5371385 * y - 0.4985314 * z,
            -0.9692660 * x + 1.8760108 * y + 0.0415560 * z,
            0.0556434 * x - 0.2040259 * y + 1.0572252 * z)


def linear_srgb_to_xyz(r, g, b):
    return (0.4124564 * r + 0.3575761 * g + 0.1804375 * b,
            0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
            0.0193339 * r + 0.1191920 * g + 0.9503041 * b)


def _white_adaptation():
    """The diagonal that pins a perfect reflector to sRGB's white (WhiteAdaptation)."""
    normalisation = sum(d65(l) * cie_xyz_bar(l)[1] for l in grid()) * INTEGRATION_STEP
    raw = [0.0, 0.0, 0.0]
    for l in grid():
        bar = cie_xyz_bar(l)
        for c in range(3):
            raw[c] += bar[c] * d65(l)
    raw = [value * INTEGRATION_STEP / normalisation for value in raw]
    white = linear_srgb_to_xyz(1.0, 1.0, 1.0)
    return [white[c] / raw[c] for c in range(3)]


def reflectance_to_linear_srgb(spectrum):
    """TabulatedReflectanceToLinearSrgb: the colour a reflectance is under D65."""
    normalisation = 0.0
    xyz = [0.0, 0.0, 0.0]
    for l in grid():
        illuminant = d65(l)
        bar = cie_xyz_bar(l)
        normalisation += illuminant * bar[1]
        weight = tabulated(spectrum, l) * illuminant
        for c in range(3):
            xyz[c] += bar[c] * weight
    scale = _white_adaptation()
    return xyz_to_linear_srgb(*[xyz[c] / normalisation * scale[c] for c in range(3)])


def resample_to_5nm(spectrum):
    """ResampleTo5nm: the draft UsdLux proposal's "resampled to 5nm intervals
    according to ASTM-E308" -- Lagrange through the nearest samples, cubic in
    interior intervals and quadratic in the first and last, keeping the bounds,
    with any dip below zero clamped."""
    xs, ys = spectrum
    n = len(xs)

    def lagrange(first, count, at):
        total = 0.0
        for i in range(first, first + count):
            term = ys[i]
            for j in range(first, first + count):
                if j != i:
                    term *= (at - xs[j]) / (xs[i] - xs[j])
            total += term
        return total

    def evaluate(at):
        k = 0
        while k < n - 2 and xs[k + 1] <= at:
            k += 1
        if xs[k] == at:
            return ys[k]
        if n == 2:
            return lagrange(0, 2, at)
        if n == 3 or k == 0:
            return lagrange(0, 3, at)
        if k == n - 2:
            return lagrange(n - 3, 3, at)
        return lagrange(k - 1, 4, at)

    out_x, out_y = [xs[0]], [ys[0]]
    lam = math.floor(xs[0] / 5.0) * 5.0 + 5.0
    while lam < xs[-1]:
        if lam > xs[0]:
            out_x.append(lam)
            out_y.append(max(0.0, evaluate(lam)))
        lam += 5.0
    out_x.append(xs[-1])
    out_y.append(ys[-1])
    return (out_x, out_y)


def luminous_scale(spectrum):
    """TabulatedLuminousScale: what scales an emission to D65's luminous power."""
    reference = sum(d65(l) * cie_xyz_bar(l)[1] for l in grid()) * INTEGRATION_STEP
    own = sum(tabulated(spectrum, l) * cie_xyz_bar(l)[1] for l in grid()) * INTEGRATION_STEP
    return reference / own


# --- The stage ---------------------------------------------------------------------

# The light's intensity. The page's emission has no unit, and hdClaude scales a
# tabulated emission to D65's luminous power, so this is the light's luminance
# in the film's units: chosen once so that the box's white walls sit in the
# display's linear range at the gallery's exposure, and then left alone.
LIGHT_INTENSITY = 40.0


def write_stage(path, measured_paints=True):
    """Write the box to `path`.

    `measured_paints` False leaves the paints' spectra off, so they are only
    the colours those spectra are under D65 -- what a renderer without
    spectral input would be given. The reference test renders that too, as
    the control that shows it can tell the two apart.
    """
    from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt

    if os.path.exists(path):
        os.remove(path)
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 0.001)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    stage.GetRootLayer().documentation = (
        "The Cornell box as Cornell measured it: geometry, camera, light and "
        "the paints' reflectance spectra from "
        "https://bowers.cornell.edu/computer-graphics/data#cornellbox, in "
        "millimetres. Generated by scripts/cornell_box.py; edit that, not this.")

    # The camera looks along +z with +y up. A USD camera looks along its own
    # -z, so it is turned half a turn about y, which also sends its +x to the
    # world's -x: the red wall (x = 556) is on the left, as in the photograph.
    camera = UsdGeom.Camera.Define(stage, "/World/camera")
    camera.CreateFocalLengthAttr(FOCAL_LENGTH_MM * 10.0)        # tenths of a unit
    camera.CreateHorizontalApertureAttr(FILM_MM * 10.0)
    camera.CreateVerticalApertureAttr(FILM_MM * 10.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(10.0, 10000.0))
    camera.AddTransformOp().Set(Gf.Matrix4d(
        -1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, -1.0, 0.0,
        CAMERA_POSITION[0], CAMERA_POSITION[1], CAMERA_POSITION[2], 1.0))

    # One material per paint: a Lambertian MaterialX diffuse whose colour is
    # the paint's D65 projection -- what every renderer reads -- and whose
    # measured spectrum is hdClaude's own attribute beside it.
    materials = {}
    for paint, spectrum in REFLECTANCE.items():
        material = UsdShade.Material.Define(stage, f"/World/materials/{paint}")
        surface = UsdShade.Shader.Define(stage, f"/World/materials/{paint}/surface")
        surface.CreateIdAttr("ND_surface")
        diffuse = UsdShade.Shader.Define(stage, f"/World/materials/{paint}/diffuse")
        diffuse.CreateIdAttr("ND_oren_nayar_diffuse_bsdf")
        colour = reflectance_to_linear_srgb(spectrum)
        diffuse.CreateInput("color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*colour))
        diffuse.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.0)
        diffuse.CreateInput("weight", Sdf.ValueTypeNames.Float).Set(1.0)
        if measured_paints:
            diffuse.CreateInput("hdclaude:spectrum:color:wavelengths",
                                Sdf.ValueTypeNames.FloatArray).Set(Vt.FloatArray(spectrum[0]))
            diffuse.CreateInput("hdclaude:spectrum:color:values",
                                Sdf.ValueTypeNames.FloatArray).Set(Vt.FloatArray(spectrum[1]))
        diffuse_out = diffuse.CreateOutput("out", Sdf.ValueTypeNames.Token)
        surface.CreateInput("bsdf", Sdf.ValueTypeNames.Token).ConnectToSource(diffuse_out)
        surface_out = surface.CreateOutput("out", Sdf.ValueTypeNames.Token)
        material.CreateSurfaceOutput("mtlx").ConnectToSource(surface_out)
        materials[paint] = material

    for name in SURFACES:
        paint, quads = surface_quads(name)
        mesh = UsdGeom.Mesh.Define(stage, f"/World/box/{name}")
        points = []
        indices = []
        for quad in quads:
            for vertex in quad:
                indices.append(len(points))
                points.append(Gf.Vec3f(*vertex))
        mesh.CreatePointsAttr(points)
        mesh.CreateFaceVertexCountsAttr([4] * len(quads))
        mesh.CreateFaceVertexIndicesAttr(indices)
        # Flat quads, measured: there is no smooth surface to refine toward.
        mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
        mesh.CreateDoubleSidedAttr(True)
        UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(materials[paint])

    # The light: the published rectangle, 130 x 105 mm at y = 548.8, emitting
    # down. A RectLight emits along its own -z and spans its x and y, so it is
    # turned a quarter about x, which takes -z to -y and y to -z.
    xs = [vertex[0] for vertex in LIGHT]
    zs = [vertex[2] for vertex in LIGHT]
    light = UsdLux.RectLight.Define(stage, "/World/light")
    light.CreateWidthAttr(max(xs) - min(xs))
    light.CreateHeightAttr(max(zs) - min(zs))
    light.CreateColorAttr(Gf.Vec3f(1.0, 1.0, 1.0))
    light.CreateIntensityAttr(LIGHT_INTENSITY)
    light.CreateNormalizeAttr(False)
    # The measured emission through the draft UsdLux physical-lighting
    # proposal's PhysicalLightIlluminantAPI (docs/usd-proposals.md). The schema
    # is not registered yet, so it is applied by name and its attributes are
    # authored as the proposal declares them.
    light.GetPrim().AddAppliedSchema("PhysicalLightIlluminantAPI")
    light.GetPrim().CreateAttribute(
        "physical:illuminant", Sdf.ValueTypeNames.Token).Set("custom")
    light.GetPrim().CreateAttribute(
        "physical:customIlluminant", Sdf.ValueTypeNames.Float2Array
    ).Set(Vt.Vec2fArray([Gf.Vec2f(l, v) for l, v in zip(*LIGHT_EMISSION)]))
    light.AddTransformOp().Set(Gf.Matrix4d(
        1.0, 0.0, 0.0, 0.0,
        0.0, 0.0, -1.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.5 * (min(xs) + max(xs)), LIGHT[0][1], 0.5 * (min(zs) + max(zs)), 1.0))

    stage.GetRootLayer().Save()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    write_stage(os.path.abspath(sys.argv[1]))


if __name__ == "__main__":
    main()
