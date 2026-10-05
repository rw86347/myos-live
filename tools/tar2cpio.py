#!/usr/bin/env python3
"""tar2cpio.py — stream a tar to a newc cpio without unpacking on the Mac.

Linux trees have names that differ only in case. The Mac cannot unpack them
into a folder. The archive can still be rewritten file by file.

    python3 tools/tar2cpio.py in.tar out.cpio [--add dest=hostfile]...
"""
from __future__ import annotations

import argparse
import os
import stat
import tarfile
import time


def align4(n: int) -> int:
    return (n + 3) & ~3


def put_member(out, name: str, mode: int, uid: int, gid: int,
               mtime: int, data: bytes, ino: int) -> None:
    if name.startswith("/"):
        name = name[1:]
    raw = name.encode("utf-8") + b"\0"
    namesize = len(raw)
    hdr = (
        "070701"
        + f"{ino:08x}{mode:08x}{uid:08x}{gid:08x}{1:08x}{mtime:08x}"
        + f"{len(data):08x}{0:08x}{0:08x}{0:08x}{0:08x}{namesize:08x}{0:08x}"
    ).encode("ascii")
    out.write(hdr)
    out.write(raw)
    out.write(b"\0" * (align4(110 + namesize) - 110 - namesize))
    out.write(data)
    out.write(b"\0" * (align4(len(data)) - len(data)))


def tar_mode(ti: tarfile.TarInfo) -> int:
    perm = ti.mode & 0o7777
    if ti.isdir():
        return stat.S_IFDIR | perm
    if ti.issym():
        return stat.S_IFLNK | (perm or 0o777)
    if ti.isfifo():
        return stat.S_IFIFO | perm
    if ti.ischr():
        return stat.S_IFCHR | perm
    if ti.isblk():
        return stat.S_IFBLK | perm
    return stat.S_IFREG | (perm or 0o644)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("tar")
    ap.add_argument("cpio")
    ap.add_argument("--add", action="append", default=[],
                    help="archive_path=host_file (repeatable)")
    args = ap.parse_args()

    extras = []
    for item in args.add:
        if "=" not in item:
            raise SystemExit(f"bad --add {item!r}, want dest=host_file")
        dest, src = item.split("=", 1)
        extras.append((dest.lstrip("/"), src))

    ino = 1
    files = 0
    seen: dict[str, bytes] = {}
    now = int(time.time())

    with tarfile.open(args.tar, "r:") as tar, open(args.cpio + ".tmp", "wb") as out:
        for ti in tar:
            name = ti.name
            while name.startswith("./"):
                name = name[2:]
            if not name or name == ".":
                continue
            if ti.isdir():
                data = b""
            elif ti.issym():
                data = (ti.linkname or "").encode("utf-8")
            elif ti.islnk():
                target = ti.linkname or ""
                while target.startswith("./"):
                    target = target[2:]
                data = seen.get(target, b"")
            elif ti.isreg():
                fo = tar.extractfile(ti)
                data = fo.read() if fo else b""
                seen[name] = data
            else:
                data = b""
            put_member(out, name, tar_mode(ti), ti.uid, ti.gid,
                       int(ti.mtime or now), data, ino)
            ino += 1
            files += 1

        for dest, src in extras:
            with open(src, "rb") as f:
                data = f.read()
            mode = stat.S_IFREG | (os.stat(src).st_mode & 0o777)
            put_member(out, dest, mode, 0, 0, now, data, ino)
            ino += 1
            files += 1

        put_member(out, "TRAILER!!!", 0, 0, 0, 0, b"", ino)

    os.replace(args.cpio + ".tmp", args.cpio)
    print(f"{args.cpio}: {files} members, {os.path.getsize(args.cpio)} bytes")


if __name__ == "__main__":
    main()
