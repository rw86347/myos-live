#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# mklivehd.sh — the live disk: the boot area of myos.img plus an ext2
# partition labelled myos-root holding the live userland (rootfs.tar).
#
#   tools/mklivehd.sh build/myos.img build/live/rootfs.tar out.img MiB
#
# mke2fs -d can read a tar itself, but only when built with libarchive, and
# Homebrew's isn't. So the tar is unpacked first — into a case-sensitive
# disk image, because the Mac's own disk would merge names that differ only
# in case (man pages _exit.2 and _Exit.2). Unpacking as a normal user makes
# every file ours (uid 501), so afterwards debugfs gives each file the owner
# the tar says (root, group shadow...).
set -e
img=$1 tar=$2 out=$3 mb=$4
# img="-" : ext2 myos-root only (x86_64 UEFI; the bootloader is on the ESP).
[ -n "$img" ] && [ -f "$tar" ] && [ -n "$out" ] && [ -n "$mb" ] ||
    { echo "usage: $0 boot.img|- rootfs.tar out.img MiB" >&2; exit 1; }
[ "$img" = "-" ] || [ -f "$img" ] ||
    { echo "usage: $0 boot.img|- rootfs.tar out.img MiB" >&2; exit 1; }
E2FS=${E2FS:-/opt/homebrew/opt/e2fsprogs/sbin}

work=$(mktemp -d /tmp/mklivehd.XXXXXX)
mnt=$work/mnt
cleanup() {
    hdiutil detach -quiet "$mnt" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

hdiutil create -quiet -size $((mb * 2))m -type SPARSE -fs "Case-sensitive APFS" \
    -volname myoslive "$work/root.sparseimage"
mkdir "$mnt"
hdiutil attach -quiet -nobrowse -noautoopen -mountpoint "$mnt" "$work/root.sparseimage"
mkdir "$mnt/root"
# Python, not tar: macOS's tar rewrites accented names in decomposed form
# (o + combining accent), and Linux programs look names up byte for byte.
# "fully_trusted" keeps setuid bits and absolute symlinks: it's our tar.
python3 - "$tar" "$mnt/root" <<'EOF'
import os, sys, tarfile
# Unlink first. Alpine's coreutils is one binary; /bin/rm is a symlink to
# it. Opening that path to overwrite follows the link and replaces mkdir,
# chmod, touch and the rest with whatever we meant only for rm.
dest = sys.argv[2]
filt = {'filter': 'fully_trusted'} if hasattr(tarfile, 'fully_trusted_filter') else {}
with tarfile.open(sys.argv[1]) as t:
    for m in t:
        path = os.path.join(dest, m.name)
        if not m.isdir() and os.path.lexists(path) and not os.path.isdir(path):
            os.unlink(path)
        t.extract(m, dest, **filt)
EOF
rm -f "$mnt/root/.dummy"
find "$mnt/root" -name .DS_Store -delete

if [ "$img" = "-" ]; then
    python3 -c "open('$out','wb').truncate($mb * 1048576)"
    START=0
    OFFSET=0
    S=0
else
    cp "$img" "$out"
    S=$(( $(wc -c < "$img") / 512 ))
    START=$(( (S + 2047) / 2048 * 2048 ))
    OFFSET=$((START * 512))
fi
if [ "$OFFSET" -eq 0 ]; then
    "$E2FS/mke2fs" -q -t ext2 -O none,filetype,sparse_super -L myos-root -b 4096 \
        -d "$mnt/root" -E root_owner=0:0 -F "$out" "${mb}M"
else
    "$E2FS/mke2fs" -q -t ext2 -O none,filetype,sparse_super -L myos-root -b 4096 \
        -d "$mnt/root" -E offset=$OFFSET,root_owner=0:0 -F "$out" "${mb}M"
fi

# Owners from the tar, as debugfs commands on the partition (a file that's
# in the tar twice gets its last owner, like the unpacked file).
python3 - "$tar" > "$work/owners.cmd" <<'EOF'
import sys, tarfile
owner = {}
for m in tarfile.open(sys.argv[1]):
    name = m.name.strip('/')
    if name and name != '.dummy':
        owner[name] = (m.uid, m.gid)
for name, (uid, gid) in owner.items():
    print(f'sif "/{name}" uid {uid}')
    print(f'sif "/{name}" gid {gid}')
EOF
if [ "$OFFSET" -eq 0 ]; then
    dbg="$out"
else
    dbg="$out?offset=$OFFSET"
fi
"$E2FS/debugfs" -w -f "$work/owners.cmd" "$dbg" >/dev/null 2>"$work/debugfs.err" || true
if grep -v '^debugfs ' "$work/debugfs.err" | grep -q .; then
    echo "debugfs complained:" >&2
    grep -v '^debugfs ' "$work/debugfs.err" | head >&2
    exit 1
fi
if [ "$img" != "-" ]; then
    "$(dirname "$0")/mbr-partition.sh" "$out" "$S" "$START" $((mb * 2048))
    echo "Built $out (boot area + $mb MiB ext2 myos-root, live userland)"
else
    echo "Built $out ($mb MiB ext2 myos-root, live userland)"
fi
