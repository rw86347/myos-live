#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# stick-data.sh — on the live USB stick, keep people's things on myos-data.
#
# Writing a new build onto the stick replaces the system partition, and
# with it everything in /home: the desktop's settings (~/.config), the
# files on the Desktop. live/tools/write-stick.py gives the stick a
# myos-data partition after the image, which a rewrite leaves alone; the
# kernel mounts it at /data (the root's own disk's, as on an installed
# system). Here, from /etc/runit/1 at every start:
#
#   /home        the image's homes are copied onto /data/home the first
#                time (one already there is the person's own, and wins),
#                then /home becomes a link to /data/home, as the installer
#                makes it on a hard disk.
#   Wi-Fi        /data/stick/wpa_supplicant.conf, the networks the Wi-Fi
#                icon saved (with their passwords: root only). The
#                wpa_supplicant service reads it instead of /etc's.
#
# Nothing happens without a myos-data, nor on a computer MyOS is installed
# on (its root is not on a removable disk).
mountpoint -q /data || exit 0
root=$(awk '$2 == "/" { print $1; exit }' /proc/mounts)
disk=${root#/dev/}
disk=${disk%%[0-9]*}
[ "$(cat "/sys/block/$disk/removable" 2>/dev/null)" = 1 ] || exit 0

if [ ! -L /home ]; then
    mkdir -p /data/home && chmod 755 /data/home || exit 1
    for h in /home/*; do
        [ -d "$h" ] || continue
        [ -e "/data/home/${h##*/}" ] || cp -a "$h" /data/home/ || exit 1
    done
    rm -rf /home && ln -s /data/home /home
fi

w=/data/stick/wpa_supplicant.conf
if [ ! -e $w ]; then
    (umask 077; mkdir -p /data/stick) && cp /etc/wpa_supplicant/wpa_supplicant.conf $w.new &&
        chmod 600 $w.new && mv $w.new $w
fi
exit 0
