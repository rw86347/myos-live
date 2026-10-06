#!/usr/bin/env python3
"""qemu-desktop.py — drive a 64-bit MyOS desktop in QEMU like a person:
keys, the mouse, screenshots, and the serial log to check afterwards.

  tools/qemu-desktop.py IMG SCRIPT SERIAL_LOG SHOT_DIR OVMF_CODE OVMF_VARS

IMG is a UEFI image (make ARCH=x86_64 ... myos-uefi-login.img). SCRIPT has
one command per line (# starts a comment):

  expect TEXT      wait (up to 4 minutes) until the serial log shows TEXT
  wait SECONDS
  key NAME         one key, QEMU's names (ret, tab, minus, a, ...)
  until TEXT | KEY press KEY every 3 s until the serial log shows TEXT (up
                   to a minute): for a screen that may not take keys yet
  type TEXT        letters, digits and '-', one key each
  corner           the pointer to the screen's top-left corner
  move DX DY       the pointer by DX, DY pixels
  moveto X Y       the pointer to X, Y, checked: it looks where the pointer
                   is on a screenshot and moves the difference, until it
                   is there (needs Pillow)
  click            the left button
  shot NAME        the screen to SHOT_DIR/NAME.png

The machine: 4 GiB, no PIT, a Cirrus card (so the firmware's 800x600
framebuffer, simplefb), as in make test-uefi-login. The pointer is a PS/2
mouse and moves are relative, and X speeds up fast moves, so `move` goes in
4-pixel steps 50 ms apart, along one axis at a time, which X takes 1:1 —
nearly always: QEMU sometimes hands two over at once and X speeds that up,
so `moveto` checks the result on the screen.
"""
import os, socket, subprocess, sys, time

img, script, serial_log, shot_dir, ovmf_code, ovmf_vars = sys.argv[1:7]
vars_copy = os.path.join(shot_dir, 'desktop-ovmf-vars.fd')
subprocess.run(['cp', ovmf_vars, vars_copy], check=True)
SER, MON = 45454, 45455
q = subprocess.Popen(['qemu-system-x86_64', '-machine', 'q35,pit=off', '-m', '4G', '-cpu', 'qemu64',
                      '-vga', 'cirrus',
                      '-drive', 'if=pflash,format=raw,readonly=on,file=' + ovmf_code,
                      '-drive', 'if=pflash,format=raw,file=' + vars_copy,
                      '-drive', 'if=none,format=raw,file=' + img + ',id=bootdisk',
                      '-device', 'ide-hd,drive=bootdisk,bootindex=0', '-net', 'none', '-no-reboot',
                      '-display', 'none', '-serial', 'tcp:127.0.0.1:%d,server=on,wait=on' % SER,
                      '-monitor', 'tcp:127.0.0.1:%d,server=on,wait=off' % MON],
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


try:
    for line in open(script):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        op, _, arg = line.partition(' ')
        if op == 'expect':
            end = time.time() + 240
            while arg.encode() not in seen and time.time() < end:
                pump(0.5)
            if arg.encode() not in seen:
                sys.exit('qemu-desktop: never saw "%s"' % arg)
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
            for ch in arg:
                monitor('sendkey ' + ('minus' if ch == '-' else ch), 0.1)
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
        elif op == 'shot':
            ppm = os.path.join(shot_dir, arg + '.ppm')
            monitor('screendump ' + ppm, 2)
            try:
                from PIL import Image
                Image.open(ppm).save(os.path.join(shot_dir, arg + '.png'))
                os.remove(ppm)
            except ImportError:
                pass                    # (no Pillow: keep the .ppm)
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
