#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""make-worldmap.py LAND.geojson OUT.h — the Earth's land as a C table, for
the installer's time zone map (live/myos-install/worldmap.h).

LAND.geojson is Natural Earth's 1:110m land, public domain:
  https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_land.geojson
(naturalearthdata.com). The header is checked in, so building needs no network;
run this again only to change the map.

Points that move a coastline by less than TOLERANCE degrees are dropped
(Douglas-Peucker): at the map's size that is less than a pixel, even on a
HiDPI screen. Antarctica is cut off at LAT_BOTTOM, a little south of the
southernmost time zone (Vostok station, 78.5°S): below that the map would
only stretch empty ice across the whole width.
"""
import json
import sys

TOLERANCE = 0.2         # degrees
LAT_TOP, LAT_BOTTOM = 84, -84   # the map's edges: Greenland's north; room below Vostok's dot


def simplify(pts, tol):
    """Douglas-Peucker: keep the point farthest from the line between the
    ends if it is more than tol away, and do the same to each half."""
    if len(pts) < 3:
        return pts
    (ax, ay), (bx, by) = pts[0], pts[-1]
    dx, dy = bx - ax, by - ay
    length = (dx * dx + dy * dy) ** 0.5
    far, at = -1.0, 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        if length:
            d = abs(dy * (px - ax) - dx * (py - ay)) / length
        else:
            d = ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
        if d > far:
            far, at = d, i
    if far <= tol:
        return [pts[0], pts[-1]]
    return simplify(pts[:at + 1], tol)[:-1] + simplify(pts[at:], tol)


def main():
    src, out = sys.argv[1], sys.argv[2]
    rings = []
    for f in json.load(open(src))["features"]:
        g = f["geometry"]
        for poly in [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]:
            rings += poly       # outer outlines and any holes alike: drawn even-odd

    kept = []
    for r in rings:
        r = r[:-1] if r[0] == r[-1] else r      # GeoJSON repeats the first point
        r = [(lon, max(lat, LAT_BOTTOM)) for lon, lat in r]     # Antarctica's cut
        half = len(r) // 2                      # a closed outline has no ends:
        s = simplify(r[:half + 1], TOLERANCE)[:-1] + simplify(r[half:] + r[:1], TOLERANCE)[:-1]
        s = [(round(lon * 10), round(lat * 10)) for lon, lat in s]
        if len(s) >= 3:
            kept.append(s)

    npts = sum(map(len, kept))
    with open(out, "w") as h:
        h.write(f"""/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * worldmap.h — the Earth's land, for the installer's time zone map.
 *
 * Made by tools/make-worldmap.py from Natural Earth's 1:110m land
 * (public domain, naturalearthdata.com); don't edit, run that again.
 * Antarctica is cut off at {-LAT_BOTTOM}°S, and points that move a coastline
 * by less than {TOLERANCE} degrees are dropped: {len(kept)} outlines, {npts} points.
 *
 * Outline k is the next WORLD_RING[k] points of WORLD_PTS, closed (the last
 * joins the first). A point is {{ longitude, latitude }} in tenths of a
 * degree, east and north positive. Outlines may be holes (a lake): fill
 * them even-odd. The map shows latitudes WORLD_LAT_TOP to WORLD_LAT_BOTTOM.
 */
#ifndef MYOS_WORLDMAP_H
#define MYOS_WORLDMAP_H

#define WORLD_LAT_TOP     {LAT_TOP}
#define WORLD_LAT_BOTTOM  ({LAT_BOTTOM})

static const unsigned short WORLD_RING[{len(kept)}] = {{
""")
        lens = [str(len(s)) for s in kept]
        for i in range(0, len(lens), 16):
            h.write("    " + ", ".join(lens[i:i + 16]) + ",\n")
        h.write(f"}};\n\nstatic const short WORLD_PTS[{npts}][2] = {{\n")
        for s in kept:
            cells = [f"{{{lon},{lat}}}" for lon, lat in s]
            for i in range(0, len(cells), 8):
                h.write("    " + ",".join(cells[i:i + 8]) + ",\n")
        h.write("};\n\n#endif\n")
    print(f"{out}: {len(kept)} outlines, {npts} points")


if __name__ == "__main__":
    main()
