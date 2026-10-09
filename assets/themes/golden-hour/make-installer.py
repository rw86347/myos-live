#!/usr/bin/env python3
"""make-installer.py — mockups of the installer (docs/INSTALLER-PLAN.md).

Draws installWizard01..09 in the Golden Hour theme with make-theme.py's own
pieces (palette, window, buttons, text boxes, the login screen's backdrop),
so the wizard looks like the rest of MyOS. SVGs land next to the other
mockups here; the PNGs (1024x768) go to docs/installer/.

  python3 assets/themes/golden-hour/make-installer.py
"""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "..", "docs", "installer")
spec = importlib.util.spec_from_file_location("theme", os.path.join(HERE, "make-theme.py"))
T = importlib.util.module_from_spec(spec)
spec.loader.exec_module(T)
C, S, text, img, draw_bar, window = T.C, T.S, T.text, T.img, T.draw_bar, T.window
W, H, FONT_TITLE = T.W, T.H, T.FONT_TITLE

STEPS = ["Language", "Keyboard", "Time zone", "Disk", "Partitions", "Your account",
         "Summary", "Install", "Done"]

# the wizard window: centered on the blurred wallpaper, like the login screen
WX, WY, WW, WH = 112, 74, 800, 600
SIDE = 190                      # the step list on the left
PX, PY = WX + SIDE + 28, WY + 60
PW = WW - SIDE - 56


def backdrop():
    o = ['<image href="wallpaper.jpg" x="-20" y="-20" width="%d" height="%d" '
         'preserveAspectRatio="xMidYMid slice" filter="url(#blur)"/>' % (W + 40, H + 40),
         draw_bar(S("Login background", "normal"), 0, 0, W, H)]
    band = S("Login bands", "normal")
    o.append(draw_bar(band, 0, 0, W, 56))
    o.append(img("logo", 20, 12, 32))
    o.append(text(62, 37, "Install MyOS", 20, "#ffffff", "bold", FONT_TITLE,
                  extra='font-style="italic" filter="url(#labelshadow)"'))
    o.append(text(W - 24, 36, "Running from the USB stick", 12, C["teal_btn_top"], anchor="end"))
    return "".join(o)


def side(step):
    """The step list: done steps ticked, the current one highlighted."""
    x, y = WX + 1, WY + 30
    o = ['<rect x="%d" y="%d" width="%d" height="%d" fill="url(#sand)"/>' % (x, y, SIDE, WH - 31),
         '<path d="M%s %sV%s" stroke="%s"/>' % (x + SIDE, y, WY + WH - 1, C["border"])]
    hl = S("Menu highlight", "hover")
    for k, name in enumerate(STEPS):
        yy = y + 18 + k * 34
        if k == step:
            o.append(draw_bar(hl, x + 8, yy - 4, SIDE - 16, 28))
        color = hl["text"] if k == step else (C["text"] if k < step else C["text_soft"])
        mark = "✓" if k < step else str(k + 1)
        o.append('<circle cx="%s" cy="%s" r="10" fill="%s" stroke="%s"/>'
                 % (x + 26, yy + 10, "#ffffff" if k <= step else "none",
                    C["label_teal"] if k <= step else C["border"]))
        o.append(text(x + 26, yy + 14, mark, 11, C["label_teal"] if k <= step else C["text_soft"],
                      "bold", anchor="middle"))
        o.append(text(x + 44, yy + 15, name, 13, color, "bold" if k == step else "normal"))
    return "".join(o)


def heading(icon, title, sub):
    return (img(icon, PX, PY - 10, 44) +
            text(PX + 56, PY + 10, title, 22, weight="bold", font=FONT_TITLE) +
            text(PX + 56, PY + 30, sub, 12, C["text_soft"]))


def buttons(back=True, next_label="Next", next_state="default", extra=None):
    y = WY + WH - 46
    o = ['<path d="M%s %sH%s" stroke="%s"/>' % (WX + SIDE + 1, y - 12, WX + WW - 1, C["border"])]
    if extra:
        o.append(draw_bar(S("Push button", "normal"), WX + SIDE + 24, y, 130, 28, extra))
    if back:
        o.append(draw_bar(S("Push button", "normal"), WX + WW - 236, y, 100, 28, "Back"))
    o.append(draw_bar(S("Push button", next_state), WX + WW - 124, y, 100, 28, next_label))
    return "".join(o)


def card(x, y, w, h, fill="#ffffff", opacity=0.75, stroke=None):
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="%s" fill-opacity="%s" stroke="%s"/>'
            % (x, y, w, h, fill, opacity, stroke or C["border"]))


def field(x, y, w, label, value, focused=False, hint=None, secret=False):
    o = [text(x, y + 17, label, 12)]
    st = S("Text box", "focused" if focused else "normal")
    o.append(draw_bar(st, x + 150, y, w - 150, 26))
    shown = "•" * len(value) if secret else value
    o.append(text(x + 160, y + 18, shown, 13, extra='letter-spacing="2"' if secret else ""))
    if focused:
        o.append('<rect x="%s" y="%s" width="1.5" height="16" fill="%s"/>'
                 % (x + 162 + len(shown) * (9 if secret else 7.6), y + 5, C["text"]))
    if hint:
        o.append(text(x + 150, y + 42, hint, 11, C["text_soft"]))
    return "".join(o)


def radio(x, y, on, label, sub=None, bold=False):
    o = ['<circle cx="%s" cy="%s" r="8" fill="#ffffff" stroke="%s" stroke-width="1.5"/>'
         % (x + 8, y + 8, C["label_teal"] if on else C["text_soft"])]
    if on:
        o.append('<circle cx="%s" cy="%s" r="4.5" fill="%s"/>' % (x + 8, y + 8, C["label_teal"]))
    o.append(text(x + 26, y + 13, label, 13, weight="bold" if bold or on else "normal"))
    if sub:
        o.append(text(x + 26, y + 31, sub, 11, C["text_soft"]))
    return "".join(o)


def check(x, y, on, label):
    o = ['<rect x="%s" y="%s" width="16" height="16" rx="3" fill="#ffffff" stroke="%s" stroke-width="1.5"/>'
         % (x, y, C["label_teal"] if on else C["text_soft"])]
    if on:
        o.append('<path d="M%s %s l4 4 l7 -9" fill="none" stroke="%s" stroke-width="2.4" '
                 'stroke-linecap="round" stroke-linejoin="round"/>' % (x + 3, y + 8, C["label_teal"]))
    o.append(text(x + 26, y + 13, label, 12))
    return "".join(o)


def list_rows(x, y, w, rows, selected, h=26):
    o = [card(x, y, w, h * len(rows) + 8)]
    hl = S("Menu highlight", "hover")
    for k, cols in enumerate(rows):
        yy = y + 4 + k * h
        if k == selected:
            o.append(draw_bar(hl, x + 4, yy, w - 8, h))
        color = hl["text"] if k == selected else C["text"]
        cx = x + 14
        for c, col in enumerate(cols):
            o.append(text(cx, yy + h / 2 + 5, col[0], 12, color if c == 0 else
                          (hl["text"] if k == selected else C["text_soft"]),
                          "bold" if c == 0 and k == selected else "normal"))
            cx += col[1]
    return "".join(o)


def disk_bar(x, y, w, parts, h=34):
    """A disk drawn as a bar of partitions: (fraction, color, label, sublabel)."""
    o = ['<rect x="%s" y="%s" width="%s" height="%s" rx="6" fill="#ffffff" stroke="%s"/>'
         % (x, y, w, h, C["border"])]
    cx = x
    for frac, color, label, sub in parts:
        pw = w * frac
        o.append('<rect x="%s" y="%s" width="%s" height="%s" fill="%s" stroke="#ffffff" stroke-width="1.5"/>'
                 % (cx + 1, y + 1, max(pw - 2, 2), h - 2, color))
        if pw > 60:
            o.append(text(cx + pw / 2, y + 15, label, 11, "#ffffff", "bold", anchor="middle"))
            o.append(text(cx + pw / 2, y + 28, sub, 10, "#ffffff", anchor="middle", extra='opacity="0.9"'))
        elif pw > 28 and label:         # narrow: the name only, smaller
            o.append(text(cx + pw / 2, y + 21, label, 9, "#ffffff", "bold", anchor="middle"))
        cx += pw
    return "".join(o)


def page(step, body):
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s'
         '<filter id="blur" x="-5%%" y="-5%%" width="110%%" height="110%%"><feGaussianBlur stdDeviation="6"/></filter>'
         '<linearGradient id="sand" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
         '<stop offset="1" stop-color="%s"/></linearGradient>'
         '<linearGradient id="prog" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
         '<stop offset="0.5" stop-color="%s"/><stop offset="0.55" stop-color="%s"/>'
         '<stop offset="1" stop-color="%s"/></linearGradient></defs>'
         % (W, H, W, H, T.COMMON_DEFS, C["panel_sand"], C["panel_sand_deep"],
            C["green_1"], C["green_2"], C["green_3"], C["green_4"])]
    o.append(backdrop())
    o.append(window(WX, WY, WW, WH, "Install MyOS", "disks", "active"))
    o.append(side(step))
    o.append(body)
    o.append("</svg>\n")
    return "".join(o)


# --- the pages ------------------------------------------------------------------------
def p1():
    langs = [("English (United States)", "English"), ("English (United Kingdom)", "English"),
             ("中文 (简体)", "Chinese, Simplified"), ("中文 (繁體)", "Chinese, Traditional"),
             ("Deutsch", "German"), ("Español", "Spanish"), ("Français", "French"),
             ("日本語", "Japanese"), ("Português (Brasil)", "Portuguese")]
    o = [heading("web-browser", "Welcome", "Choose the language for the installer and for MyOS.")]
    o.append(list_rows(PX, PY + 56, PW, [[(a, 260), (b, 200)] for a, b in langs], 0, 30))
    y = PY + 56 + len(langs) * 30 + 22
    o.append(check(PX, y, True, "Also install fonts and input for Chinese (pinyin)"))
    o.append(text(PX, y + 40, "Not ready to install? Close this window to keep trying MyOS from the stick.",
                  11, C["text_soft"]))
    return page(0, "".join(o) + buttons(back=False))


def p2():
    layouts = ["English (US)", "English (US, international with dead keys)", "English (UK)",
               "Chinese", "German", "French", "Spanish", "Dvorak"]
    o = [heading("keyboard", "Keyboard", "Pick the layout printed on your keys.")]
    o.append(text(PX, PY + 66, "Detected:", 12, C["text_soft"]))
    o.append(text(PX + 70, PY + 66, "a US keyboard (MSI Katana, built in)", 12, weight="bold"))
    o.append(list_rows(PX, PY + 80, PW, [[(n, 300)] for n in layouts], 0, 28))
    y = PY + 80 + len(layouts) * 28 + 26
    o.append(field(PX, y, PW, "Try it here:", "The quick brown fox", focused=True,
                   hint="Type a few keys, with Shift and AltGr, to check the layout."))
    return page(1, "".join(o) + buttons())


def p3():
    o = [heading("datetime", "Time zone", "Where you are, so the clock is right.")]
    mx, my, mw, mh = PX, PY + 56, PW, 230
    o.append(card(mx, my, mw, mh, "#cfe8f0", 1))
    # a schematic world: continents as soft shapes, a pin on China
    land = "#9cc6a0"
    for d in ["M40 60 q40 -30 90 -10 q20 30 -10 60 q-30 40 -50 10 z",          # North America
              "M110 140 q30 -10 40 30 q-10 50 -30 50 q-20 -40 -10 -80 z",      # South America
              "M240 50 q50 -20 90 0 q-10 30 -60 30 q-30 0 -30 -30 z",          # Europe
              "M250 100 q40 -10 60 30 q-10 60 -40 70 q-30 -50 -20 -100 z",     # Africa
              "M320 40 q120 -30 200 10 q10 60 -70 90 q-60 0 -90 -30 q-50 -20 -40 -70 z",  # Asia
              "M470 160 q40 -10 60 20 q-20 30 -60 10 z"]:                      # Australia
        o.append('<path d="%s" transform="translate(%s,%s) scale(%s,0.95)" fill="%s"/>'
                 % (d, mx + 10, my + 10, mw / 560, land))
    pinx, piny = mx + 445 * mw / 560, my + 95
    o.append('<path d="M%s %s c-8 -12 -10 -16 -10 -20 a10 10 0 0 1 20 0 c0 4 -2 8 -10 20 z" fill="%s" '
             'stroke="#ffffff" stroke-width="1.5"/>' % (pinx, piny, C["amber_mid"]))
    o.append(text(pinx + 14, piny - 14, "Shanghai", 12, C["text"], "bold"))
    y = my + mh + 22
    o.append(field(PX, y, PW, "Region:", "Asia"))
    o.append(field(PX, y + 36, PW, "City:", "Shanghai (China Standard Time)", focused=True))
    o.append(text(PX + 150, y + 96, "Now: 8:11 PM, Wednesday 7 October 2026", 12, weight="bold"))
    o.append(check(PX, y + 116, True, "Set the time from the network"))
    o.append(check(PX, y + 140, False, "This PC also runs Windows (keep the hardware clock in local time)"))
    return page(2, "".join(o) + buttons())


def p4():
    o = [heading("disks", "Where to install", "MyOS needs at least 8 GB. Pick a disk, then how to use it.")]
    rows = [[("Samsung SSD 980 (NVMe)", 260), ("512 GB", 90), ("Windows 11 on it", 150)],
            [("USB DISK 3.2", 260), ("60 GB", 90), ("this stick: in use", 150)]]
    o.append(list_rows(PX, PY + 56, PW, rows, 0, 30))
    y = PY + 140
    o.append(text(PX, y, "Samsung SSD 980, as it is now:", 12, C["text_soft"]))
    o.append(disk_bar(PX, y + 8, PW, [(0.004, "#7d9bb8", "", ""), (0.03, "#7d9bb8", "EFI", "260 MB"),
                                       (0.76, "#1f6fd1", "Windows (C:)", "NTFS, 390 GB"),
                                       (0.19, "#c9c2b2", "Free", "98 GB"),
                                       (0.016, "#5b6573", "", "")]))
    y += 64
    o.append(radio(PX, y, True, "Install MyOS in the free space (98 GB)",
                   "Windows stays. You choose MyOS or Windows when the computer starts (F11)."))
    o.append(radio(PX, y + 50, False, "Erase this disk and install MyOS",
                   "Windows and everything on this disk will be deleted."))
    o.append(radio(PX, y + 100, False, "Something else",
                   "Choose partitions and sizes yourself."))
    o.append(card(PX, y + 150, PW, 46, "#fff4dc", 1, C["gold"]))
    o.append(img("help", PX + 10, y + 158, 28))
    o.append(text(PX + 48, y + 170, "No free space? Shrink the Windows partition first, in Windows'", 11))
    o.append(text(PX + 48, y + 186, "Disk Management. MyOS can't resize NTFS.", 11))
    return page(3, "".join(o) + buttons())


def p5():
    o = [heading("disks", "Partitions", "What MyOS will make in the free space. Nothing is written yet.")]
    y = PY + 66
    o.append(text(PX, y, "Before", 12, C["text_soft"]))
    o.append(disk_bar(PX, y + 8, PW, [(0.034, "#7d9bb8", "", ""), (0.76, "#1f6fd1", "Windows (C:)", "390 GB"),
                                       (0.19, "#c9c2b2", "Free", "98 GB"), (0.016, "#5b6573", "", "")]))
    o.append(text(PX, y + 66, "After", 12, C["text_soft"]))
    o.append(disk_bar(PX, y + 74, PW, [(0.034, "#7d9bb8", "", ""), (0.76, "#1f6fd1", "Windows (C:)", "390 GB"),
                                        (0.06, C["title_mid"], "root", "32 GB"), (0.13, C["green_3"], "myos-data", "66 GB"),
                                        (0.016, "#5b6573", "", "")]))
    rows = [[("EFI system partition", 210), ("260 MB", 80), ("kept; MyOS adds EFI/MYOS", 200)],
            [("Windows (C:)", 210), ("390 GB", 80), ("not touched", 200)],
            [("myos-root  (ext2, /)", 210), ("32 GB", 80), ("new: the system", 200)],
            [("myos-data  (ext2, /home)", 210), ("66 GB", 80), ("new: your files", 200)]]
    o.append(list_rows(PX, y + 132, PW, rows, 2, 28))
    yy = y + 132 + 4 * 28 + 24
    o.append(field(PX, yy, 330, "System size:", "32 GB"))
    o.append(check(PX, yy + 40, True, "Keep my files on a separate partition (myos-data)"))
    o.append(check(PX, yy + 64, False, "Encrypt the data partition (later: not available yet)"))
    return page(4, "".join(o) + buttons(extra="Edit partitions..."))


def p6():
    o = [heading("users", "Your account", "You'll use it to log in. It can also change settings.")]
    y = PY + 66
    o.append(T.user_picture("rodger", PX + PW - 80, y, 72))
    o.append(text(PX + PW - 44, y + 92, "Change...", 11, C["label_teal"], anchor="middle"))
    o.append(field(PX, y, PW - 100, "Your name:", "Rodger Wilson"))
    o.append(field(PX, y + 40, PW - 100, "User name:", "rodger", hint="Lower case, for logging in and your folder."))
    o.append(field(PX, y + 100, PW - 100, "Password:", "pass1234", focused=True, secret=True))
    o.append(field(PX, y + 140, PW - 100, "Again:", "", secret=True))
    o.append('<rect x="%s" y="%s" width="%s" height="6" rx="3" fill="%s"/>' % (PX + 150, y + 176, 180, C["border"]))
    o.append('<rect x="%s" y="%s" width="%s" height="6" rx="3" fill="%s"/>' % (PX + 150, y + 176, 110, C["amber_mid"]))
    o.append(text(PX + 340, y + 182, "Fair: add a word or two", 11, C["text_soft"]))
    o.append(field(PX, y + 206, PW - 100, "Computer name:", "rodger-msi"))
    o.append(check(PX, y + 254, True, "Ask for my password when I log in"))
    o.append(check(PX, y + 278, False, "Also make a guest account (no password, nothing kept)"))
    o.append(text(PX, y + 318, "The root account will be locked. Your account is the administrator (doas).",
                  11, C["text_soft"]))
    return page(5, "".join(o) + buttons())


def p7():
    o = [heading("about", "Ready to install", "Check this. Nothing has been written to the disk yet.")]
    rows = [("Language", "English (United States), with Chinese fonts and pinyin"),
            ("Keyboard", "English (US)"),
            ("Time zone", "Asia/Shanghai; clock from the network"),
            ("Account", "Rodger Wilson (rodger), administrator; computer rodger-msi"),
            ("Disk", "Samsung SSD 980, 512 GB (NVMe)"),
            ("Will create", "myos-root 32 GB (ext2, /), myos-data 66 GB (ext2, /home)"),
            ("Will add", "EFI/MYOS on the EFI partition, and a \"MyOS\" startup entry"),
            ("Won't touch", "Windows (C:), 390 GB, and the rest of the disk"),
            ("Desktop effects", "Full (measured: 1.9 ms to redraw the screen)")]
    y = PY + 60
    o.append(card(PX, y, PW, len(rows) * 30 + 12))
    for k, (a, b) in enumerate(rows):
        o.append(text(PX + 14, y + 26 + k * 30, a, 12, C["label_teal"], "bold"))
        o.append(text(PX + 140, y + 26 + k * 30, b, 12))
    yy = y + len(rows) * 30 + 30
    o.append(check(PX, yy, True, "Copy the Wi-Fi network I'm using now (wilson1)"))
    o.append(card(PX, yy + 30, PW, 44, "#fde3d4", 1, C["amber_rim"]))
    o.append(img("lock", PX + 10, yy + 38, 28))
    o.append(text(PX + 48, yy + 50, "Clicking Install writes to the Samsung SSD. You can't undo it.", 12,
                  C["amber_rim"], "bold"))
    o.append(text(PX + 48, yy + 66, "Back up anything you care about first.", 11, C["text"]))
    return page(6, "".join(o) + buttons(next_label="Install"))


def p8():
    o = [heading("updates", "Installing MyOS", "This takes about 5 minutes. Keep the stick plugged in.")]
    y = PY + 70
    steps = [("Making partitions", True), ("Making file systems", True), ("Copying the system", None),
             ("Setting up your account and language", False), ("Making the disk start MyOS", False),
             ("Cleaning up", False)]
    for k, (name, done) in enumerate(steps):
        yy = y + k * 30
        if done:
            o.append('<circle cx="%s" cy="%s" r="9" fill="%s"/>' % (PX + 9, yy + 8, C["green_3"]))
            o.append('<path d="M%s %s l3 3 l6 -7" fill="none" stroke="#fff" stroke-width="2.2"/>' % (PX + 5, yy + 9))
        elif done is None:
            o.append('<circle cx="%s" cy="%s" r="9" fill="none" stroke="%s" stroke-width="3" '
                     'stroke-dasharray="20 40"/>' % (PX + 9, yy + 8, C["label_teal"]))
        else:
            o.append('<circle cx="%s" cy="%s" r="8" fill="none" stroke="%s"/>' % (PX + 9, yy + 8, C["border"]))
        o.append(text(PX + 28, yy + 13, name, 13, C["text"] if done is not False else C["text_soft"],
                      "bold" if done is None else "normal"))
    yy = y + len(steps) * 30 + 20
    o.append('<rect x="%s" y="%s" width="%s" height="22" rx="11" fill="#ffffff" stroke="%s"/>'
             % (PX, yy, PW, C["border"]))
    o.append('<rect x="%s" y="%s" width="%s" height="20" rx="10" fill="url(#prog)"/>'
             % (PX + 1, yy + 1, PW * 0.46))
    o.append(text(PX, yy + 44, "Copying /usr/lib/chromium ...   1.1 GB of 2.4 GB", 12, C["text_soft"]))
    o.append(text(PX + PW, yy + 44, "about 3 minutes left", 12, C["text_soft"], anchor="end"))
    o.append(card(PX, yy + 70, PW, 70, "#ffffff", 0.6))
    o.append(img("my-files", PX + 14, yy + 82, 44))
    o.append(text(PX + 72, yy + 98, "Your files have their own partition", 13, weight="bold"))
    o.append(text(PX + 72, yy + 118, "Reinstalling MyOS later keeps /home: your documents stay.", 12, C["text_soft"]))
    return page(7, "".join(o) + buttons(back=False, next_label="Next", next_state="normal"))


def p9():
    o = [heading("logo", "MyOS is installed", "Remove the USB stick, then restart.")]
    y = PY + 76
    o.append(card(PX, y, PW, 150))
    o.append(img("turn-off", PX + 20, y + 20, 56))
    o.append(text(PX + 92, y + 40, "When the computer starts, choose MyOS:", 13, weight="bold"))
    o.append(text(PX + 92, y + 62, "It is the first choice now. Windows is still there: press F11 while", 12))
    o.append(text(PX + 92, y + 80, "the MSI logo shows and pick \"Windows Boot Manager\".", 12))
    o.append(text(PX + 92, y + 110, "Your account: rodger. Your files go in /home/rodger.", 12, C["text_soft"]))
    o.append(check(PX, y + 176, True, "Save the install log to the stick (/myos-install.log)"))
    return page(8, "".join(o) + buttons(back=False, next_label="Restart now", extra="Keep trying MyOS"))


def main():
    os.makedirs(OUT, exist_ok=True)
    for k, fn in enumerate([p1, p2, p3, p4, p5, p6, p7, p8, p9], 1):
        name = "installWizard%02d" % k
        svg = os.path.join(HERE, name + ".svg")
        open(svg, "w").write(fn())
        T.render(name + ".svg", os.path.join(OUT, name + ".png"))
    print("9 installer mockups in", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
