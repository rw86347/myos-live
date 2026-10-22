#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""write-stick — write a live image onto a USB stick, keeping its myos-data.

    sudo live/tools/write-stick.py [IMAGE] [--disk diskN] [--yes] [--no-verify]
    live/tools/write-stick.py IMAGE --target FILE [--size GiB]    (a test: a file as the stick,
                                                                   or an hdiutil-attached /dev/rdiskN)

IMAGE defaults to build/x86_64/myos-live-uefi.img.

A plain `dd` of the image replaces everything the image covers, and the
image covers the home folders: every rewrite of the stick lost the
desktop's settings (~/.config: the scale, the mouse, WritePerfect's font
size) and the files on the Desktop. Now the stick has a fourth partition,
myos-data, after the image, which no image ever reaches:

    0 ............ ~4.4 GB      8 GiB ...................... end of the stick
    | EFI | myos-root | config |  (unused)  | myos-data: /home, Wi-Fi networks |

It always starts at DATA_START (8 GiB), so it is found again even after a
plain dd lost it from the partition table: its ext2 superblock sits at the
same place, with the label "myos-data". This script writes the image, puts
myos-data back into both tables (the image's MBR is a hybrid one with no
0xEE entry, so MyOS's kernel reads the MBR, while firmware and the
installer read the GPT), and makes the file system the first time only.
The kernel mounts it at /data (the root's own disk's myos-data, as on an
installed system) and /etc/runit/1 moves /home onto it
(/etc/myos/stick-data.sh).

Then everything the image covers is read back and compared: a bad write
once left a stick with a damaged root (docs/LOG.md, 2026-10-21).

macOS: run with sudo (the stick is written as a raw disk). The stick must
be an external, physical disk; it asks before writing unless --yes.
"""
import argparse
import hashlib
import os
import plistlib
import struct
import subprocess
import sys
import uuid
import zlib

SECTOR = 512
DATA_START = 8 << 30 >> 9               # sectors: 8 GiB, past any image we build
DATA_MIN = 1 << 30 >> 9                 # a stick with less room than 1 GiB gets none
CHUNK = 4 << 20
LINUX_GUID = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
MKE2FS = "/opt/homebrew/opt/e2fsprogs/sbin/mke2fs"


def die(msg):
    sys.exit("write-stick: " + msg)


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def pread(fd, off, n):
    data = os.pread(fd, n, off)
    if len(data) != n:
        die(f"short read at byte {off}")
    return data


def pwrite(fd, off, data):
    if os.pwrite(fd, data, off) != len(data):
        die(f"short write at byte {off}")


# --- the stick ------------------------------------------------------------------

def diskutil_plist(verb, *args):
    out = subprocess.run(["diskutil", verb, "-plist", *args], check=True, capture_output=True).stdout
    return plistlib.loads(out)


def describe(disk):
    """Name, size in bytes, and whether it is an external physical disk."""
    info = diskutil_plist("info", disk)
    if info.get("DeviceBlockSize", SECTOR) != SECTOR:
        die(f"{disk} has {info.get('DeviceBlockSize')}-byte sectors; MyOS reads 512")
    external = not info.get("Internal", True) and not info.get("VirtualOrPhysical") == "Virtual"
    return info.get("MediaName", "?").strip(), info["TotalSize"], external


def pick_disk(name):
    if name:
        disk = name.removeprefix("/dev/").removeprefix("r")
        if not disk.startswith("disk"):
            die(f"{name}: not a disk name like disk4")
        return disk
    found = diskutil_plist("list", "external", "physical").get("WholeDisks", [])
    if len(found) != 1:
        listing = ", ".join(found) or "none"
        die(f"plug in exactly one USB stick, or say which with --disk (external disks: {listing})")
    return found[0]


# --- partition tables -------------------------------------------------------------

def gpt_of(head):
    """The GPT in the first 34 sectors of a disk or image: (header, entries) or None."""
    hdr = head[SECTOR:2 * SECTOR]
    if hdr[:8] != b"EFI PART":
        return None
    elba, nents, entsz = struct.unpack_from("<QII", hdr, 72)
    if elba != 2 or entsz != 128 or nents * entsz > 32 * SECTOR:
        die("the image's GPT is not the usual layout (entries at LBA 2, 128 x 128 bytes)")
    return hdr, bytearray(head[2 * SECTOR:2 * SECTOR + nents * entsz])


def gpt_last_used(entries):
    last = 0
    for k in range(0, len(entries), 128):
        e = entries[k:k + 128]
        if any(e[:16]):
            last = max(last, struct.unpack_from("<Q", e, 40)[0])
    return last


def mbr_last_used(mbr):
    last = 0
    for i in range(4):
        typ, start, count = mbr[446 + 16 * i + 4], *struct.unpack_from("<II", mbr, 446 + 16 * i + 8)
        if typ and count:
            last = max(last, start + count - 1)
    return last


def new_tables(head, disk_sectors, data_sectors):
    """The image's first 34 sectors with myos-data added, and the backup GPT
    (entries, then header) for the end of the disk."""
    head = bytearray(head)
    # MBR: the fourth slot. Not bootable; CHS "too big", as mkuefi.py writes.
    slot = 446 + 3 * 16
    if head[slot + 4]:
        die("the image's MBR already uses its fourth slot")
    head[slot:slot + 16] = struct.pack("<BBBBBBBBII", 0x00, 0xFE, 0xFF, 0xFF,
                                       0x83, 0xFE, 0xFF, 0xFF, DATA_START,
                                       min(data_sectors, 0xFFFFFFFF))
    gpt = gpt_of(head)
    if not gpt:
        return bytes(head), None
    hdr, entries = gpt
    nents = len(entries) // 128
    free = 3                            # the MBR's slot too: sda4 either way
    if nents <= free or any(entries[128 * free:128 * free + 16]):
        die("the image's GPT already uses its fourth entry")
    entries[128 * free:128 * free + 128] = (
        LINUX_GUID.bytes_le + uuid.uuid4().bytes_le
        + struct.pack("<QQQ", DATA_START, DATA_START + data_sectors - 1, 0)
        + "myos-data".encode("utf-16le").ljust(72, b"\0"))
    ent_sectors = (len(entries) + SECTOR - 1) // SECTOR
    entries = bytes(entries).ljust(ent_sectors * SECTOR, b"\0")
    last = disk_sectors - 1
    disk_guid = hdr[56:72]

    def header(current, backup, entries_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x00010000, 92, 0, 0,
                         current, backup, 34, last - 1 - ent_sectors)
        h[56:72] = disk_guid
        struct.pack_into("<QII", h, 72, entries_lba, nents, 128)
        struct.pack_into("<I", h, 88, crc32(entries[:nents * 128]))
        struct.pack_into("<I", h, 16, crc32(bytes(h[:92])))
        return bytes(h)

    head[SECTOR:2 * SECTOR] = header(1, last, 2)
    head[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    return bytes(head), entries + header(last, 1, last - ent_sectors)


def data_fs(fd):
    """What is at DATA_START now: ("myos-data", bytes it covers), ("other", 0)
    for some other ext2, or (None, 0) for no ext2 at all."""
    sb = pread(fd, DATA_START * SECTOR + 1024, 1024)
    if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
        return None, 0
    label = sb[120:136].split(b"\0")[0].decode("latin-1")
    blocks, log_bs = struct.unpack_from("<I", sb, 4)[0], struct.unpack_from("<I", sb, 24)[0]
    return ("myos-data" if label == "myos-data" else "other"), blocks * (1024 << log_bs)


# --- writing --------------------------------------------------------------------

def copy_image(fd, image, size, skip):
    """The image from byte `skip` on, to the same place on the stick."""
    done, step = skip, 256 << 20
    with open(image, "rb") as src:
        src.seek(skip)
        while done < size:
            buf = src.read(CHUNK)
            pwrite(fd, done, buf)
            done += len(buf)
            if done % step < len(buf) or done == size:
                print(f"\r  written {done >> 20} of {size >> 20} MiB", end="", flush=True)
    print()


def verify(fd, image, size, skip):
    a, b = hashlib.sha256(), hashlib.sha256()
    done = skip
    with open(image, "rb") as src:
        src.seek(skip)
        while done < size:
            buf = src.read(CHUNK)
            a.update(buf)
            b.update(pread(fd, done, len(buf)))
            done += len(buf)
            if done % (256 << 20) < len(buf) or done == size:
                print(f"\r  read back {done >> 20} of {size >> 20} MiB", end="", flush=True)
    print()
    if a.digest() != b.digest():
        die("the stick does not hold what was written: try again, or another stick")


def mke2fs(dev, data_sectors):
    """ext2 with the features MyOS's ext2 reads and writes, as the installer
    makes myos-data (myos-install-helper.c): 4 KiB blocks, one inode per
    256 KiB (each is zeroed: fewer make it quicker on a big stick)."""
    if not os.access(MKE2FS, os.X_OK):
        die(f"{MKE2FS} not found: brew install e2fsprogs")
    blocks = data_sectors * SECTOR // 4096
    subprocess.run([MKE2FS, "-q", "-F", "-t", "ext2", "-O", "none,filetype,sparse_super,large_file",
                    "-b", "4096", "-i", "262144", "-L", "myos-data",
                    "-E", f"offset={DATA_START * SECTOR},nodiscard", dev, str(blocks)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", default="build/x86_64/myos-live-uefi.img")
    ap.add_argument("--disk", help="the stick, as diskutil names it (disk4)")
    ap.add_argument("--target", help="a file standing in for the stick (tests)")
    ap.add_argument("--size", type=int, default=16, help="--target's size in GiB if it is new")
    ap.add_argument("--yes", action="store_true", help="don't ask")
    ap.add_argument("--no-verify", action="store_true", help="skip reading it back")
    a = ap.parse_args()

    size = os.path.getsize(a.image)
    if size % SECTOR or size < 34 * SECTOR:
        die(f"{a.image}: not a disk image")
    with open(a.image, "rb") as f:
        head = f.read(34 * SECTOR)
    gpt = gpt_of(head)
    image_end = max(size // SECTOR, mbr_last_used(head) + 1, gpt_last_used(gpt[1]) + 1 if gpt else 0)
    if image_end > DATA_START:
        die(f"the image reaches {image_end * SECTOR >> 20} MiB, past myos-data's start at "
            f"{DATA_START * SECTOR >> 20} MiB: writing it would overwrite the home folders")

    if a.target and a.target.startswith("/dev/"):  # e.g. a disk image attached with hdiutil
        dev, name = a.target, a.target
        disk_bytes = describe(a.target.replace("/rdisk", "/disk"))[1]
    elif a.target:
        dev, name = a.target, a.target
        if not os.path.exists(dev):
            with open(dev, "wb") as f:
                f.truncate(a.size << 30)
        disk_bytes = os.path.getsize(dev)
    else:
        if os.geteuid() != 0:
            die("run it with sudo: the stick is written as a raw disk")
        disk = pick_disk(a.disk)
        media, disk_bytes, external = describe(disk)
        if not external:
            die(f"{disk} ({media}) is not an external physical disk")
        dev, name = f"/dev/r{disk}", f"{disk} ({media}, {disk_bytes / 1e9:.1f} GB)"
    disk_sectors = disk_bytes // SECTOR

    fd = os.open(dev, os.O_RDWR)
    try:
        room = disk_sectors - 34 - DATA_START
        kind, fs_bytes = data_fs(fd) if room >= DATA_MIN else (None, 0)
        if room < DATA_MIN:
            plan = "no room for myos-data (it needs a stick over 9 GiB): home stays on the system"
        elif kind == "myos-data":
            if fs_bytes > room * SECTOR:
                die("the myos-data on it is bigger than the room it has: not this stick's?")
            plan = f"keeps myos-data ({fs_bytes / 1e9:.1f} GB: home folders, Wi-Fi networks)"
        elif kind == "other":
            die(f"there is a file system at {DATA_START * SECTOR >> 30} GiB that is not myos-data: "
                "not touching it")
        else:
            plan = f"makes myos-data ({room * SECTOR / 1e9:.1f} GB, empty: the first time)"
        print(f"Writing {a.image} ({size >> 20} MiB) to {name}.")
        print(f"Everything on it before {DATA_START * SECTOR >> 30} GiB is replaced; it {plan}.")
        if not a.yes and input("Go ahead? [y/N] ").strip().lower() != "y":
            die("nothing written")
        if not a.target:
            subprocess.run(["diskutil", "unmountDisk", dev.replace("/rdisk", "/disk")], check=True)

        # The image without its first 34 sectors, then the tables: a stop in
        # between leaves the old tables, which still say where everything is.
        copy_image(fd, a.image, size, 34 * SECTOR)
        if room >= DATA_MIN:
            new_head, backup = new_tables(head, disk_sectors, room)
            if backup:
                pwrite(fd, (disk_sectors - len(backup) // SECTOR) * SECTOR, backup)
        else:
            new_head = head
        pwrite(fd, 0, new_head)
        os.fsync(fd)
        if room >= DATA_MIN and kind is None:
            os.close(fd)
            fd = -1
            print("Making the myos-data file system...")
            mke2fs(dev, room)
            fd = os.open(dev, os.O_RDWR)
        if not a.no_verify:
            verify(fd, a.image, size, 34 * SECTOR)
            if pread(fd, 0, 34 * SECTOR) != new_head:
                die("the stick's partition table did not read back as written")
        if room >= DATA_MIN and data_fs(fd)[0] != "myos-data":
            die("no myos-data file system after writing")
    finally:
        if fd >= 0:
            os.close(fd)
    if not a.target:
        subprocess.run(["sync"])
        subprocess.run(["diskutil", "eject", dev.replace("/rdisk", "/disk")])
    print("Done: the stick is ready.")


if __name__ == "__main__":
    main()
