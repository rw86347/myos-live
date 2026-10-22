#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""write-release — /usr/lib/myos/release for a live or installed root.

    live/tools/write-release.py --kernel-version FILE --arch ARCH --out FILE
                               [--bootx64 FILE] [--font FILE] [--bootarea FILE]

The upgrade compares this file on the stick with the one on the disk
(docs/INSTALLER-PLAN.md, "Upgrading from the stick"). It is not under /etc,
so a later upgrade never treats it as a local change.
"""
import argparse
import hashlib
import os
import subprocess
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest(), os.path.getsize(path)


def git_id():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel-version", required=True)
    ap.add_argument("--arch", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bootx64")
    ap.add_argument("--font")
    ap.add_argument("--bootarea")
    a = ap.parse_args()
    lines = []
    with open(a.kernel_version) as f:
        lines.extend(l.rstrip("\n") for l in f if l.strip())
    kver = next((l.split("=", 1)[1] for l in lines if l.startswith("KERNEL_VERSION=")), "unknown")
    lines.append("SYSTEM_VERSION=%s" % kver)
    lines.append("ARCH=%s" % a.arch)
    lines.append("BUILD_DATE=%s" % time.strftime("%Y-%m-%d"))
    lines.append("GIT=%s" % git_id())
    for key, path in (("BOOTX64", a.bootx64), ("FONT", a.font), ("BOOTAREA", a.bootarea)):
        if path and os.path.isfile(path):
            digest, size = sha256(path)
            lines.append("%s_SHA256=%s" % (key, digest))
            lines.append("%s_SIZE=%d" % (key, size))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    tmp = a.out + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, a.out)


if __name__ == "__main__":
    main()
