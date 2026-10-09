#!/usr/bin/env python3
"""make-wastebasket-icons.py — the Recycle Bin's two pictures, empty and
full, from the photos in assets/themes/golden-hour/wastebasket-*-source.jpg
(chosen by Rodger, 2026-10-08).

The sources are JPEGs with a checkerboard painted in where transparency
would be (JPEG has none), and the basket is wire mesh, so the checkerboard
shows through it too. This works out the checkerboard exactly — its two
colours, square size and offset, from a clean corner — and makes every pixel
that matches it transparent, with soft edges (the alpha follows how far the
pixel is from the checkerboard) and without a pale halo (the colour of a
half-transparent pixel has the checkerboard taken back out of it). Then it
crops to the basket and writes square PNGs.

    tools/make-wastebasket-icons.py      writes live/overlay/usr/share/myos/icons/
                                         wastebasket-empty.png, wastebasket-full.png
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "assets/themes/golden-hour")
OUT = os.path.join(ROOT, "live/overlay/usr/share/myos/icons")
SIZE = 256


def checkerboard(img):
    """The two colours, the square size and the offset, from the top-left
    120 x 120 pixels (background only in both photos)."""
    corner = img[:120, :120]
    lum = corner.mean(axis=2)
    light = lum > lum.mean()
    # the run length along the first row is the square size
    row = light[2]
    edges = np.flatnonzero(row[1:] != row[:-1])
    size = int(np.median(np.diff(edges)))
    # offset: shift so a square starts at 0
    off_x = (edges[0] + 1) % size
    col = light[:, 2]
    ey = np.flatnonzero(col[1:] != col[:-1])
    off_y = (ey[0] + 1) % size
    ys, xs = np.mgrid[0:120, 0:120]
    parity = (((xs - off_x) // size + (ys - off_y) // size) % 2).astype(bool)
    # which parity is the light one, and the two colours (medians: robust to JPEG noise)
    c0 = np.median(corner[~parity], axis=0)
    c1 = np.median(corner[parity], axis=0)
    return size, off_x, off_y, c0, c1


def inside(alpha, close=21):
    """The basket's whole outline, filled: what is solidly basket, with gaps
    up to `close` px closed (dilate, then erode) and the holes filled (all
    that a flood fill from the border cannot reach)."""
    from PIL import ImageDraw, ImageFilter
    m = Image.fromarray(((alpha > 0.35) * 255).astype(np.uint8), "L")
    m = m.filter(ImageFilter.MaxFilter(close)).filter(ImageFilter.MinFilter(close))
    m = m.point(lambda v: 255 if v > 127 else 0)
    out = m.copy()
    ImageDraw.floodfill(out, (0, 0), 128, thresh=0)        # outside the outline
    return np.asarray(out) != 128


def cut_out(path, solid):
    img = np.asarray(Image.open(path).convert("RGB")).astype(np.float32)
    h, w, _ = img.shape
    size, ox, oy, c0, c1 = checkerboard(img)
    ys, xs = np.mgrid[0:h, 0:w]
    parity = (((xs - ox) // size + (ys - oy) // size) % 2).astype(bool)
    bg = np.where(parity[..., None], c1, c0)
    # Near a square's edge JPEG blurs the two colours together: accept either
    # colour there (the smaller difference).
    other = np.where(parity[..., None], c0, c1)
    d = np.minimum(np.abs(img - bg).max(axis=2), np.abs(img - other).max(axis=2) + 2)
    lo, hi = 10.0, 48.0                 # within lo: background; past hi: the basket
    alpha = np.clip((d - lo) / (hi - lo), 0, 1)
    alpha[alpha < 0.12] = 0             # faint watermark streaks: background
    if solid:
        # Full: white paper is the checkerboard's white, so colour can't tell
        # them apart. Inside the outline everything is basket (the paper
        # fills it), kept as the photo has it.
        fill = inside(alpha)
        alpha = np.where(fill, 1.0, alpha)
    # Un-mix: a pixel that is part basket, part checkerboard holds
    # a*colour + (1 - a)*bg; take the background back out.
    a3 = np.maximum(alpha[..., None], 1e-3)
    colour = np.clip((img - (1 - alpha[..., None]) * bg) / a3, 0, 255)
    if solid:
        colour = np.where(fill[..., None], img, colour)
    rgba = np.dstack([colour, alpha * 255]).astype(np.uint8)
    # Crop to what is left (ignoring specks), square it, scale it.
    solid = alpha > 0.5
    rows = np.flatnonzero(solid.sum(axis=1) > 3)
    cols = np.flatnonzero(solid.sum(axis=0) > 3)
    top, bottom, left, right = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
    im = Image.fromarray(rgba).crop((left, top, right, bottom))
    side = max(im.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(im, ((side - im.size[0]) // 2, side - im.size[1]))  # standing on the bottom
    return square.resize((SIZE, SIZE), Image.LANCZOS), size


def main():
    os.makedirs(OUT, exist_ok=True)
    for kind in ("empty", "full"):
        src = os.path.join(SRC, "wastebasket-%s-source.jpg" % kind)
        icon, sq = cut_out(src, solid=kind == "full")
        dst = os.path.join(OUT, "wastebasket-%s.png" % kind)
        icon.save(dst, optimize=True)
        print("%s: %dx%d (checker squares %d px)" % (os.path.relpath(dst, ROOT), SIZE, SIZE, sq))
    return 0


if __name__ == "__main__":
    sys.exit(main())
