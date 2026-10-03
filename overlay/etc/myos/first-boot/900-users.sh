#!/bin/sh
# The live disk's user, "myos", is already in /etc/passwd (tools/fetch-live.py
# writes it at build time: useradd can't lock /etc/group on MyOS yet).
#
# alpine-baselayout lists BusyBox's /bin/ash as a login shell; there is none.
remove-shell /bin/ash
