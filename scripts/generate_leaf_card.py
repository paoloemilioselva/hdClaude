"""Writes gallery/textures/leaf_card.png, the mask the cut-out gallery scene uses.

A leaf on a transparent background, the way a leaf card is authored: colour in
RGB, the leaf's shape in alpha, and the card cut at an opacity threshold. Two
holes are bitten out of it so the cut-out is visible inside the leaf as well as
around it -- in the leaf itself and in its shadow.

Procedural rather than photographed, so the scene ships no third-party image
and the shape is exactly known: the alpha is 255 inside the outline and 0
outside, with no soft edge for a threshold to argue with.

    python scripts/generate_leaf_card.py
"""

import math
import os

from PIL import Image

WIDTH, HEIGHT = 256, 512
OUTPUT = os.path.join(os.path.dirname(__file__), "..", "gallery", "textures",
                      "leaf_card.png")


def inside_leaf(u, v):
    """Whether (u, v) in [0, 1]^2, v up the leaf, is on the leaf."""
    # A stem up the first eighth, then a blade that is widest a little below
    # the middle and comes to a point at the tip.
    if v < 0.12:
        return abs(u - 0.5) < 0.018
    t = (v - 0.12) / 0.86
    if t > 1.0:
        return False
    half_width = 0.44 * math.sin(math.pi * t ** 0.85) ** 0.9
    # A shallow serration along the edge.
    half_width *= 1.0 - 0.06 * (0.5 + 0.5 * math.sin(t * 60.0))
    if abs(u - 0.5) >= half_width:
        return False
    # Two bites.
    for cu, cv, r in ((0.66, 0.55, 0.075), (0.38, 0.78, 0.05)):
        if (u - cu) ** 2 + ((v - cv) * 0.5) ** 2 < r * r:
            return False
    return True


def leaf_colour(u, v):
    """sRGB colour: a green that lightens towards the tip, darker on the veins."""
    t = max(0.0, min(1.0, (v - 0.12) / 0.86))
    base = (46 + 40 * t, 112 + 50 * t, 34 + 12 * t)
    offset = u - 0.5
    midrib = abs(offset) < 0.012
    # Side veins leave the midrib at an angle, every so often up the blade.
    along = v - 0.9 * abs(offset)
    side_vein = abs(math.sin(along * 38.0)) < 0.08 and abs(offset) > 0.02
    shade = 0.62 if midrib else (0.8 if side_vein else 1.0)
    return tuple(int(c * shade) for c in base)


def main():
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    pixels = image.load()
    for y in range(HEIGHT):
        # PNG rows run top down; v runs bottom up.
        v = 1.0 - (y + 0.5) / HEIGHT
        for x in range(WIDTH):
            u = (x + 0.5) / WIDTH
            if inside_leaf(u, v):
                pixels[x, y] = leaf_colour(u, v) + (255,)
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    image.save(OUTPUT)
    print(os.path.normpath(OUTPUT))


if __name__ == "__main__":
    main()
