"""The Cornell box renders as a spectral radiosity solution says it must.

Every surface of the Cornell box is Lambertian and flat, and that is the one
case global illumination has an independent, deterministic answer for: the
radiosity equation, B = E + rho F B, which is what the box was built to
validate (Goral et al. 1984; Meyer et al. 1986). This test solves it from the
same published data the gallery scene is generated from
(scripts/cornell_box.py), with no code in common with the renderer, and compares
the two on the linear EXR.

The solve:

  geometry     the published quads, triangulated as Hydra triangulates a quad,
               each triangle cut into patches of about PATCH_MM on a side.
  coupling     point-to-triangle form factors in closed form (Lambert's contour
               integral), with a triangle that crosses the receiver's horizon
               clipped to it; the two blocks occlude, tested segment by segment
               from patch to patch. Nothing in the room is curved, so nothing
               else can.
  spectra      solved at every nanometre from 400 to 700 -- the light emits
               nothing outside that range -- with the paints' and the light's
               measured spectra exactly as the scene authors them, and the
               light scaled to D65's luminous power as hdClaude scales it.
  gather       the image is not read off the patches. Each pixel in a window is
               traced to its surface point and gathers from every patch through
               the same closed-form coupling, the final gather that removes the
               patches' piecewise-constant error from what is compared.
  film         CIE XYZ through the colour matching functions hdClaude's film
               interpolates, normalised by D65's luminous integral, then the
               sRGB matrix -- the film of docs/spectral-rendering.md 5.

The render runs at 64 bounces, the most the delegate allows, against an
infinite-bounce solution: the energy left after 64 reflections in a box whose
paints reflect at most 0.78 is far below everything else here. The light's
geometry is made visible, so one window looks straight at the emitter and
checks the tabulated emission with no transport in between.

Two things the physical box has that neither side models, and the solution
measures instead of ignoring:

  - the light's own reflectance of 0.78 (a UsdLux light reflects nothing);
  - bounces beyond the gallery's eight.

Usage: python cornell_box_reference.py <output directory> [--accept-tolerance X]
Runs in the OpenUSD environment and tests the plugin HDCLAUDE_PLUGIN_DIR names
-- the build tree's, under ctest -- or else what compile.py last installed.
"""

import math
import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import cornell_box as box  # noqa: E402

if os.name == "nt":
    RENDER = ["cmd", "/c", os.path.join(ROOT, "render_claude.bat")]
else:
    RENDER = ["bash", os.path.join(ROOT, "render_claude.sh")]

IMAGE = 256
SAMPLES = 2048
BOUNCES = 64
PATCH_MM = 60.0
LIGHT_PATCH_MM = 8.0
WAVELENGTHS = np.arange(400.0, 701.0, 1.0)

# Paint indices; the light is its own.
PAINTS = ["white", "green", "red"]
LIGHT = 3

# --- Geometry -------------------------------------------------------------------


def triangulate(quad):
    """A quad as Hydra triangulates it: (0, 1, 2) and (0, 2, 3)."""
    a, b, c, d = (np.asarray(v, dtype=np.float64) for v in quad)
    return [np.array([a, b, c]), np.array([a, c, d])]


def subdivide(triangle, size):
    """A triangle cut into n^2 similar ones, n chosen so no edge exceeds `size`."""
    a, b, c = triangle
    longest = max(np.linalg.norm(b - a), np.linalg.norm(c - b), np.linalg.norm(a - c))
    n = max(1, int(math.ceil(longest / size)))
    point = lambda i, j: a + (b - a) * (i / n) + (c - a) * (j / n)
    out = []
    for i in range(n):
        for j in range(n - i):
            out.append([point(i, j), point(i + 1, j), point(i, j + 1)])
            if i + j < n - 1:
                out.append([point(i + 1, j), point(i + 1, j + 1), point(i, j + 1)])
    return out


def clip_convex(polygon, a, b, keep_left):
    """Sutherland-Hodgman: the part of a convex polygon on one side of a->b (x, z)."""
    def side(p):
        return (b[0] - a[0]) * (p[2] - a[2]) - (b[2] - a[2]) * (p[0] - a[0])
    out = []
    for i, p in enumerate(polygon):
        q = polygon[(i + 1) % len(polygon)]
        sp, sq = side(p), side(q)
        inside_p = sp >= 0.0 if keep_left else sp <= 0.0
        inside_q = sq >= 0.0 if keep_left else sq <= 0.0
        if inside_p:
            out.append(p)
        if inside_p != inside_q:
            out.append(p + (q - p) * (sp / (sp - sq)))
    return out


def minus_footprint(polygon, footprint):
    """A convex floor piece minus a convex block footprint, as convex pieces.

    P - Q is the union, over Q's edges k, of P inside edges 0..k-1 and outside
    edge k: disjoint convex pieces, each clipped exactly.
    """
    # Orient the footprint counter-clockwise in (x, z).
    area = sum(p[0] * q[2] - q[0] * p[2]
               for p, q in zip(footprint, footprint[1:] + footprint[:1]))
    if area < 0.0:
        footprint = footprint[::-1]
    pieces = []
    remaining = polygon
    for k in range(len(footprint)):
        a, b = footprint[k], footprint[(k + 1) % len(footprint)]
        outside = clip_convex(remaining, a, b, keep_left=False)
        if len(outside) >= 3:
            pieces.append(outside)
        remaining = clip_convex(remaining, a, b, keep_left=True)
        if len(remaining) < 3:
            break
    return pieces


def floor_triangles():
    """The floor without the blocks' footprints.

    The page publishes the footprints as the floor's holes. The floor under a
    block is enclosed on every side and never lit or seen, so leaving it out
    is exact -- and keeping it would let a patch that straddles a footprint be
    counted beside the block face that hides half of it.
    """
    pieces = [[np.asarray(v, dtype=np.float64) for v in tri]
              for quad in box.FLOOR for tri in triangulate(quad)]
    for block in (box.SHORT_BLOCK, box.TALL_BLOCK):
        footprint = [np.asarray(v, dtype=np.float64) for v in block[0]]
        footprint = [np.array([v[0], 0.0, v[2]]) for v in footprint]
        pieces = [piece for polygon in pieces for piece in minus_footprint(polygon, footprint)]
    triangles = []
    for polygon in pieces:
        for i in range(1, len(polygon) - 1):
            tri = np.array([polygon[0], polygon[i], polygon[i + 1]])
            if np.linalg.norm(np.cross(tri[1] - tri[0], tri[2] - tri[0])) > 1e-6:
                triangles.append(tri)
    return triangles


BOX_CENTRE = np.array([278.0, 274.4, 279.6])


def surfaces(for_patches=False):
    """Every surface: name, paint index, triangles, and the side that faces in.

    Walls face the room's centre; a block's faces face away from the block's
    own centre. The scene is double sided, so this is only which side light
    arrives on, and in a box there is only one. `for_patches` gives the floor
    without the blocks' footprints, for the solve; the camera sees the floor
    as the scene authors it.
    """
    result = []
    for name in box.SURFACES:
        paint, quads = box.surface_quads(name)
        if name == "floor" and for_patches:
            triangles = floor_triangles()
        else:
            triangles = [t for quad in quads for t in triangulate(quad)]
        if name.endswith("Block"):
            centre = np.mean([v for quad in quads for v in quad], axis=0)
            centre = np.array([centre[0], 0.5 * max(v[1] for q in quads for v in q), centre[2]])
            outward = True
        else:
            centre = BOX_CENTRE
            outward = False
        result.append((name, PAINTS.index(paint), triangles, centre, outward))
    result.append(("light", LIGHT, triangulate(box.LIGHT), np.array([278.0, 0.0, 279.5]), False))
    return result


def facing_normal(triangle, centre, outward):
    n = np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0])
    n /= np.linalg.norm(n)
    towards = centre - triangle.mean(axis=0)
    if (np.dot(n, towards) > 0.0) == outward:
        n = -n
    return n


# --- Coupling --------------------------------------------------------------------


def form_factors(points, normals, triangles, triangle_normals):
    """F[i, j]: the form factor from the point i to triangle j, unoccluded.

    Lambert's closed form for a polygon seen from a differential area,
    F = |sum_k theta_k n . g_k| / (2 pi), valid for a polygon wholly above the
    point's horizon. A triangle that crosses the horizon is clipped to it
    first, which is rare -- a floor patch reaching under a block's face -- and
    done one at a time. A triangle facing away from the point couples nothing.
    """
    m, k = len(points), len(triangles)
    result = np.zeros((m, k))
    chunk = max(1, 400000 // max(k, 1))
    for start in range(0, m, chunk):
        x = points[start:start + chunk]                     # (c, 3)
        nx = normals[start:start + chunk]
        r = triangles[None, :, :, :] - x[:, None, None, :]  # (c, k, 3, 3)
        # The triangle's front must face the point, and the point's front the
        # triangle: heights of the vertices above the point's plane.
        height = np.einsum("ckvd,cd->ckv", r, nx)
        front = np.einsum("ckd,kd->ck", -r[:, :, 0, :], triangle_normals) > 1e-9
        above = (height >= -1e-9).all(axis=2)
        crosses = (height > 1e-9).any(axis=2) & ~above
        unit = r / np.linalg.norm(r, axis=3, keepdims=True)
        total = np.zeros(height.shape[:2])
        for e in range(3):
            a, b = unit[:, :, e, :], unit[:, :, (e + 1) % 3, :]
            ra, rb = r[:, :, e, :], r[:, :, (e + 1) % 3, :]
            theta = np.arccos(np.clip(np.einsum("ckd,ckd->ck", a, b), -1.0, 1.0))
            g = np.cross(ra, rb)
            length = np.linalg.norm(g, axis=2)
            g = g / np.maximum(length, 1e-30)[:, :, None]
            total += np.where(length > 1e-12, theta * np.einsum("ckd,cd->ck", g, nx), 0.0)
        f = np.abs(total) / (2.0 * math.pi)
        result[start:start + chunk] = np.where(front & above, f, 0.0)
        for ci, kj in zip(*np.nonzero(front & crosses)):
            result[start + ci, kj] = clipped_form_factor(x[ci], nx[ci], triangles[kj])
    return result


def clipped_form_factor(x, n, triangle):
    """The same closed form, for the part of a polygon above x's horizon."""
    polygon = []
    vertices = list(triangle)
    for i, v in enumerate(vertices):
        w = vertices[(i + 1) % len(vertices)]
        hv, hw = np.dot(v - x, n), np.dot(w - x, n)
        if hv >= 0.0:
            polygon.append(v)
        if (hv >= 0.0) != (hw >= 0.0):
            polygon.append(v + (w - v) * (hv / (hv - hw)))
    if len(polygon) < 3:
        return 0.0
    total = 0.0
    for i in range(len(polygon)):
        ra, rb = polygon[i] - x, polygon[(i + 1) % len(polygon)] - x
        la, lb = np.linalg.norm(ra), np.linalg.norm(rb)
        if la < 1e-12 or lb < 1e-12:
            continue
        theta = math.acos(max(-1.0, min(1.0, np.dot(ra, rb) / (la * lb))))
        g = np.cross(ra, rb)
        lg = np.linalg.norm(g)
        if lg > 1e-12:
            total += theta * np.dot(n, g / lg)
    return abs(total) / (2.0 * math.pi)


def occluded(origins, targets, occluders):
    """Whether the open segment from each origin to each target meets an occluder.

    `origins` (c, 3) against `targets` (k, 3): a (c, k) answer. Moller-Trumbore
    against every occluding triangle, with the ends excluded so a segment that
    starts or ends on a block face is not blocked by that face.
    """
    c, k = len(origins), len(targets)
    o = np.broadcast_to(origins[:, None, :], (c, k, 3)).reshape(-1, 3)
    d = (targets[None, :, :] - origins[:, None, :]).reshape(-1, 3)
    blocked = np.zeros(c * k, dtype=bool)
    for group in occluders:
        # Only the segments that reach the block's bounding box can meet it.
        low, high = group["low"], group["high"]
        with np.errstate(divide="ignore", invalid="ignore"):
            inverse = 1.0 / d
            t0 = (low - o) * inverse
            t1 = (high - o) * inverse
        near = np.nanmax(np.minimum(t0, t1), axis=1)
        far = np.nanmin(np.maximum(t0, t1), axis=1)
        candidates = np.nonzero((near <= far) & (far > 0.0) & (near < 1.0) & ~blocked)[0]
        if len(candidates) == 0:
            continue
        oc, dc = o[candidates], d[candidates]
        hit = np.zeros(len(candidates), dtype=bool)
        for tri in group["triangles"]:
            e1, e2 = tri[1] - tri[0], tri[2] - tri[0]
            p = np.cross(dc, e2)
            det = p @ e1
            ok = np.abs(det) > 1e-12
            inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
            s = oc - tri[0]
            u = (s * p).sum(axis=1) * inv
            q = np.cross(s, e1)
            v = (dc * q).sum(axis=1) * inv
            t = (q @ e2) * inv
            hit |= ok & (u >= 0.0) & (v >= 0.0) & (u + v <= 1.0) & (t > 1e-6) & (t < 1.0 - 1e-6)
        blocked[candidates[hit]] = True
    return blocked.reshape(c, k)


def visibility(points, normals, targets, target_normals, occluders):
    """1 where the segment from each point to each target is clear, else 0."""
    m = len(points)
    result = np.ones((m, len(targets)))
    origins = points + normals * 1e-3
    ends = targets + target_normals * 1e-3
    chunk = max(1, 300000 // max(len(targets), 1))
    for start in range(0, m, chunk):
        result[start:start + chunk] = ~occluded(origins[start:start + chunk], ends, occluders)
    return result


def partial_visibility(points, normals, triangles, triangle_normals, occluders):
    """The fraction of each triangle a point sees: four points of it, averaged.

    The centroid and the midpoints from it to each vertex -- one sample per
    quarter. Used where it matters, at the pixels the comparison reads, so a
    patch on a shadow's edge counts as part lit rather than as all or none.
    """
    centres = triangles.mean(axis=1)
    samples = [centres] + [0.5 * (centres + triangles[:, k, :]) for k in range(3)]
    return sum(visibility(points, normals, s, triangle_normals, occluders)
               for s in samples) / len(samples)


# --- Spectra and film -------------------------------------------------------------


def spectrum_on(spectrum, wavelengths):
    return np.array([box.tabulated(spectrum, float(l)) for l in wavelengths])


def emitted_radiance(wavelengths):
    """The light's spectral radiance as hdClaude emits it.

    intensity * colour * the tabulated spectrum scaled to D65's luminous power.
    The colour is white, which the upsampling turns into a flat spectrum.
    """
    scale = box.luminous_scale(box.LIGHT_EMISSION)
    return box.LIGHT_INTENSITY * scale * spectrum_on(box.LIGHT_EMISSION, wavelengths)


def film_matrix(wavelengths):
    """Spectral radiance on `wavelengths` (1 nm apart) to linear sRGB.

    The film integrates L(lambda) against the colour matching functions as
    hdClaude's film reads them -- sampled at 5 nm and interpolated linearly --
    and divides by D65's luminous integral on the same samples.
    """
    knots = np.array(box.grid())
    bars = np.array([box.cie_xyz_bar(l) for l in knots])
    rows = np.stack([np.interp(wavelengths, knots, bars[:, c]) for c in range(3)])
    luminous = sum(box.d65(l) * box.cie_xyz_bar(l)[1] for l in knots) * box.INTEGRATION_STEP
    step = wavelengths[1] - wavelengths[0]
    xyz = rows * step / luminous                                  # (3, w)
    to_rgb = np.array([[3.2404542, -1.5371385, -0.4985314],
                       [-0.9692660, 1.8760108, 0.0415560],
                       [0.0556434, -0.2040259, 1.0572252]])
    return to_rgb @ xyz                                           # (3, w)


# --- The solve ---------------------------------------------------------------------


def build():
    triangles, normals, paints, owners = [], [], [], []
    for index, (name, paint, tris, centre, outward) in enumerate(surfaces(True)):
        size = LIGHT_PATCH_MM if paint == LIGHT else PATCH_MM
        for tri in tris:
            n = facing_normal(tri, centre, outward)
            for sub in subdivide(tri, size):
                triangles.append(np.array(sub))
                normals.append(n)
                paints.append(paint)
                owners.append(index)
    gather_triangles = []          # as authored, for tracing the camera
    occluders = []                 # each block, with its bounding box
    for index, (name, paint, tris, centre, outward) in enumerate(surfaces()):
        for tri in tris:
            gather_triangles.append((tri, index, paint, facing_normal(tri, centre, outward)))
        if name.endswith("Block"):
            vertices = np.concatenate(tris)
            occluders.append({"triangles": tris, "low": vertices.min(axis=0) - 1e-6,
                              "high": vertices.max(axis=0) + 1e-6})
    return (np.array(triangles), np.array(normals), np.array(paints),
            np.array(owners), gather_triangles, occluders)


def reflectances(wavelengths, light_reflectance):
    table = np.zeros((4, len(wavelengths)))
    for i, paint in enumerate(PAINTS):
        table[i] = spectrum_on(box.REFLECTANCE[paint], wavelengths)
    table[LIGHT] = light_reflectance
    return table


def solve(F, paints, wavelengths, light_reflectance):
    rho = reflectances(wavelengths, light_reflectance)[paints]          # (n, w)
    emission = np.zeros_like(rho)
    emission[paints == LIGHT] = math.pi * emitted_radiance(wavelengths)
    B = emission.copy()
    for iteration in range(400):
        updated = emission + rho * (F @ B)
        change = np.max(np.abs(updated - B)) / max(np.max(np.abs(updated)), 1e-30)
        B = updated
        if change < 1e-10:
            return B, iteration
    raise SystemExit("FAIL: the radiosity iteration did not converge")


def camera_ray(px, py):
    u = (px + 0.5) / IMAGE
    v = (py + 0.5) / IMAGE
    direction = np.array([-(u - 0.5) * box.FILM_MM, (0.5 - v) * box.FILM_MM,
                          box.FOCAL_LENGTH_MM])
    return np.array(box.CAMERA_POSITION), direction / np.linalg.norm(direction)


def trace(origin, direction, gather_triangles):
    best, hit = math.inf, None
    for tri, index, paint, n in gather_triangles:
        e1, e2 = tri[1] - tri[0], tri[2] - tri[0]
        p = np.cross(direction, e2)
        det = np.dot(p, e1)
        if abs(det) < 1e-12:
            continue
        s = origin - tri[0]
        u = np.dot(s, p) / det
        q = np.cross(s, e1)
        v = np.dot(direction, q) / det
        t = np.dot(e2, q) / det
        if u >= 0 and v >= 0 and u + v <= 1 and 1e-6 < t < best:
            best, hit = t, (origin + t * direction, index, paint, n)
    return hit


# Windows, in pixels of the IMAGE x IMAGE frame: [x0, x1] x [y0, y1], inclusive,
# each wholly on one surface and clear of shadow edges, so a few pixels of
# jitter in where the renderer samples a pixel cannot move its mean.
WINDOWS = {
    "light":        ("light",      (118, 138, 33, 35)),
    "ceiling":      ("ceiling",    (50, 70, 18, 26)),
    "back wall":    ("backWall",   (110, 146, 62, 82)),
    "red wall":     ("leftWall",   (16, 28, 100, 140)),
    "green wall":   ("rightWall",  (228, 240, 100, 140)),
    "floor":        ("floor",      (40, 100, 230, 240)),
    "short top":    ("shortBlock", (140, 172, 168, 174)),
    "tall front":   ("tallBlock",  (84, 112, 120, 160)),
}


def gather(points_normals_paints, centres, triangles, normals, B, occluders,
           wavelengths, light_radiance):
    """Radiance at each window pixel: rho / pi times the irradiance gathered."""
    points = np.array([p for p, n, paint in points_normals_paints])
    point_normals = np.array([n for p, n, paint in points_normals_paints])
    paints = np.array([paint for p, n, paint in points_normals_paints])
    F = form_factors(points, point_normals, triangles, normals)
    F *= partial_visibility(points, point_normals, triangles, normals, occluders)
    rho = reflectances(wavelengths, 0.0)[paints]
    out = rho / math.pi * (F @ B)
    out[paints == LIGHT] = light_radiance
    return out


def reference(light_reflectance=0.0):
    started = time.time()
    triangles, normals, paints, owners, gather_triangles, occluders = build()
    centres = triangles.mean(axis=1)
    print(f"{len(triangles)} patches; coupling ...", flush=True)
    F = form_factors(centres, normals, triangles, normals)
    F *= partial_visibility(centres, normals, triangles, normals, occluders)
    rows = F.sum(axis=1)
    print(f"  form factors in {time.time() - started:.0f} s; a patch sees "
          f"{rows.min():.3f} to {rows.max():.3f} of its hemisphere in the room",
          flush=True)
    # A point's form factors to everything around it sum to at most one; past
    # one, something is counted twice. The slack is the binary visibility of a
    # patch half behind a block's edge.
    if rows.max() > 1.0 + 5e-3:
        raise SystemExit(f"FAIL: a patch's form factors sum to {rows.max():.4f} > 1")

    film = film_matrix(WAVELENGTHS)
    light_radiance = emitted_radiance(WAVELENGTHS)
    results = {}
    for reflectance in sorted({0.0, light_reflectance}):
        B, iterations = solve(F, paints, WAVELENGTHS, reflectance)
        print(f"  solved with the light reflecting {reflectance:g} in "
              f"{iterations} iterations", flush=True)
        means = {}
        for label, (surface, (x0, x1, y0, y1)) in WINDOWS.items():
            samples = []
            for py in range(y0, y1 + 1):
                for px in range(x0, x1 + 1):
                    hit = trace(*camera_ray(px, py), gather_triangles)
                    if hit is None or surfaces()[hit[1]][0] != surface:
                        raise SystemExit(
                            f"FAIL: window '{label}' pixel ({px}, {py}) does not "
                            f"see {surface}; the window is wrong for this camera")
                    samples.append((hit[0], hit[3], hit[2]))
            radiance = gather(samples, centres, triangles, normals, B, occluders,
                              WAVELENGTHS, light_radiance)
            means[label] = (film @ radiance.T).mean(axis=1)
        results[reflectance] = means
    print(f"  reference in {time.time() - started:.0f} s", flush=True)
    return results


# --- The render --------------------------------------------------------------------


def render(stage_path, output):
    env = dict(os.environ)
    env["HDCLAUDE_SAMPLES_PER_PIXEL"] = str(SAMPLES)
    env["HDCLAUDE_SAMPLES_PER_FRAME"] = "64"
    env["HDCLAUDE_MAX_BOUNCES"] = str(BOUNCES)
    env["HDCLAUDE_LIGHT_GEOMETRY"] = "1"
    result = subprocess.run(
        [*RENDER, "--camera", "/World/camera", "--frames", "1",
         "--imageWidth", str(IMAGE), stage_path, output.replace(".0001.", ".####.")],
        env=env, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(output):
        raise SystemExit(f"FAIL: render of {stage_path} failed:\n{result.stdout}\n{result.stderr}")
    log = result.stdout + result.stderr
    for line in log.splitlines():
        if "hdClaude" in line and ("Warning" in line or "not honoured" in line):
            raise SystemExit(f"FAIL: the render reported a problem: {line}")
    return log


def window_mean(image, x0, x1, y0, y1):
    tool = os.environ.get("HDCLAUDE_IMAGE_DIFF", "hdClaudeImageDiff")
    # Fractions that land on the pixel indices exactly (the tool floors).
    u0, u1 = (x0 + 0.5) / IMAGE, (x1 + 0.5) / IMAGE
    v0, v1 = (y0 + 0.5) / IMAGE, (y1 + 0.5) / IMAGE
    result = subprocess.run([tool, "--window", image, f"{u0}", f"{u1}", f"{v0}", f"{v1}"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: could not read {image}: {result.stderr}")
    green, red, blue = (float(x) for x in result.stdout.split()[:3])
    return np.array([red, green, blue])


# How far a window's mean may sit from the reference: each channel that carries
# at least a tenth of the window's brightest, relative to itself. A channel
# below that -- the blue of a red wall -- is a few thousandths in absolute
# terms and would measure only noise.
#
# The bound is what the two sides can each be trusted to, measured rather than
# assumed. The reference's patch discretisation moves no window by more than
# 0.27% between 80 mm and 60 mm patches; the render's Monte Carlo error over
# windows of 60 to 1200 pixels at 2048 samples is of the same order, and the
# first passing run sat within 0.6% everywhere. 1.5% leaves room for both and
# is still a quarter of what the control below moves.
TOLERANCE = 0.015
SIGNIFICANT = 0.1


def window_error(got, expected):
    significant = expected >= SIGNIFICANT * expected.max()
    return float(np.max(np.abs(got - expected)[significant] / expected[significant]))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        raise SystemExit(__doc__)
    out = os.path.abspath(args[0])
    os.makedirs(out, exist_ok=True)

    images = {}
    for variant, measured_paints in (("measured", True), ("rgb", False)):
        stage_path = os.path.join(out, f"cornell_box_{variant}.usda")
        box.write_stage(stage_path, measured_paints=measured_paints)
        images[variant] = os.path.join(out, f"cornell_box_{variant}.0001.exr")
        if os.path.exists(images[variant]):
            os.remove(images[variant])
        render(stage_path, images[variant])

    results = reference(box.LIGHT_REFLECTANCE)
    expected_by_window = results[0.0]
    reflecting = results[box.LIGHT_REFLECTANCE]

    failures = []
    control_errors = []
    print(f"\n{'window':<12} {'reference (linear sRGB)':>30} {'render':>30} {'error':>7} "
          f"{'as RGB':>7} {'+0.78 light':>12}")
    for label, (surface, pixels) in WINDOWS.items():
        expected = expected_by_window[label]
        got = window_mean(images["measured"], *pixels)
        error = window_error(got, expected)
        control = window_error(window_mean(images["rgb"], *pixels), expected)
        control_errors.append((control, label))
        luminance = 0.2126729 * expected[0] + 0.7151522 * expected[1] + 0.0721750 * expected[2]
        lift = (0.2126729 * reflecting[label][0] + 0.7151522 * reflecting[label][1]
                + 0.0721750 * reflecting[label][2]) / luminance - 1.0
        print(f"{label:<12} ({expected[0]:8.4f} {expected[1]:8.4f} {expected[2]:8.4f}) "
              f"({got[0]:8.4f} {got[1]:8.4f} {got[2]:8.4f}) {100 * error:6.2f}% "
              f"{100 * control:6.2f}% {100 * lift:+11.2f}%")
        if not error <= TOLERANCE:
            failures.append(f"{label}: {100 * error:.2f}% from the reference")

    # The control. The same box with each paint given only the colour its
    # spectrum is under D65 -- what a renderer without spectral input gets --
    # must miss the reference somewhere by more than the tolerance, or this
    # test could not tell measured spectra from RGB and would prove nothing
    # about them. Under D65 the two are metamers; under this light they are
    # not, and the difference is the point of the scene.
    worst, where = max(control_errors)
    print(f"\nThe paints as RGB miss by up to {100 * worst:.2f}% ({where}).")
    if not worst > 2.0 * TOLERANCE:
        failures.append(
            f"control: the paints as RGB come within {100 * worst:.2f}% of the "
            "reference, so this test cannot see whether their spectra are used")
    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("PASS: every window is within "
          f"{100 * TOLERANCE:g}% of the spectral radiosity solution")


if __name__ == "__main__":
    main()
