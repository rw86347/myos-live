#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# ping without setuid. Alpine installs iputils' ping setuid root; it then
# keeps only CAP_NET_RAW with capset, which MyOS refuses (it can't enforce a
# partial drop: kernel/sys_info.c), so ping stopped at "cap_set_proc:
# Operation not permitted". MyOS lets every user open a raw ICMP socket that
# can send only echo requests (net/socket.c, raw_send_ok), so ping needs no
# privilege at all: as the user, it has no capabilities to drop.
[ -f /bin/ping ] && chmod u-s /bin/ping
