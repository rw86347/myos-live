#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""test-install — install MyOS from the live stick onto an empty disk in
QEMU, start the new disk without the stick, and check the result.

  live/tools/test-install.py --image LIVE_IMG --e2fsck PATH --work DIR
                        [--ovmf-code PATH --ovmf-vars PATH]
                        [--machine q35|p2max|pc] [--disk nvme|sata|usb|ide] [--keep]

What it does, as a person would (docs/INSTALLER-PLAN.md, "make test-install"):

1. Starts the live image as a USB stick (snapshot=on: the image is never
   written) with an empty 16 GiB disk on NVMe, SATA (AHCI) or USB.
2. Logs in on the serial console as the live user (an administrator, empty
   password) — the console reads the serial port as well as the keyboard —
   writes an installer plan for that disk, and runs
   `myos-install-helper --apply`, the program the installer window runs.
3. On the Mac: the new disk must hold a GPT with "EFI System", myos-root,
   myos-config and myos-data, and e2fsck must find both ext2 file systems
   clean.
4. Starts the new disk alone, logs in as the new user with the new password,
   and checks the system it finds: the user, the computer name, which
   partitions are / and /data, the kernel config partition, and that the
   live user and the installer are gone.

Machines: q35 and p2max (the GPD P2 Max's shape: no PS/2 controller, USB
keyboard and mouse) start the 64-bit stick by UEFI and install for UEFI
(a GPT). pc is an old PC: the 32-bit stick on the IDE bus, started by BIOS,
installing for BIOS (an MBR) onto an IDE or SATA disk; its CPU is a
Pentium III with SSE2, the feature set the Pentium 4 and M share.
The test account's name and password below exist only on the throwaway
disk in --work, which is deleted afterwards unless --keep.
"""
import argparse, os, re, shutil, socket, struct, subprocess, sys, time

# The QEMU machine driver is the kernel repo's (live/ is checked out inside it).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from qemu_serial import Failed, Machine, free_port   # noqa: E402

TEST_USER = "tester"
TEST_PASS = "install-test-5c1e"        # this test's own throwaway account
TEST_HOST = "myos-test"
TARGET_GIB = 16


def find_target(m):
    """The test's disk, by its size (its name depends on what else is
    plugged in: sdb next to the stick, sda alone)."""
    _, out = m.sh("for d in /sys/block/*; do echo ${d##*/} $(cat $d/size); done")
    disks = [l.split() for l in out.splitlines() if len(l.split()) == 2]
    found = [d for d, n in disks if n.isdigit() and int(n) == (TARGET_GIB << 30) // 512]
    if len(found) != 1:
        raise Failed("no single %d GiB disk in /sys/block:\n%s" % (TARGET_GIB, out))
    return found[0]


def mbr_parts(img):
    """[(active, type, byte offset, byte size)] of the image's 4 MBR entries."""
    with open(img, "rb") as f:
        mbr = f.read(512)
    if mbr[510:] != b"\x55\xaa":
        return []
    out = []
    for k in range(4):
        e = mbr[446 + 16 * k:462 + 16 * k]
        start, count = struct.unpack_from("<II", e, 8)
        out.append((e[0] == 0x80, e[4], start * 512, count * 512))
    return out


def gpt_parts(img):
    """{name: (byte offset, byte size)} from the image's GPT."""
    with open(img, "rb") as f:
        f.seek(512)
        h = f.read(92)
        if h[:8] != b"EFI PART":
            return {}
        lba, n, size = struct.unpack_from("<QII", h, 72)
        f.seek(lba * 512)
        raw = f.read(n * size)
    parts = {}
    for i in range(n):
        e = raw[i * size:(i + 1) * size]
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16le").rstrip("\0")
        if first:
            parts[name] = (first * 512, (last - first + 1) * 512)
    return parts


def main():
    sys.stdout.reconfigure(line_buffering=True)     # progress shows as it happens
    a = argparse.ArgumentParser()
    a.add_argument("--image", required=True)
    a.add_argument("--ovmf-code")
    a.add_argument("--ovmf-vars")
    a.add_argument("--e2fsck", required=True)
    a.add_argument("--work", required=True)
    a.add_argument("--disk", choices=["nvme", "sata", "usb", "ide"], default="nvme")
    a.add_argument("--machine", choices=["q35", "p2max", "pc"], default="q35")
    a.add_argument("--keep", action="store_true", help="keep the installed disk")
    o = a.parse_args()

    bios = o.machine == "pc"
    if bios != (o.disk == "ide") and not (bios and o.disk == "sata"):
        a.error("--machine pc takes --disk ide or sata; ide is only for --machine pc")
    if not bios and not (o.ovmf_code and o.ovmf_vars):
        a.error("--machine %s starts by UEFI: --ovmf-code and --ovmf-vars" % o.machine)
    name = "%s-%s" % (o.machine, o.disk)
    os.makedirs(o.work, exist_ok=True)
    target = os.path.join(o.work, "target-%s.img" % name)
    vars_fd = os.path.join(o.work, "vars-%s.fd" % name)
    with open(target, "wb") as f:
        f.truncate(TARGET_GIB << 30)        # sparse: costs only what is written

    disk = ["-drive", "if=none,format=raw,id=target,file=" + target]
    if bios:
        # SMM off for a SATA disk: SeaBIOS reaches its 32-bit AHCI code
        # through System Management Mode, and that hangs on the boot
        # loader's first read (docs/gaps.md, Boot). Real old PCs' SATA ROMs
        # are 16-bit code.
        base = ["qemu-system-i386", "-machine", "pc,smm=off" if o.disk == "sata" else "pc",
                "-m", "1024M", "-cpu", "pentium3,+sse2", "-net", "none"]
        stick = ["-drive", "if=none,format=raw,id=stick,snapshot=on,file=" + o.image,
                 "-device", "ide-hd,drive=stick,bus=ide.0,unit=0,bootindex=0"]
        disk += {"ide":  ["-device", "ide-hd,drive=target,bus=ide.1,unit=0"],
                 "sata": ["-device", "ahci,id=ahci", "-device", "ide-hd,drive=target,bus=ahci.0"]}[o.disk]
    else:
        shutil.copy(o.ovmf_vars, vars_fd)
        machine = "q35,i8042=off" if o.machine == "p2max" else "q35"
        base = ["qemu-system-x86_64", "-machine", machine, "-m", "4G", "-cpu", "qemu64",
                "-drive", "if=pflash,format=raw,readonly=on,file=" + o.ovmf_code,
                "-drive", "if=pflash,format=raw,file=" + vars_fd,
                "-device", "qemu-xhci,id=xhci", "-net", "none"]
        if o.machine == "p2max":
            base += ["-device", "usb-kbd,bus=xhci.0,usb_version=1", "-device", "usb-mouse,bus=xhci.0"]
        stick = ["-drive", "if=none,format=raw,id=stick,snapshot=on,file=" + o.image,
                 "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0"]
        disk += {"nvme": ["-device", "nvme,drive=target,serial=myos-target"],
                 "sata": ["-device", "ide-hd,drive=target,bus=ide.1"],
                 "usb":  ["-device", "usb-storage,bus=xhci.0,drive=target"]}[o.disk]
    results = []

    def ok(what, good, detail=""):
        results.append((what, good))
        print("  %s %s%s" % ("ok  " if good else "FAIL", what, (": " + detail) if detail and not good else ""))

    t0 = time.time()
    print("test-install (%s): installing from the stick" % name)
    m = Machine(base + stick + disk, os.path.join(o.work, "install-%s.log" % name))
    try:
        m.expect(rb"the login screen is up", 900, "the live system's login screen")
        m.login("myos", None)
        dev = "/dev/" + find_target(m)
        ok("the empty disk is seen (%s)" % dev, True)
        plan = "\n".join([
            "# myos-install plan v1", "lang=en_US.UTF-8", "lang_name=English (US)", "cjk=0",
            "keyboard=us", "keyboard_name=English (US)", "timezone=America/Denver", "ntp=1",
            "hwclock=utc", "disk=" + dev, "disk_name=test disk",
            "disk_bytes=%d" % (TARGET_GIB << 30), "free_bytes=%d" % (TARGET_GIB << 30),
            "mode=erase", "keep_esp=0", "root_gb=8", "separate_home=1", "encrypt=0",
            "fullname=Install Tester", "user=" + TEST_USER, "password=" + TEST_PASS,
            "hostname=" + TEST_HOST, "ask_password=1", "guest=0", "copy_wifi=0",
            "wifi_ssid=", "compositor=none", "compositor_ms=0.00",
            "firmware=" + ("bios" if bios else "uefi")])
        # Written as the installer window writes it: private from the start.
        m.sh("umask 077; rm -f /tmp/ti.plan; cat > /tmp/ti.plan <<'PLAN_END'\n%s\nPLAN_END" % plan)
        t1 = time.time()
        rc, out = m.sh("/usr/libexec/myos-install-helper --apply /tmp/ti.plan", 3600, check=False)
        steps = [l[5:] for l in out.splitlines() if l.startswith("step ")]
        ok("the helper installs (%s; %.0f s)" % (" ".join(steps), time.time() - t1),
           rc == 0 and "\nok" in "\n" + out, out[-1500:])
        if rc:
            _, log = m.sh("tail -40 /tmp/myos-install.log", check=False)
            print(log)
            raise Failed("the install failed")
        m.sh("rm -f /tmp/ti.plan; sync")
    finally:
        m.quit()

    # 3. The new disk, from the Mac.
    if bios:
        mbr = mbr_parts(target)
        types = [(act, typ) for act, typ, _, _ in mbr]
        ok("its MBR: boot area (7f, active), myos-root (83), myos-config (da), myos-data (83)",
           types == [(True, 0x7F), (False, 0x83), (False, 0xDA), (False, 0x83)] and mbr[0][2] == 512,
           str(mbr))
        parts = {"myos-root": mbr[1][2:], "myos-data": mbr[3][2:]} if len(mbr) == 4 else {}
        area = mbr_parts(o.image)[0][3]     # the stick's boot area, all of it
        with open(target, "rb") as t, open(o.image, "rb") as s:
            code_ok = t.read(446) == s.read(446)
            t.seek(512), s.seek(512)
            area_ok = t.read(area) == s.read(area)
        ok("the stick's boot code and boot area (%d KiB) are on it" % (area >> 10), code_ok and area_ok)
    else:
        parts = gpt_parts(target)
        want = ["EFI System", "myos-root", "myos-config", "myos-data"]
        ok("its partitions: " + ", ".join(want), all(p in parts for p in want), str(sorted(parts)))
    for p in ("myos-root", "myos-data"):
        if p in parts:
            r = subprocess.run([o.e2fsck, "-fn", "%s?offset=%d" % (target, parts[p][0])],
                               capture_output=True, text=True)
            ok("e2fsck: %s is clean" % p, r.returncode == 0, r.stdout[-600:])

    # 4. The new disk alone.
    print("test-install (%s): starting the installed disk" % name)
    if not bios:
        shutil.copy(o.ovmf_vars, vars_fd)   # a machine that never saw the stick
    boot = disk[:]
    if bios and o.disk == "ide":
        boot[-1] = "ide-hd,drive=target,bus=ide.0,unit=0"   # where a BIOS looks first
    boot[-1] += ",bootindex=0"
    m = Machine(base + boot, os.path.join(o.work, "installed-%s.log" % name))
    try:
        m.expect(rb"the login screen is up", 300, "the installed system's login screen")
        ok("the installed disk starts to the login screen", True)
        m.login(TEST_USER, TEST_PASS)
        ok("the new account logs in with its password", True)
        _, out = m.sh("id; cat /etc/hostname")
        ok("the account is an administrator (wheel)", "(wheel)" in out, out)
        ok("the computer name", TEST_HOST in out.split(), out)
        devname = find_target(m)
        _, out = m.sh("awk '$2==\"/\" || $2==\"/data\" {print $1, $2}' /proc/mounts")
        mounts = dict(reversed(l.split()) for l in out.splitlines() if len(l.split()) == 2)
        ok("/ is the new myos-root (%s)" % mounts.get("/"), devname in mounts.get("/", ""), out)
        ok("/data is the new myos-data (%s)" % mounts.get("/data"), devname in mounts.get("/data", ""), out)
        _, out = m.sh("dmesg | grep -i 'kconfig' | tail -3", check=False)
        ok("the kernel read the new myos-config partition", ("kconfig: %s" % devname) in out, out)
        rc, out = m.sh("grep -c '^myos:' /etc/passwd", check=False)
        ok("the live user is gone", out.strip() == "0", out)
        rc, _ = m.sh("test ! -e /usr/bin/myos-install", check=False)
        ok("the installer is gone", rc == 0)
        rc, out = m.sh("ls -ld /home; ls /home", check=False)
        ok("/home holds the new user", TEST_USER in out, out)
    finally:
        m.quit()

    if not o.keep:
        os.unlink(target)
    if os.path.exists(vars_fd):
        os.unlink(vars_fd)
    bad = [w for w, g in results if not g]
    print("test-install (%s): %s in %.0f s" % (name, "FAILED: " + "; ".join(bad) if bad else "OK",
                                               time.time() - t0))
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failed as e:
        print("test-install: FAILED: %s" % e)
        sys.exit(1)
