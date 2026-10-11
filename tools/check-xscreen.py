#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""check-xscreen.py — is X's login screen showing? (make test-uefi-login)

A text console (fbcon) uses a handful of colours. The login screen is drawn
over a photograph, which has thousands. So: count the distinct colours in a
QEMU `screendump` and fail below a threshold. This catches X failing to
start (the console stays on the screen) without knowing any pixel positions.

usage: check-xscreen.py screen.ppm [MIN_COLOURS]
"""
import sys


def read_ppm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:                      # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":           # a comment line
            pos = data.index(b"\n", pos)
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    if fields[0] != b"P6" or fields[3] != b"255":
        sys.exit("check-xscreen: not an 8-bit P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip().splitlines()[-1])
    need = int(sys.argv[2]) if len(sys.argv) == 3 else 1000
    w, h, px = read_ppm(sys.argv[1])
    colours = len({px[i:i + 3] for i in range(0, len(px), 3)})
    print(f"check-xscreen: {w}x{h}, {colours} colours (X needs {need})")
    sys.exit(0 if colours >= need else 1)


main()
