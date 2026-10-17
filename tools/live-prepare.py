#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""live-prepare — do the live disk's first boot on the build machine.

    tools/live-prepare.py IMAGE --debugfs PATH --e2fsck PATH \\
                          --ovmf-code PATH --ovmf-vars PATH [--timeout SECONDS]

A fresh live disk's first boot runs Alpine's install scripts (font cache,
icon caches, MIME database, GLib schemas...: /etc/myos/first-boot.sh), 10-15 s
in QEMU and minutes on a Pentium 4, on every stick and every install, and the
results are the same everywhere. So the image build does it once
(docs/performance.md, C1): this puts a marker in the image's root
(/etc/myos/first-boot.build), boots the image in QEMU with no screen, and
stage 1 (/etc/runit/1) runs the first boot, removes what must differ between
machines (/etc/machine-id, SSH host keys), deletes the marker and shuts down.

Then it checks, with Linux's own tools, that the first boot finished, that the
marker and the machine's own files are gone, and that the file system is
clean. If anything is wrong the image is deleted: an image that would power
off at its first real boot must never be shipped.
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile, time


def root_offset(img):
    """The byte offset of the GPT partition named myos-root."""
    with open(img, "rb") as f:
        f.seek(512)
        h = f.read(92)
        if h[:8] != b"EFI PART":
            sys.exit("live-prepare: %s has no GPT" % img)
        lba, n, size = struct.unpack_from("<QII", h, 72)
        f.seek(lba * 512)
        entries = f.read(n * size)
    for i in range(n):
        e = entries[i * size:(i + 1) * size]
        if e[56:128].decode("utf-16le").rstrip("\0") == "myos-root":
            return struct.unpack_from("<Q", e, 32)[0] * 512
    sys.exit("live-prepare: %s has no myos-root partition" % img)


def debugfs(tool, dev, cmds, write=False):
    with tempfile.NamedTemporaryFile("w", suffix=".cmds", delete=False) as f:
        f.write("\n".join(cmds) + "\n")
        path = f.name
    try:
        r = subprocess.run([tool] + (["-w"] if write else []) + ["-f", path, dev],
                           capture_output=True, text=True)
        return r.stdout + r.stderr
    finally:
        os.unlink(path)


def exists(tool, dev, path):
    return "File not found" not in debugfs(tool, dev, ["stat " + path])


def main():
    a = argparse.ArgumentParser()
    a.add_argument("image")
    a.add_argument("--debugfs", required=True)
    a.add_argument("--e2fsck", required=True)
    a.add_argument("--ovmf-code", required=True)
    a.add_argument("--ovmf-vars", required=True)
    a.add_argument("--timeout", type=int, default=600)
    o = a.parse_args()
    dev = "%s?offset=%d" % (o.image, root_offset(o.image))

    def fail(why):
        print("live-prepare: FAILED: %s; %s deleted" % (why, o.image), file=sys.stderr)
        os.unlink(o.image)
        sys.exit(1)

    # 1. The marker. (debugfs writes a file from the host: an empty one.)
    empty = tempfile.NamedTemporaryFile(delete=False)
    empty.close()
    debugfs(o.debugfs, dev, ["cd /etc/myos", "write %s first-boot.build" % empty.name],
            write=True)
    os.unlink(empty.name)
    if not exists(o.debugfs, dev, "/etc/myos/first-boot.build"):
        fail("couldn't put the marker in the image")

    # 2. The first boot, with no screen; stage 3 powers the machine off.
    scratch = tempfile.mkdtemp(prefix="live-prepare-")
    log = os.path.join(scratch, "serial.log")
    vars_fd = os.path.join(scratch, "vars.fd")
    shutil.copy(o.ovmf_vars, vars_fd)
    t0 = time.time()
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-machine", "q35,vmport=off,hpet=off", "-m", "2G",
         "-cpu", "qemu64", "-vga", "none", "-device", "VGA",
         "-drive", "if=pflash,format=raw,readonly=on,file=" + o.ovmf_code,
         "-drive", "if=pflash,format=raw,file=" + vars_fd,
         "-drive", "if=none,id=d0,format=raw,file=" + o.image,
         "-device", "ide-hd,drive=d0,bus=ide.0,bootindex=0",
         "-net", "none", "-no-reboot", "-display", "none",
         "-serial", "file:" + log, "-monitor", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        q.wait(timeout=o.timeout)
    except subprocess.TimeoutExpired:
        q.kill()
        q.wait()
        tail = open(log, errors="replace").read()[-600:] if os.path.exists(log) else ""
        shutil.rmtree(scratch, ignore_errors=True)
        fail("the first boot didn't finish in %d s:\n%s" % (o.timeout, tail))
    took = time.time() - t0
    said = open(log, errors="replace").read()
    shutil.rmtree(scratch, ignore_errors=True)

    # 3. Did it all happen, and nothing more?
    if "this image is ready to ship" not in said:
        fail("stage 1 never said it was done")
    if not exists(o.debugfs, dev, "/etc/myos/first-boot.done"):
        fail("the first boot didn't finish (no first-boot.done)")
    for path in ["/etc/myos/first-boot.build", "/etc/machine-id"]:
        if exists(o.debugfs, dev, path):
            fail("%s is still in the image" % path)
    if "ssh_host_" in debugfs(o.debugfs, dev, ["ls /etc/ssh"]):
        fail("SSH host keys are in the image")
    r = subprocess.run([o.e2fsck, "-fn", dev], capture_output=True, text=True)
    if r.returncode != 0:
        fail("e2fsck found damage:\n" + r.stdout[-800:])
    print("live-prepare: the first boot ran on the build machine (%.0f s in QEMU); "
          "the image is ready" % took)


if __name__ == "__main__":
    main()
