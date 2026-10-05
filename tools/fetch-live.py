#!/usr/bin/env python3
"""fetch-live.py — gather the software for the MyOS live disk.

    tools/fetch-live.py            build from live/packages.lock (exact files)
    tools/fetch-live.py --update   re-resolve live/packages.txt against Alpine's
                                   index and write a new lock file
    tools/fetch-live.py --sysroot  only unpack the packages' headers and
                                   libraries into build/live/sysroot, to
                                   cross-compile MyOS's own programs against
    tools/fetch-live.py --login-root
                                   a smaller archive (base+init+desktop+compositor)
                                   for the live UEFI RAM boot, when USB storage
                                   is not in the kernel yet
    tools/fetch-live.py --arch x86|x86_64
                                   Alpine architecture. Default x86 (i386).
                                   x86_64 merges live/packages.x86_64.txt on
                                   top of packages.txt and uses
                                   live/packages.x86_64.lock.

Steps:
  1. Read live/packages.txt: the programs we want, in groups.
  2. Resolve the libraries they need from Alpine 3.20's package index
     (main + community, 32-bit x86), including "so:" (shared library) and
     "cmd:" (command) dependencies, the way apk does it.
  3. Download every .apk into build/ext/live/apk/ and check its SHA-256
     against live/packages.lock: a file that doesn't match is refused, so
     every build gets exactly the same bytes (as tools/fetch-alpine.sh).
  4. Write the whole root file system into ONE archive, build/live/rootfs.tar:
     the packages' files, apk's database (lib/apk/db/installed) so apk on
     MyOS knows what is installed, the MyOS source in usr/src/myos, and our
     own files from live/overlay on top, then the programs the Makefile
     built for the live disk (build/live/extra: myos-login, myos-menu).
  5. Print the size of each group.

Why an archive and not a folder: the Mac's disk ignores upper/lower case,
and Linux packages have names that differ only in case (terminfo has both
"A/" and "a/"). Unpacking here would silently merge them. The archive also
keeps every file owned by root, which a copy made by the Mac user cannot.

Alpine's install scripts (.post-install, triggers) can't run on a Mac. They
go into etc/myos/first-boot/ in install order; /etc/myos/first-boot.sh
(called by runit's stage 1, /etc/runit/1) runs them on MyOS's first boot.

The live user (LIVE_USER) is written into /etc/passwd, group and shadow here,
with a home made from /etc/skel, the way image builders bake in a default
user: useradd on MyOS can't lock /etc/group yet (it needs hard links).
"""
import base64
import gzip
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORLD = os.path.join(ROOT, "live", "packages.txt")
LOCK = os.path.join(ROOT, "live", "packages.lock")
OVERLAY = os.path.join(ROOT, "live", "overlay")
CACHE = os.path.join(ROOT, "build", "ext", "live")
APKDIR = os.path.join(CACHE, "apk")
OUT = os.path.join(ROOT, "build", "live", "rootfs.tar")
LOGIN_OUT = os.path.join(ROOT, "build", "live", "login-root.tar")
EXTRA = os.path.join(ROOT, "build", "live", "extra")
SYSROOT = os.path.join(ROOT, "build", "live", "sysroot")
WORLD_FILES = [WORLD]
ARCH_ALIASES = {"i386": "x86", "x86": "x86", "x86_64": "x86_64", "amd64": "x86_64"}
# RAM login (live UEFI when the USB ext2 root is invisible): enough to
# start X and myos-login. Office, Firefox, gcc and the source stay on disk.
LOGIN_GROUPS = frozenset({"base", "init", "desktop", "compositor"})
LOGIN_EXTRA_SKIP = ("usr/share/writeperfect", "usr/lib/writeperfect")

BRANCH = "v3.20"
ARCH = "x86"
MIRROR = os.environ.get("LIVE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
REPOS = ["main", "community"]
# packages.txt's [build-only] group: unpacked into the sysroot for compiling
# MyOS's own programs, never put on the disk. Only the packages named, not
# their dependencies (gtk+3.0-dev's tree is 260 MiB, LLVM included; the
# headers it needs are a few MiB).
BUILD_ONLY = "build-only"

# The live disk's user: no password (PAM's nullok lets an empty one in), in
# wheel so `doas` works. The installer must ask for a real password.
LIVE_USER = dict(name="myos", uid=1000, gecos="MyOS user", shell="/bin/bash",
                 groups=["wheel", "audio", "video", "input"])
KEEP = ("etc/passwd", "etc/group", "etc/shadow")   # Root keeps a copy of these


def die(msg):
    sys.exit(f"fetch-live: {msg}")


def parse_arch(argv):
    """Pull --arch NAME out of argv. Returns (alpine_arch, leftover)."""
    arch, out, i = "x86", [], 0
    while i < len(argv):
        if argv[i] == "--arch":
            if i + 1 >= len(argv):
                die("--arch needs x86 or x86_64")
            name = argv[i + 1]
            if name not in ARCH_ALIASES:
                die(f"unknown --arch {name} (want x86 / i386 / x86_64)")
            arch = ARCH_ALIASES[name]
            i += 2
            continue
        out.append(argv[i])
        i += 1
    return arch, out


def configure(alpine_arch):
    """Set ARCH, lock path, world files, apk cache. x86 keeps the paths
    that already exist so a fetch does not re-download the 32-bit tree."""
    global ARCH, LOCK, WORLD_FILES, APKDIR, OUT, LOGIN_OUT, SYSROOT, EXTRA
    ARCH = alpine_arch
    if alpine_arch == "x86":
        LOCK = os.path.join(ROOT, "live", "packages.lock")
        WORLD_FILES = [WORLD]
        APKDIR = os.path.join(CACHE, "apk")
        OUT = os.path.join(ROOT, "build", "live", "rootfs.tar")
        LOGIN_OUT = os.path.join(ROOT, "build", "live", "login-root.tar")
        SYSROOT = os.path.join(ROOT, "build", "live", "sysroot")
        EXTRA = os.path.join(ROOT, "build", "live", "extra")
    else:
        LOCK = os.path.join(ROOT, "live", f"packages.{alpine_arch}.lock")
        WORLD_FILES = [WORLD, os.path.join(ROOT, "live", f"packages.{alpine_arch}.txt")]
        APKDIR = os.path.join(CACHE, f"apk-{alpine_arch}")
        OUT = os.path.join(ROOT, "build", "live", f"rootfs-{alpine_arch}.tar")
        LOGIN_OUT = os.path.join(ROOT, "build", "live", f"login-root-{alpine_arch}.tar")
        SYSROOT = os.path.join(ROOT, "build", "live", f"sysroot-{alpine_arch}")
        EXTRA = os.path.join(ROOT, "build", "live", f"extra.{alpine_arch}")


def fetch(url, dest):
    """curl, giving up on a stalled transfer after 30 s and trying again:
    the mirror sometimes stops sending in the middle of a file."""
    tmp = dest + ".tmp"
    for attempt in range(5):
        r = subprocess.run(["curl", "-fsSL", "--http1.1", "--connect-timeout", "20",
                            "--speed-limit", "1024", "--speed-time", "30",
                            "-o", tmp, url])
        if r.returncode == 0:
            os.replace(tmp, dest)
            return
        time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"curl failed 5 times on {url}")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --- 1. what we want ----------------------------------------------------------

def read_world_file(path, groups, group_map, forbidden):
    """Append one world file: new groups, extra names in an existing
    group, or !name to drop a package on this architecture."""
    current, lineno = None, 0
    for raw in open(path):
        lineno += 1
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("!"):
            forbidden.add(line[1:])
            continue
        m = re.fullmatch(r"\[(\w[\w-]*)\]", line)
        if m:
            name = m.group(1)
            if name in group_map:
                current = group_map[name]
            else:
                current = []
                groups.append((name, current))
                group_map[name] = current
            continue
        if current is None:
            die(f"{path}:{lineno}: '{line}' comes before the first [group]")
        if line not in current:
            current.append(line)


def read_world():
    """Returns ([(group, [names])], {forbidden names})."""
    groups, group_map, forbidden = [], {}, set()
    for path in WORLD_FILES:
        if not os.path.exists(path):
            die(f"missing {os.path.relpath(path, ROOT)}")
        read_world_file(path, groups, group_map, forbidden)
    for g, names in groups:
        names[:] = [n for n in names if n not in forbidden]
    return groups, forbidden


# --- 2. resolve against Alpine's index ----------------------------------------

def load_index():
    """Every package in main + community: name -> fields (lists for D/p)."""
    pkgs = {}
    os.makedirs(CACHE, exist_ok=True)
    for repo in REPOS:
        path = os.path.join(CACHE, f"APKINDEX-{repo}.tar.gz" if ARCH == "x86"
                            else f"APKINDEX-{ARCH}-{repo}.tar.gz")
        if not os.path.exists(path) or time.time() - os.path.getmtime(path) > 86400:
            print(f"Downloading the {repo} package index...")
            fetch(f"{MIRROR}/{BRANCH}/{repo}/{ARCH}/APKINDEX.tar.gz", path)
        with tarfile.open(path) as t:
            text = t.extractfile("APKINDEX").read().decode()
        for block in text.split("\n\n"):
            rec = {}
            for line in block.splitlines():
                if len(line) > 2 and line[1] == ":":
                    rec[line[0]] = line[2:]
            if "P" in rec:
                rec["repo"] = repo
                rec["D"] = rec.get("D", "").split()
                rec["p"] = rec.get("p", "").split()
                pkgs.setdefault(rec["P"], rec)     # main wins over community
    return pkgs


def dep_name(tok):
    return re.split(r"[<>=~]", tok, 1)[0]


def resolve(groups, pkgs, forbidden):
    """Returns [(group, name)] in install order: every package after the
    packages it depends on, each listed once under the first group that
    needs it."""
    providers = {}
    for p in pkgs.values():
        for tok in p["p"]:
            providers.setdefault(dep_name(tok), []).append(p)

    wanted = {n for _, names in groups for n in names}
    chosen = {}         # name -> group
    order = []          # install order (dependencies first)
    notes = []

    def pick(dep, wanted_by):
        if dep in pkgs:
            return pkgs[dep]
        cands = providers.get(dep)
        if not cands:
            die(f"nothing provides '{dep}' (needed by {wanted_by})")
        for c in cands:
            if c["P"] in chosen:
                return c
        # As apk: a provider named in packages.txt wins (dash-binsh over
        # busybox-binsh for /bin/sh); then never a forbidden one; then the
        # highest provider_priority.
        named = [c for c in cands if c["P"] in wanted]
        if named:
            return named[0]
        allowed = [c for c in cands if c["P"] not in forbidden] or cands
        best = max(allowed, key=lambda c: (int(c.get("k", "0")), -len(c["P"]), c["P"]))
        if len(cands) > 1:
            notes.append(f"  {dep}: chose {best['P']} of " +
                         ", ".join(sorted(c["P"] for c in cands)))
        return best

    def visit(name, group, chain):
        if name in chosen:
            return
        if name in forbidden:
            die(f"'{name}' is forbidden in {os.path.relpath(WORLD, ROOT)}, but needed: "
                + " -> ".join(chain + (name,)))
        chosen[name] = group
        for tok in pkgs[name]["D"]:
            if tok.startswith("!"):
                continue            # a conflict, not a dependency
            dep = pick(dep_name(tok), name)
            if dep["P"] not in chain:
                visit(dep["P"], group, chain + (name,))
        order.append((group, name))

    for group, names in groups:
        for name in names:
            if name not in pkgs:
                die(f"'{name}' ([{group}]) is not in Alpine {BRANCH} {ARCH}")
            if group == BUILD_ONLY:
                if name not in chosen:      # just the package: its headers, not its tree
                    chosen[name] = group
                    order.append((group, name))
                continue
            visit(name, group, ())

    # install_if, as apk does it: a package whose conditions are all
    # installed comes along by itself (man pages once "docs" is there, the
    # bash completions of util-linux once bash-completion is...). It joins
    # the latest group among its conditions.
    group_rank = {g: i for i, (g, _) in enumerate(groups)}
    conditional = sorted((p for p in pkgs.values() if p.get("i")), key=lambda p: p["P"])
    while True:
        have = {}
        for n, g in chosen.items():
            if g == BUILD_ONLY:
                continue
            have[n] = g
            for tok in pkgs[n]["p"]:
                have.setdefault(dep_name(tok), g)
        added = False
        for p in conditional:
            if p["P"] in chosen or p["P"] in forbidden:
                continue
            conds = [dep_name(t) for t in p["i"].split()]
            if all(c in have for c in conds):
                group = max((have[c] for c in conds), key=lambda g: group_rank.get(g, -1))
                visit(p["P"], group, ("install_if " + " ".join(conds),))
                added = True
        if not added:
            break

    if notes:
        print("Where several packages offer the same thing:")
        print("\n".join(notes))
    return order


# --- the lock file --------------------------------------------------------------

def world_hash():
    # x86 hashes only packages.txt, the same bytes the existing lock uses.
    if len(WORLD_FILES) == 1:
        return hashlib.sha256(open(WORLD_FILES[0], "rb").read()).hexdigest()
    h = hashlib.sha256()
    for path in WORLD_FILES:
        h.update(os.path.relpath(path, ROOT).encode() + b"\0")
        h.update(open(path, "rb").read())
    return h.hexdigest()


def write_lock(entries):
    with open(LOCK, "w") as f:
        f.write("# packages.lock — written by tools/fetch-live.py --update; do not edit.\n")
        f.write("# Every package on the live disk, in install order, with the SHA-256 of\n")
        f.write("# its .apk. The build refuses any download that doesn't match.\n")
        f.write(f"world {world_hash()}\n")
        f.write(f"# group      package                          version              repo       sha256\n")
        for e in entries:
            f.write(f"{e['group']:<12} {e['name']:<32} {e['version']:<20} {e['repo']:<10} {e['sha']}\n")


def read_lock():
    if not os.path.exists(LOCK):
        die(f"no {os.path.relpath(LOCK, ROOT)} yet: "
            "run tools/fetch-live.py --arch "
            f"{'x86_64' if ARCH != 'x86' else 'x86'} --update")
    entries, world = [], None
    for line in open(LOCK):
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        if f[0] == "world":
            world = f[1]
            continue
        entries.append(dict(group=f[0], name=f[1], version=f[2], repo=f[3], sha=f[4]))
    if world != world_hash():
        die(f"{os.path.relpath(WORLD_FILES[0], ROOT)} changed since the lock "
            "was written: run tools/fetch-live.py --update")
    return entries


# --- 3. download ----------------------------------------------------------------

def apk_path(e):
    return os.path.join(APKDIR, f"{e['name']}-{e['version']}.apk")


def download(e):
    path = apk_path(e)
    if not os.path.exists(path):
        url = f"{MIRROR}/{BRANCH}/{e['repo']}/{ARCH}/{e['name']}-{e['version']}.apk"
        try:
            fetch(url, path)
        except Exception as err:
            die(f"{e['name']} {e['version']}: download failed ({err}). If the "
                "mirror is up, Alpine may have replaced this version: run --update")
    got = sha256(path)
    if e["sha"] and got != e["sha"]:
        os.remove(path)
        die(f"{e['name']}: checksum mismatch ({got}), refusing it")
    return got


# --- 4. the root file system archive ------------------------------------------

def split_gzip(data):
    """An .apk is three gzip streams back to back: signature, control
    (.PKGINFO and install scripts), data (the files). Returns their bytes."""
    parts, pos = [], 0
    while pos < len(data):
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        d.decompress(data[pos:])
        end = len(data) - len(d.unused_data)
        parts.append(data[pos:end])
        pos = end
    return parts


def norm(path):
    """'./usr/bin/' -> 'usr/bin'. (Not lstrip('./'): that eats '.profile'.)"""
    while path.startswith("./"):
        path = path[2:]
    return path.strip("/") if path != "." else ""


def q1(digest):
    """apk's checksum notation: 'Q1' + base64 of a SHA-1."""
    return "Q1" + base64.b64encode(digest).decode()


def read_pkginfo(text):
    info = {}
    for line in text.splitlines():
        if " = " in line and not line.startswith("#"):
            k, v = line.split(" = ", 1)
            info.setdefault(k, []).append(v)
    return info


class Root:
    """Writes the archive: directories once each, files in order (a later
    file with the same path replaces an earlier one when unpacked)."""

    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.tar = tarfile.open(path + ".tmp", "w", format=tarfile.PAX_FORMAT)
        self.path = path
        self.dirs = set()
        self.owner = {}         # path -> package that installed it
        self.clashes = []
        self.bytes = {}         # group -> bytes
        self.kept = {}          # KEEP path -> (TarInfo, contents): the last one
        self.skel = {}          # etc/skel/... -> (TarInfo, contents or None)

    def add_dirs(self, path):
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in self.dirs:
                self.dirs.add(d)
                ti = tarfile.TarInfo(d)
                ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, int(time.time())
                self.tar.addfile(ti)

    def add(self, ti, fileobj, group, who):
        ti.name = norm(ti.name)
        if not ti.name:
            return
        ti.pax_headers = {}
        if ti.isdir():
            if ti.name in self.dirs:
                return
            self.add_dirs(ti.name)
            self.dirs.add(ti.name)
            self.tar.addfile(ti)
            return
        self.add_dirs(ti.name)
        if ti.islnk():
            ti.linkname = norm(ti.linkname)
        prev = self.owner.get(ti.name)
        if prev and prev != who and not ti.issym():
            self.clashes.append(f"  {ti.name}: {prev}, then {who}")
        self.owner[ti.name] = who
        if ti.name in KEEP or ti.name.startswith("etc/skel/"):
            body = fileobj.read() if fileobj else None
            fileobj = io.BytesIO(body) if fileobj else None
            if ti.name in KEEP:
                self.kept[ti.name] = (ti, body)
            else:
                self.skel[ti.name] = (ti, body)
        self.tar.addfile(ti, fileobj)
        self.bytes[group] = self.bytes.get(group, 0) + (ti.size if ti.isreg() else 0)

    def add_bytes(self, name, data, group, mode=0o644):
        ti = tarfile.TarInfo(name)
        ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
        self.add(ti, io.BytesIO(data), group, "MyOS")

    def close(self):
        self.tar.close()
        os.replace(self.path + ".tmp", self.path)


def add_package(root, e, db, scripts, first_boot, triggers):
    data = open(apk_path(e), "rb").read()
    parts = split_gzip(data)
    if len(parts) != 3:
        die(f"{e['name']}: expected 3 gzip streams in the .apk, found {len(parts)}")
    control, payload = parts[1], parts[2]
    checksum = q1(hashlib.sha1(control).digest())

    info, pkg_scripts = {}, {}
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(control)), mode="r:", ignore_zeros=True) as t:
        for m in t:
            if m.name == ".PKGINFO":
                info = read_pkginfo(t.extractfile(m).read().decode())
            elif m.name.startswith(".") and m.isreg():
                pkg_scripts[m.name[1:]] = t.extractfile(m).read()
    name, version = info["pkgname"][0], info["pkgver"][0]

    # apk's database entry: the package's facts, then its files by folder.
    rec = [f"C:{checksum}", f"P:{name}", f"V:{version}", f"A:{info.get('arch', [ARCH])[0]}",
           f"S:{len(data)}", f"I:{info.get('size', ['0'])[0]}", f"T:{info.get('pkgdesc', [''])[0]}",
           f"U:{info.get('url', [''])[0]}", f"L:{info.get('license', [''])[0]}"]
    for key, field in (("origin", "o"), ("maintainer", "m"), ("builddate", "t"), ("commit", "c"),
                       ("provider_priority", "k")):
        if key in info:
            rec.append(f"{field}:{info[key][0]}")
    for key, field in (("depend", "D"), ("provides", "p"), ("install_if", "i")):
        if key in info:
            rec.append(f"{field}:{' '.join(info[key])}")

    cur_dir = None
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(payload)), mode="r:", ignore_zeros=True) as t:
        for m in t:
            sha1 = m.pax_headers.get("APK-TOOLS.checksum.SHA1")
            path = norm(m.name)
            if not path:
                continue
            if m.isdir():
                rec.append(f"F:{path}")
                if (m.mode & 0o7777, m.uid, m.gid) != (0o755, 0, 0):
                    rec.append(f"M:{m.uid}:{m.gid}:{m.mode & 0o7777:o}")
                cur_dir = path
            else:
                d, base = os.path.split(path)
                if d != cur_dir:
                    rec.append(f"F:{d}")
                    cur_dir = d
                rec.append(f"R:{base}")
                if (m.mode & 0o7777, m.uid, m.gid) != (0o644, 0, 0):
                    rec.append(f"a:{m.uid}:{m.gid}:{m.mode & 0o7777:o}")
                if sha1:
                    rec.append("Z:" + q1(bytes.fromhex(sha1)))
            root.add(m, t.extractfile(m) if m.isreg() else None, e["group"], name)
    db.append("\n".join(rec) + "\n")

    for kind, body in pkg_scripts.items():
        scripts.append((f"{name}-{version}.{checksum}.{kind}", body))
        if kind in ("pre-install", "post-install", "trigger"):
            # A trigger gets, as arguments, the folders matching its
            # patterns (apk passes the ones that changed: all, on install).
            pats = " ".join(info.get("triggers", [])) if kind == "trigger" else ""
            first_boot.append((name, version, kind, body, pats))
    if "triggers" in info:
        triggers.append(f"{checksum} {' '.join(info['triggers'])}")


def add_source(root):
    """The MyOS source, as git sees the working tree: tracked and new files,
    without what .gitignore leaves out (build/, dist/, disks/)."""
    files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"],
                           cwd=ROOT, check=True, capture_output=True).stdout.split(b"\0")
    n = 0
    for f in files:
        rel = f.decode()
        full = os.path.join(ROOT, rel)
        if not rel or not os.path.lexists(full):
            continue
        ti = root.tar.gettarinfo(full, "usr/src/myos/" + rel)
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = "root"
        with open(full, "rb") if ti.isreg() else open(os.devnull, "rb") as fo:
            root.add(ti, fo if ti.isreg() else None, "myos-source", "MyOS")
        n += 1
    return n


def add_overlay(root, src=OVERLAY, group="overlay", who="MyOS overlay",
                skip_prefix=()):
    """live/overlay as it is, owned by root. Symlinks stay symlinks (runit's
    enabled services are links into /etc/sv)."""
    n = 0
    for dirpath, dirnames, filenames in os.walk(src):
        links = [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
        dirnames[:] = sorted(d for d in dirnames if d not in links)
        for fn in sorted(filenames + links):
            if fn == ".DS_Store":
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, src)
            if any(rel == p or rel.startswith(p + "/") for p in skip_prefix):
                continue
            ti = root.tar.gettarinfo(full, rel)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            if ti.isreg():
                with open(full, "rb") as fo:
                    root.add(ti, fo, group, who)
            else:
                root.add(ti, None, group, who)
            n += 1
    return n


def add_live_user(root):
    """LIVE_USER in /etc/passwd, /etc/group and /etc/shadow (owners and modes
    as the packages had them), and /home/<name> from /etc/skel."""
    u = LIVE_USER
    name, uid = u["name"], u["uid"]
    for path in KEEP:
        if path not in root.kept:
            die(f"no {path} in the packages (alpine-baselayout-data?)")
    def put(path, lines):
        ti0, _ = root.kept[path]
        data = ("\n".join(lines) + "\n").encode()
        ti = tarfile.TarInfo(path)
        ti.size, ti.mode, ti.mtime = len(data), ti0.mode, int(time.time())
        ti.uid, ti.gid, ti.uname, ti.gname = ti0.uid, ti0.gid, ti0.uname, ti0.gname
        root.add(ti, io.BytesIO(data), "overlay", root.owner.get(path))
    def lines(path):
        return [l for l in root.kept[path][1].decode().splitlines() if l]

    passwd = [l for l in lines("etc/passwd") if not l.startswith(name + ":")]
    passwd.append(f"{name}:x:{uid}:{uid}:{u['gecos']}:/home/{name}:{u['shell']}")
    group = []
    for l in lines("etc/group"):
        f = l.split(":")
        if f[0] == name:
            continue
        if f[0] in u["groups"]:
            members = [m for m in f[3].split(",") if m and m != name] + [name]
            f[3] = ",".join(members)
        group.append(":".join(f))
    group.append(f"{name}:x:{uid}:")
    days = int(time.time()) // 86400
    shadow = [l for l in lines("etc/shadow") if not l.startswith(name + ":")]
    shadow.append(f"{name}::{days}:0:99999:7:::")
    put("etc/passwd", passwd)
    put("etc/group", group)
    put("etc/shadow", shadow)

    home = f"home/{name}"
    ti = tarfile.TarInfo(home)
    ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o700, int(time.time())
    ti.uid = ti.gid = uid
    ti.uname = ti.gname = name
    root.add(ti, None, "overlay", "MyOS live user")
    for path in sorted(root.skel):
        sti, body = root.skel[path]
        ti = tarfile.TarInfo(home + path[len("etc/skel"):])
        ti.type, ti.mode, ti.mtime = sti.type, sti.mode, sti.mtime
        ti.linkname, ti.size = sti.linkname, sti.size
        ti.uid = ti.gid = uid
        ti.uname = ti.gname = name
        root.add(ti, io.BytesIO(body) if body is not None else None, "overlay", "MyOS live user")


def first_boot_files(root, first_boot):
    """Alpine's install scripts, numbered in install order (a package's
    scripts run after those of the packages it needs)."""
    listing = []
    for i, (name, version, kind, body, pats) in enumerate(first_boot, 1):
        fn = f"{i:03d}-{name}.{kind}"
        root.add_bytes(f"etc/myos/first-boot/{fn}", body, "overlay", 0o755)
        listing.append(f"{fn} {version} {pats}".rstrip())
    root.add_bytes("etc/myos/first-boot/LIST", ("\n".join(listing) + "\n").encode(), "overlay")
    return listing


def make_sysroot(entries):
    """Headers and libraries of every package, for a cross compiler on the
    Mac: usr/include, usr/lib and lib. Absolute symlinks (usr/lib/libc.so ->
    /lib/ld-musl-x86.so.1) are made relative, so they point inside the
    sysroot rather than at the Mac's own /lib."""
    import shutil
    shutil.rmtree(SYSROOT, ignore_errors=True)
    keep = ("usr/include/", "usr/lib/", "lib/")
    n = 0
    for e in entries:
        payload = split_gzip(open(apk_path(e), "rb").read())[2]
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(payload)), mode="r:", ignore_zeros=True) as t:
            for m in t:
                path = norm(m.name)
                if not path.startswith(keep) or m.isdir():
                    continue
                dest = os.path.join(SYSROOT, path)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if os.path.lexists(dest):
                    os.remove(dest)
                if m.issym():
                    target = m.linkname
                    if target.startswith("/"):
                        target = os.path.relpath(os.path.join(SYSROOT, target.lstrip("/")),
                                                 os.path.dirname(dest))
                    os.symlink(target, dest)
                elif m.isreg():
                    with open(dest, "wb") as f:
                        f.write(t.extractfile(m).read())
                    n += 1
    open(os.path.join(SYSROOT, ".done"), "w").close()
    print(f"{os.path.relpath(SYSROOT, ROOT)}: {n} files")


def mib(n):
    return f"{n / (1 << 20):8.1f} MiB"


def main():
    alpine_arch, argv = parse_arch(sys.argv[1:])
    configure(alpine_arch)
    update = "--update" in argv
    login_only = "--login-root" in argv
    groups, forbidden = read_world()

    if update:
        pkgs = load_index()
        order = resolve(groups, pkgs, forbidden)
        entries = [dict(group=g, name=n, version=pkgs[n]["V"], repo=pkgs[n]["repo"], sha="")
                   for g, n in order]
    else:
        entries = read_lock()
    bad = [e["name"] for e in entries if e["name"] in forbidden]
    if bad:
        die(f"forbidden package(s) in the lock: {', '.join(bad)}: run --update")

    os.makedirs(APKDIR, exist_ok=True)
    total = sum(1 for _ in entries)
    for i, e in enumerate(entries, 1):
        if not os.path.exists(apk_path(e)):
            print(f"[{i}/{total}] {e['name']} {e['version']}")
        e["sha"] = download(e)
    if update:
        write_lock(entries)
        print(f"Wrote {os.path.relpath(LOCK, ROOT)}: {total} packages")
    if "--sysroot" in argv:
        make_sysroot(entries)
        return

    entries = [e for e in entries if e["group"] != BUILD_ONLY]
    if login_only:
        entries = [e for e in entries if e["group"] in LOGIN_GROUPS]
        out_path = LOGIN_OUT
    else:
        out_path = OUT
    total = len(entries)
    root = Root(out_path)
    db, scripts, first_boot, triggers = [], [], [], []
    for e in entries:
        add_package(root, e, db, scripts, first_boot, triggers)

    # apk's world file takes "!name" too: apk on MyOS won't install it either.
    world = [n for g, names in groups
             if g != BUILD_ONLY and (not login_only or g in LOGIN_GROUPS)
             for n in names] + \
        sorted("!" + n for n in forbidden)
    root.add_bytes("lib/apk/db/installed", "\n".join(db).encode(), "overlay")
    root.add_bytes("lib/apk/db/triggers", ("\n".join(triggers) + "\n").encode(), "overlay")
    root.add_bytes("lib/apk/db/lock", b"", "overlay", 0o600)
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as st:
        for name, body in scripts:
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(body), 0o755, int(time.time())
            st.addfile(ti, io.BytesIO(body))
    root.add_bytes("lib/apk/db/scripts.tar", buf.getvalue(), "overlay")
    root.add_bytes("etc/apk/world", ("\n".join(world) + "\n").encode(), "overlay")
    root.add_bytes("etc/apk/arch", f"{ARCH}\n".encode(), "overlay")
    listing = first_boot_files(root, first_boot)
    nsrc = 0 if login_only else add_source(root)
    nover = add_overlay(root)
    arch_overlay = os.path.join(ROOT, "live", f"overlay.{ARCH}")
    if os.path.isdir(arch_overlay):
        nover += add_overlay(root, arch_overlay, "overlay", f"MyOS overlay.{ARCH}")
    if os.path.isdir(EXTRA):
        extra_skip = LOGIN_EXTRA_SKIP if login_only else ()
        nover += add_overlay(root, EXTRA, "overlay", "MyOS build", extra_skip)
    add_live_user(root)
    root.close()

    if root.clashes:
        print(f"Files installed by two packages (the later one is kept): {len(root.clashes)}")
        print("\n".join(root.clashes[:20]))
    print(f"\n{os.path.relpath(out_path, ROOT)}: {total} packages, "
          f"{len(listing)} install scripts for the first boot, "
          f"{nsrc} source files, {nover} overlay files")
    print("Size (file contents, before compression):")
    for g in [g for g, _ in groups if g != BUILD_ONLY] + ["myos-source", "overlay"]:
        n = sum(1 for e in entries if e["group"] == g)
        label = f"{n} packages" if n else ""
        print(f"  {g:<12} {mib(root.bytes.get(g, 0))}  {label}")
    print(f"  {'total':<12} {mib(sum(root.bytes.values()))}")


if __name__ == "__main__":
    main()
