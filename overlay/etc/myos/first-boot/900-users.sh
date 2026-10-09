#!/bin/sh
# The live disk's user, "myos", is already in /etc/passwd (tools/fetch-live.py
# writes it at build time: useradd can't lock /etc/group on MyOS yet).
#
# alpine-baselayout lists BusyBox's /bin/ash as a login shell; there is none.
remove-shell /bin/ash

# Login is Enter (an empty password). A package script that rewrites
# /etc/shadow must not leave a hash or a lock in that field.
if [ -f /etc/shadow ]; then
    awk -F: 'BEGIN { OFS=":" } $1=="myos" { $2="" } { print }' \
        /etc/shadow > /etc/shadow.myos \
    && cat /etc/shadow.myos > /etc/shadow
    rm -f /etc/shadow.myos
fi
