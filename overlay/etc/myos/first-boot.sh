#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# first-boot.sh — finish the install on MyOS's first boot (run by
# /etc/runit/1), doing what tools/fetch-live.py could not do on the Mac:
#
# KERNEL GAP (x86_64): Alpine install scripts need copy_file_range (326) and
# vfork (190); stage 1 mounts need fsopen/fsmount (430+). See
# docs/x86_64-LIVE-HANDOFF.md — not fixable in this script alone.
# Alpine's install scripts, in install order, the way apk runs them — from
# /, with /bin/sh; a pre-/post-install script gets the package version, a
# trigger the folders its patterns match (and doesn't run if none exist) —
# then our own 9xx-*.sh scripts.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
dir=/etc/myos/first-boot
log=/var/log/first-boot.log

echo "First boot: finishing the install (log: $log)"
cd / || exit 1
while read -r f ver pats; do
    args=$ver
    case $f in
    *.trigger)
        args=
        for d in $pats; do              # unquoted: the shell expands the globs
            [ -e "$d" ] && args="$args $d"
        done
        [ -n "$args" ] || continue
        ;;
    esac
    echo "== $f$args" >> $log
    /bin/sh "$dir/$f" $args < /dev/null >> $log 2>&1 || echo "  $f failed (see $log)"
done < $dir/LIST

for f in "$dir"/9[0-9][0-9]-*.sh; do
    [ -e "$f" ] || continue
    echo "== $f" >> $log
    /bin/sh "$f" < /dev/null >> $log 2>&1 || echo "  $f failed (see $log)"
done
touch /etc/myos/first-boot.done
