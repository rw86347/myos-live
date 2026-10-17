#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# sshd's account and configuration. Its host keys are made by its service
# (/etc/sv/sshd/run, at every start, ssh-keygen -A: only the missing ones),
# not here: this runs when the image is built (live/tools/live-prepare.py), and
# every machine must have keys of its own.
mkdir -p /etc/ssh /var/empty
if ! grep -q '^sshd:' /etc/passwd 2>/dev/null; then
    adduser -S -D -H -h /var/empty -s /sbin/nologin sshd 2>/dev/null || true
fi
# Alpine 3.20's sshd_config already Includes this directory; add it if a
# future package drop loses the line.
if [ -f /etc/ssh/sshd_config ] && ! grep -q 'sshd_config.d' /etc/ssh/sshd_config; then
    echo 'Include /etc/ssh/sshd_config.d/*.conf' >> /etc/ssh/sshd_config
fi
