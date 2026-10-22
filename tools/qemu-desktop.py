#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""qemu-desktop.py — drive a 64-bit MyOS desktop in QEMU like a person:
keys, the mouse, screenshots, and the serial log to check afterwards.

  live/tools/qemu-desktop.py IMG SCRIPT SERIAL_LOG SHOT_DIR OVMF_CODE OVMF_VARS

IMG is a UEFI image (make ARCH=x86_64 ... myos-uefi-login.img). SCRIPT has
one command per line (# starts a comment):

  expect TEXT      wait (up to 4 minutes) until the serial log shows TEXT
  next TEXT        the same, but only what the log said after the last
                   expect or next matched (for a line that comes again)
  wait SECONDS
  key NAME         one key, QEMU's names (ret, tab, minus, a, ...)
  until TEXT | KEY press KEY every 3 s until the serial log shows TEXT (up
                   to a minute): for a screen that may not take keys yet
  type TEXT        any text on the US keyboard (tools/qemu-type.sh's keys),
                   one key each; no Enter (`key ret`)
  corner           the pointer to the screen's top-left corner
  move DX DY       the pointer by DX, DY pixels
  moveto X Y       the pointer to X, Y, checked: it looks where the pointer
                   is on a screenshot and moves the difference, until it
                   is there (needs Pillow)
  click            the left button
  shot NAME        the screen to SHOT_DIR/NAME.png
  burst NAME N MS  N screens, MS milliseconds apart, to NAME-1.png ... NAME-N.png:
                   an animation's frames (start it in the background first,
                   `sh ... setsid -f sh -c 'sleep 1; ...'`)
  lid              the lid shuts, or opens if it is shut: a CPU added in
                   QEMU's monitor, which a SLEEPTEST=1 kernel takes for the
                   lid (kernel/fakelid_test.c). QEMU_EXTRA needs
                   tools/fakelid.py's table and -smp 1,maxcpus=4 (3 moves)
  powerbtn         the power button (QEMU's system_powerdown)

and, from inside the machine, over ssh:

  sshkey [USER]    let this tool in: types a one-time public key into the
                   window that has the keyboard (a terminal: `key ctrl-alt-t`
                   first) and waits until ssh works. USER is the one logged
                   in, "myos" if left out. The key is made once, in SHOT_DIR.
  sh COMMAND       run COMMAND there (the user's shell); its output is
                   printed, and the run stops if it fails. A program that
                   keeps running: `sh DISPLAY=:0 setsid -f prog >/dev/null 2>&1`
  xdo ARGS         xdotool ARGS on the desktop (DISPLAY=:0): exact and in
                   one step, where move/moveto creep and check —
                     xdo mousemove 412 640 click 1
                     xdo key ctrl+s
                     xdo search --name 'Install MyOS' getwindowgeometry
                   (`xdo type` only for plain ASCII: see `text`)
  text TEXT        type TEXT, any characters (Grüße, 世界), where the
                   keyboard is. ASCII goes through `xdotool type`; each other
                   character is its Unicode keysym (`xdotool key U4E16`),
                   200 ms apart. xdotool gives it a spare key code for the
                   moment, and a program that reads the key before it has
                   seen the new mapping loses it. `xdotool type` loses them
                   all here even when slowed down.
  get PATH [NAME]  copy the file PATH out to SHOT_DIR/NAME (its own name if
                   NAME is left out)
  put FILE PATH    copy FILE on this Mac in to PATH (a newer build of one
                   program, say, without making a new disk)

The machine: 4 GiB, no PIT, a Cirrus card (so the firmware's 800x600
framebuffer, simplefb), as in make test-uefi-login. The pointer is a PS/2
mouse and moves are relative, and X speeds up fast moves, so `move` goes in
4-pixel steps 50 ms apart, along one axis at a time, which X takes 1:1 —
nearly always: QEMU sometimes hands two over at once and X speeds that up,
so `moveto` checks the result on the screen. `xdo` doesn't go through the
mouse at all: X's test extension puts the pointer where it is told.

QEMU_EXTRA in the environment is added to QEMU's options (say,
QEMU_EXTRA='-acpitable file=build/fakebat.aml' for a laptop's battery).

The disk is opened with snapshot=on: nothing a run does is written to IMG
(so the ssh key never stays on a disk that might be copied to a stick).
A script that uses sshkey, sh, xdo or get gets a network card (e1000, QEMU's
user network) whose ssh port is forwarded to a free port on this Mac;
otherwise there is none. The serial and monitor ports are free ones too, so
two runs can go at once.
"""
import os, shlex, socket, subprocess, sys, time

img, script, serial_log, shot_dir, ovmf_code, ovmf_vars = sys.argv[1:7]
vars_copy = os.path.join(shot_dir, 'desktop-ovmf-vars.fd')
subprocess.run(['cp', ovmf_vars, vars_copy], check=True)
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools')  # the kernel repo's


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


lines = [l.split('#', 1)[0].strip() for l in open(script)]
lines = [l for l in lines if l]
want_ssh = any(l.split()[0] in ('sshkey', 'sh', 'xdo', 'text', 'get', 'put') for l in lines)
SER, MON, SSH = free_port(), free_port(), free_port()
net = (['-netdev', 'user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22' % SSH, '-device', 'e1000,netdev=net0']
       if want_ssh else ['-net', 'none'])
q = subprocess.Popen(['qemu-system-x86_64', '-machine', 'q35,pit=off', '-m', '4G', '-cpu', 'qemu64',
                      '-vga', 'cirrus',
                      '-drive', 'if=pflash,format=raw,readonly=on,file=' + ovmf_code,
                      '-drive', 'if=pflash,format=raw,file=' + vars_copy,
                      '-drive', 'if=none,format=raw,file=' + img + ',id=bootdisk,snapshot=on',
                      '-device', 'ide-hd,drive=bootdisk,bootindex=0'] + net + ['-no-reboot',
                      '-display', 'none', '-serial', 'tcp:127.0.0.1:%d,server=on,wait=on' % SER,
                      '-monitor', 'tcp:127.0.0.1:%d,server=on,wait=off' % MON] +
                     shlex.split(os.environ.get('QEMU_EXTRA', '')),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
ser = socket.create_connection(('127.0.0.1', SER))
mon = socket.create_connection(('127.0.0.1', MON))
ser.setblocking(False)
mon.setblocking(False)
log = open(serial_log, 'wb')
seen = b''


def pump(seconds):
    """Read the serial port (and drain the monitor) for a while."""
    global seen
    end = time.time() + seconds
    while time.time() < end:
        for s in (ser, mon):
            try:
                d = s.recv(65536)
                if s is ser and d:
                    seen += d
                    log.write(d)
                    log.flush()
            except BlockingIOError:
                pass
        time.sleep(0.02)


def monitor(cmd, settle=0.02):
    mon.sendall(cmd.encode() + b'\n')
    pump(settle)


def screen():
    """The screen now, as a Pillow image."""
    from PIL import Image
    ppm = os.path.join(shot_dir, 'desktop-pointer.ppm')
    monitor('screendump ' + ppm, 1.5)
    im = Image.open(ppm).convert('RGB')
    os.remove(ppm)
    return im


def find_pointer(im, near, radius):
    """The arrow's tip: its topmost black pixel within radius of near."""
    px, (w, h) = im.load(), im.size
    x0, y0 = max(0, near[0] - radius), max(0, near[1] - radius)
    for y in range(y0, min(h, near[1] + radius)):
        for x in range(x0, min(w, near[0] + radius)):
            if px[x, y] == (0, 0, 0):
                return x, y
    return None


tip = None                              # where the tip's pixel is, from the hot spot


def moveto(x, y):
    global tip
    if tip is None:                     # learn it once: the hot spot is 0,0 in the corner
        for _ in range(20):
            monitor('mouse_move -100 -100')
        im = screen()
        tip = find_pointer(im, (0, 0), 30) or (0, 0)
        at = (0, 0)
    else:
        at = None
    for _ in range(10):
        if at is None:
            # Close to where it should be first: a widening search could
            # find something else black (a tooltip pops up while the
            # pointer rests on an icon).
            im = screen()
            p = None
            for r in (12, 40, 120):
                p = p or find_pointer(im, (x + tip[0], y + tip[1]), r)
            p = p or find_pointer(im, (im.size[0] // 2, im.size[1] // 2), max(im.size))
            if p is None:
                im.save(os.path.join(shot_dir, 'desktop-pointer-lost.png'))
                break
            at = (p[0] - tip[0], p[1] - tip[1])
        dx, dy = x - at[0], y - at[1]
        if abs(dx) <= 1 and abs(dy) <= 1:
            return
        steps(dx, dy, 2)
        at = None
    sys.exit('qemu-desktop: could not put the pointer at %d,%d' % (x, y))


def steps(dx, dy, size=4):
    for d, axis in ((dx, 0), (dy, 1)):
        sign = 1 if d > 0 else -1
        while d:
            n = sign * min(size, abs(d))
            # (50 ms apart: closer together, X counts them as one fast
            # move and speeds the pointer up)
            monitor('mouse_move %d %d' % ((n, 0) if axis == 0 else (0, n)), 0.05)
            d -= n


def type_text(text):
    """One sendkey per character, the keys tools/qemu-type.sh knows (it
    adds an Enter at the end, which is left off here)."""
    keys = subprocess.run([os.path.join(TOOLS, 'qemu-type.sh'), text], capture_output=True,
                          text=True, check=True).stdout.split('\n')
    for k in keys[:-2]:                 # (the last two: "sendkey ret" and "")
        monitor(k, 0.1)


key_file = os.path.join(shot_dir, 'desktop-key')
ssh_user = 'myos'


def guest(cmd, out_path=None, timeout=120, in_path=None):
    """Run cmd in the machine over ssh: (exit status, output). The serial
    port is read meanwhile, so the machine never waits on a full buffer;
    the output goes to a file, so a big one can't fill a pipe and stall.
    in_path, if given, is cmd's input."""
    out_path = out_path or os.path.join(shot_dir, 'desktop-ssh.out')
    with open(out_path, 'wb') as out, open(in_path or os.devnull, 'rb') as inp:
        p = subprocess.Popen(['ssh', '-i', key_file, '-p', str(SSH), '-o', 'IdentitiesOnly=yes',
                              '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                              '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
                              '-o', 'LogLevel=ERROR', ssh_user + '@127.0.0.1',
                              # X wants the session's cookie (myos-login writes it)
                              'export XAUTHORITY="$HOME/.Xauthority"; ' + cmd],
                             stdin=inp, stdout=out, stderr=subprocess.STDOUT)
        end = time.time() + timeout
        while p.poll() is None:
            if time.time() > end:
                p.kill()
                p.wait()
                return 124, b'(timed out)'
            pump(0.2)
    return p.returncode, open(out_path, 'rb').read()


def guest_ok(cmd):
    """guest(), printing what it said; the run stops if cmd failed."""
    rc, out = guest(cmd)
    text = out.decode(errors='replace').rstrip()
    print('$ ' + cmd + ('\n' + text if text else ''), flush=True)
    if rc:
        sys.exit('qemu-desktop: failed (%d): %s' % (rc, cmd))


def sshkey(user):
    global ssh_user
    ssh_user = user or 'myos'
    if not os.path.exists(key_file):
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', 'qemu-desktop',
                        '-f', key_file], check=True)
    pub = open(key_file + '.pub').read().strip()
    type_text("mkdir -p ~/.ssh && echo '%s' >> ~/.ssh/authorized_keys" % pub)
    monitor('sendkey ret', 0.2)
    end = time.time() + 90              # sshd may still be starting
    while time.time() < end:
        if guest('true', timeout=15)[0] == 0:
            return
        pump(3)
    sys.exit('qemu-desktop: ssh never let us in (as %s, port %d)' % (ssh_user, SSH))


pos = 0                                 # where the last expect or next matched
cpus = 0                                # CPUs added (the lid)
try:
    for line in lines:
        op, _, arg = line.partition(' ')
        if op in ('expect', 'next'):
            start = 0 if op == 'expect' else pos
            end = time.time() + 240
            while seen.find(arg.encode(), start) < 0 and time.time() < end:
                pump(0.5)
            at = seen.find(arg.encode(), start)
            if at < 0:
                sys.exit('qemu-desktop: never saw "%s"' % arg)
            pos = at + len(arg.encode())
        elif op == 'wait':
            pump(float(arg))
        elif op == 'until':
            text, _, key = arg.partition(' | ')
            end = time.time() + 60
            while text.encode() not in seen and time.time() < end:
                monitor('sendkey ' + key.strip(), 0.2)
                pump(3)
            if text.encode() not in seen:
                sys.exit('qemu-desktop: never saw "%s"' % text)
        elif op == 'key':
            monitor('sendkey ' + arg, 0.2)
        elif op == 'type':
            type_text(arg)
        elif op == 'corner':
            for _ in range(20):
                monitor('mouse_move -100 -100')
        elif op == 'move':
            dx, dy = (int(v) for v in arg.split())
            steps(dx, dy)
        elif op == 'moveto':
            x, y = (int(v) for v in arg.split())
            moveto(x, y)
        elif op == 'click':
            monitor('mouse_button 1', 0.1)
            monitor('mouse_button 0', 0.1)
        elif op in ('shot', 'burst'):
            name, n, ms = (arg.split() + ['1', '0'])[:3] if op == 'burst' else (arg, '1', '0')
            names = [name] if op == 'shot' else ['%s-%d' % (name, i + 1) for i in range(int(n))]
            for i, nm in enumerate(names):
                monitor('screendump ' + os.path.join(shot_dir, nm + '.ppm'),
                        2 if op == 'shot' else int(ms) / 1000)
            if op == 'burst':
                pump(2)                 # (the last one written)
            for nm in names:
                ppm = os.path.join(shot_dir, nm + '.ppm')
                try:
                    from PIL import Image
                    Image.open(ppm).save(os.path.join(shot_dir, nm + '.png'))
                    os.remove(ppm)
                except ImportError:
                    pass                # (no Pillow: keep the .ppm)
        elif op == 'lid':
            cpus += 1
            monitor('device_add qemu64-x86_64-cpu,id=lid%d,socket-id=0,core-id=%d,thread-id=0'
                    % (cpus, cpus), 0.5)
        elif op == 'powerbtn':
            monitor('system_powerdown', 0.5)
        elif op == 'sshkey':
            sshkey(arg)
        elif op == 'sh':
            guest_ok(arg)
        elif op == 'xdo':
            guest_ok('DISPLAY=:0 xdotool ' + arg)
        elif op == 'text':
            runs, i = [], 0             # alternate: ASCII with type, the rest with key
            while i < len(arg):
                j = i
                ascii = ord(arg[i]) < 128
                while j < len(arg) and (ord(arg[j]) < 128) == ascii:
                    j += 1
                part = arg[i:j]
                runs.append('xdotool type -- ' + shlex.quote(part) if ascii else
                            'xdotool key --delay 200 ' + ' '.join('U%04X' % ord(c) for c in part))
                i = j
            guest_ok('export DISPLAY=:0; ' + ' && '.join(runs))
        elif op == 'get':
            path, _, name = arg.partition(' ')
            dest = os.path.join(shot_dir, name or os.path.basename(path))
            rc, _ = guest('cat ' + shlex.quote(path), out_path=dest)
            if rc:
                sys.exit('qemu-desktop: could not get %s (%d): %s'
                         % (path, rc, open(dest, 'rb').read(200).decode(errors='replace')))
            print('got %s -> %s' % (path, dest), flush=True)
        elif op == 'put':
            src, _, path = arg.partition(' ')
            rc, out = guest('cat > ' + shlex.quote(path), in_path=src)
            if rc:
                sys.exit('qemu-desktop: could not put %s (%d): %s'
                         % (path, rc, out[:200].decode(errors='replace')))
            print('put %s -> %s' % (src, path), flush=True)
        else:
            sys.exit('qemu-desktop: unknown command: ' + line)
finally:
    try:
        mon.sendall(b'quit\n')
    except OSError:
        pass
    time.sleep(1)
    q.kill()
    log.close()
