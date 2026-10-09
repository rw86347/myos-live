#!/usr/bin/env bash
# msi-chrome-report.sh — run on the Mac: log in to the MSI over ssh, try
# Chromium the ways that matter, and save what happened in
# build/msi-chrome-report.txt (for reading back on the Mac).
#
#   tools/msi-chrome-report.sh                 the MSI at 192.168.1.69
#   MYOS_HOST=192.168.1.50 tools/msi-chrome-report.sh
#
# Also the network (address, route, DNS) and sound (the kernel's findings).
# It only looks and tries: nothing on the MSI changes except scratch files
# in /tmp. Log in at the MSI's own screen first (the desktop must be up),
# and leave Chromium closed there. It takes about three minutes.
set -u
cd "$(dirname "$0")/.."
HOST=${MYOS_HOST:-192.168.1.69}
KEY=${SSH_KEY:-$HOME/.ssh/id_ed25519_nopass}
OUT=build/msi-chrome-report.txt
mkdir -p build

remote() {
cat <<'EOF'
F='bus.cc|angle_platform|^ERR: |gl_display|gl_ozone|zygote_host_impl|udev_watcher|vk_renderer'
echo "== which image"
tail -1 /etc/chromium/myos.conf 2>&1
grep Exec "/usr/share/myos/programs/Internet/Web Browser.desktop" 2>&1
uname -a
echo "== memory"
free -m 2>/dev/null || head -5 /proc/meminfo
echo "== the screen"
ls -l /tmp/.X11-unix/ 2>&1
tail -5 /var/log/Xorg.0.log 2>&1
echo "== network: address, route, DNS"
ip addr 2>/dev/null | grep -E "inet |wlan|eth" | head -6
route -n 2>&1
ls -l /etc/resolv.conf; cat /etc/resolv.conf
python3 -c "import socket; print('lookup baidu.com:', socket.getaddrinfo('baidu.com', 443, 0, 0, 0, socket.AI_ADDRCONFIG)[0][4])" 2>&1
echo "== sound"
dmesg | grep -E "^hda|^sound|hda:|sound:" | head -30
ls -l /dev/snd 2>&1
id
echo "== chromium's profile (a leftover lock stops new windows)"
ls -la ~/.config/chromium/ 2>&1 | head -20
ls -la ~/.config/chromium/Singleton* 2>&1
echo "== kernel log before"
dmesg | tail -25
echo "== 1. a window on the MSI's screen, as the Start menu starts it (60 s)"
( cd ~ && DISPLAY=:0 timeout -k 5 60 setsid sh -c chromium > /tmp/c-win.log 2>&1 )
echo "exit=$?   (124: it was still running at 60 s, good; 133/139/134: it crashed)"
echo "-- its log, without the usual noise:"
grep -aEv "$F" /tmp/c-win.log | tail -40
echo "-- lines in the whole log: $(wc -l < /tmp/c-win.log)"
echo "== kernel log after"
dmesg | tail -25
echo "== 2. no window: render a page and print it (60 s)"
timeout -k 5 60 chromium --headless --dump-dom 'data:text/html,<p>hi</p>' > /tmp/c-hl.out 2> /tmp/c-hl.log
echo "exit=$?"
head -c 300 /tmp/c-hl.out; echo
grep -aEv "$F" /tmp/c-hl.log | tail -15
echo "== 3. the network"
timeout 20 wget -qO- https://example.com 2>&1 | head -3
echo "wget exit=$?"
echo "== the whole window log"
cat /tmp/c-win.log
EOF
}

echo "Asking the MSI ($HOST)... (about three minutes)"
{
    echo "MSI Chromium report, $(date)"
    remote | ssh -i "$KEY" -o ConnectTimeout=10 -o BatchMode=yes "myos@$HOST" 'sh -s' 2>&1
    echo "== ssh exit: $?"
} > "$OUT"
echo "Saved: $OUT ($(wc -l < "$OUT") lines). Tell Claude it is there."
tail -5 "$OUT"
