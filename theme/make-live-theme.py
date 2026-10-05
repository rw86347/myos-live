#!/usr/bin/env python3
"""make-live-theme.py — the Golden Hour theme as files the live disk uses.

assets/themes/golden-hour/make-theme.py is the design: one palette, and a
table that says how every element looks in every state. It draws the
pictures people look at (mockup.png, login.png, the icon sheet). This
script takes the same palette and table and writes what the machine needs:

  usr/share/icewm/themes/GoldenHour/   the window manager's theme (100%)
  usr/share/icewm/themes/GoldenHour-150/   ... and at each larger scale
  usr/share/myos/theme/wallpaper.jpg   the desktop background
  usr/share/myos/login/                the login screen's pictures
  usr/share/myos/menu/150/             the start menu's pictures and layout,
                                       and the taskbar tray, one folder per scale
  usr/share/myos/icons/                the icon set as SVG, for My Files

into build/live/extra, which tools/fetch-live.py copies into the disk
image on top of the packages. So the look has one source: change a color
in make-theme.py and both the mockup and the running desktop follow.

The mockup is a 1024x768 screen. On a bigger, sharper one the desktop is
drawn bigger by a scale (Settings > Displays; 150% on a 1920x1200 screen,
see live/myos-settings/scale.h). Fonts follow it through Xft.dpi, but
IceWM's title bars, buttons and taskbar are pictures of a fixed size, so
they are drawn here once for every scale: the drawings keep the mockup's
numbers and come out K times as many pixels. myos-xsettings tells IceWM
which theme folder to use, and myos-menu picks its folder the same way.

Needs rsvg-convert (Homebrew: librsvg), like make-theme.py.
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
THEME_DIR = os.path.join(ROOT, "assets", "themes", "golden-hour")
OUT = os.path.join(ROOT, "build", "live", "extra")
WORK = os.path.join(ROOT, "build", "live", "theme-svg")

# The design, imported as a module (its file name has a dash in it).
_spec = importlib.util.spec_from_file_location("make_theme", os.path.join(THEME_DIR, "make-theme.py"))
mt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mt)
C, S, G = mt.C, mt.S, mt.G

THEMES = os.path.join(OUT, "usr/share/icewm/themes")
LOGIN = os.path.join(OUT, "usr/share/myos/login")
WALL = os.path.join(OUT, "usr/share/myos/theme")
MENUS = os.path.join(OUT, "usr/share/myos/menu")
ICONS = os.path.join(OUT, "usr/share/myos/icons")

# Settings offers these, in percent; anything between is rounded to one.
SCALES = (100, 125, 150, 175, 200)
K = 1                   # the scale being drawn; set_scale() sets it
ICEWM = MENU = None     # its theme and menu folders

TITLE_H = 30            # the mockup's title bar
BUTTON = 21             # the round buttons in it
BAR_H = 40              # the taskbar
TRAY_W = 150            # the mockup's tray: the last 150 px of the taskbar
RADIUS = 8              # the window's corners, in the mockup
STROKE = 1.5            # the dark edge around them


def set_scale(pct):
    global K, ICEWM, MENU
    K = pct / 100
    ICEWM = os.path.join(THEMES, "GoldenHour" if pct == 100 else "GoldenHour-%d" % pct)
    MENU = os.path.join(MENUS, str(pct))


def px(v):
    """A length in the mockup's pixels, in pixels at this scale. Rounded
    half up (Python's round() would make 1.5 and 2.5 both 2)."""
    return int(v * K + 0.5)

# The pictures the login program draws, taken from the icon set.
LOGIN_ICONS = {"logo": 192, "turn-off": 64, "all-programs": 64}


def svg(body, w, h, defs=True):
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'viewBox="0 0 %g %g" width="%g" height="%g"><defs>%s</defs>%s</svg>\n'
            % (w, h, w, h, mt.COMMON_DEFS if defs else "", body))


def render(name, body, w, h, out, ow=None, oh=None, exact=False):
    """Write an SVG into the work folder and turn it into a PNG. Relative
    image references (the wallpaper) resolve there, so the work folder has
    a copy of everything a drawing uses. The drawing is w x h in the
    mockup's pixels (or ow x oh); the PNG is that at the current scale,
    unless exact says ow x oh are already the PNG's own pixels."""
    path = os.path.join(WORK, name + ".svg")
    open(path, "w").write(svg(body, w, h))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    pw, ph = (ow, oh) if exact else (px(ow or w), px(oh or h))
    subprocess.run(["rsvg-convert", path, "-w", str(pw), "-h", str(ph), "-o", out],
                   check=True, cwd=WORK)


# --- the window manager's theme ----------------------------------------------------

def gradient_strip(state, w, h):
    """A title bar's gradient, as a strip IceWM repeats sideways. The nine
    pieces of a title bar (left, title, middle, right...) are all this same
    strip: the gradient runs down, so repeating it across loses nothing —
    as long as it has no left and right edge of its own, or every repeat
    would show as a dark line."""
    st = S("Window title bar", state)
    body = mt.draw_bar(dict(st, rx=0, border="none", bw=0, shine=0), 0, 0, w, h)
    # The gloss across the top, drawn here rather than by draw_bar, which
    # keeps it 2 px clear of the left and right edges — on a strip this
    # narrow that gap is a notch at every repeat.
    return body + ('<rect x="0" y="0" width="%g" height="%g" fill="url(#shine)" opacity="%s"/>'
                   % (w, h * 0.42, st["shine"]))


def title_bars():
    """titleAL, titleAS, ... titleAJ: IceWM's nine title pieces, active (A)
    and inactive (I). Same strip for each, so the bar is one gradient.
    IceWM keeps the width of each piece it doesn't stretch, so the ones left
    of the window icon (J, the left end: the buttons start at its width)
    and between it and the title (L, S, P) are 1 px. The icon lands 9 px in
    and the title about 37 (the mockup: 8 and 32; IceWM makes the icon's
    button as wide as the bar is high)."""
    for state, letter in (("active", "A"), ("inactive", "I")):
        for piece in "LSPTMBRQJ":
            w = 1 if piece in "JLSP" else 8
            render("title%s" % letter, gradient_strip(state, w, TITLE_H), w, TITLE_H,
                   os.path.join(ICEWM, "title%s%s.png" % (letter, piece)))


def title_buttons():
    """The round buttons: teal for minimize/maximize, amber for close.
    A = the focused window, I = an unfocused one, O = under the mouse.
    Each is the full height of the title bar with the bar's own gradient
    behind it, so there is nothing to see through: IceWM turns a picture's
    transparency into a one-bit mask, which would leave the round corners
    black.

    Each picture holds two buttons, one above the other: as it rests, and
    pressed. IceWM (YFrameButton::paint) shows the top half, or the bottom
    half while the mouse button is held, so a one-state picture would
    leave the button's lower half unpainted (black)."""
    pad = (TITLE_H - BUTTON) / 2
    for name, element in (("close", "Close button"),
                          ("maximize", "Minimize and maximize buttons"),
                          ("minimize", "Minimize and maximize buttons"),
                          ("restore", "Minimize and maximize buttons")):
        glyph = "close" if name == "close" else "maximize" if name in ("maximize", "restore") else "minimize"
        for letter, state in (("A", "normal"), ("I", "inactive"), ("O", "hover")):
            bar = gradient_strip("inactive" if letter == "I" else "active", TITLE_H, TITLE_H)
            body = (bar + mt.draw_ball(S(element, state), glyph, pad, pad, BUTTON) +
                    '<g transform="translate(0,%d)">%s%s</g>'
                    % (TITLE_H, bar, mt.draw_ball(S(element, "pressed"), glyph, pad, pad, BUTTON)))
            # Exactly twice the bar in pixels, so the halves stay equal at
            # scales where the bar's height isn't a whole number.
            render("btn", body, TITLE_H, 2 * TITLE_H, os.path.join(ICEWM, "%s%s.png" % (name, letter)),
                   px(TITLE_H), 2 * px(TITLE_H), exact=True)


def menu_button():
    """The window menu button's background, under the window's icon: plain
    title bar, twice over (resting and pressed, see title_buttons). IceWM
    makes the button as wide as the bar is high and centres the icon in it;
    a narrower picture would leave the rest of the button unpainted."""
    w = TITLE_H
    for letter, state in (("A", "active"), ("I", "inactive"), ("O", "active")):
        bar = gradient_strip(state, w, TITLE_H)
        render("menubutton", bar + '<g transform="translate(0,%d)">%s</g>' % (TITLE_H, bar),
               w, 2 * TITLE_H, os.path.join(ICEWM, "menuButton%s.png" % letter),
               px(w), 2 * px(TITLE_H), exact=True)


# The theme's icons, for IceWM: (name in the icon set, name IceWM asks for).
# IceWM gets a window's icon from /etc/icewm/winoptions first, then from the
# program, and falls back to one called "icewm" — here the MyOS logo.
ICEWM_ICONS = [("terminal", "terminal"), ("text-editor", "text-editor"),
               ("logo", "icewm"), ("run", "run"), ("writeperfect", "writeperfect"),
               ("preview", "preview"), ("settings", "settings"), ("folder", "folder"),
               ("web-browser", "firefox"), ("web-browser", "Navigator")]
ICON_SIZES = (16, 18, 32, 48)       # menus, title bars and taskbar, Alt+Tab

# Programs that bring their own icon rather than one from the set.
OWN_ICONS = {"writeperfect": os.path.join(ROOT, "live", "writeperfect", "writeperfect.svg"),
             "preview": os.path.join(ROOT, "live", "myos-preview", "preview.svg")}


def own_icons():
    """Into the work folder's copy of the icon set, as if they were part of
    it: the SVG, and the 96 px PNG the menu drawings refer to."""
    for name, src in OWN_ICONS.items():
        shutil.copy(src, os.path.join(WORK, "icons/svg/%s.svg" % name))
        subprocess.run(["rsvg-convert", src, "-w", "96", "-h", "96",
                        "-o", os.path.join(WORK, "icons/png/96/%s.png" % name)], check=True)


def window_icons():
    """In the theme's icons/ folder, which IceWM searches before its own,
    named the way it looks for them: name_18x18.png and so on."""
    out = os.path.join(ICEWM, "icons")
    os.makedirs(out, exist_ok=True)
    for src, name in ICEWM_ICONS:
        for s in map(px, ICON_SIZES):
            subprocess.run(["rsvg-convert", os.path.join(WORK, "icons/svg/%s.svg" % src),
                            "-w", str(s), "-h", str(s),
                            "-o", os.path.join(out, "%s_%dx%d.png" % (name, s, s))], check=True)


def mix(a, b, t):
    """The colour t of the way from a to b ('#rrggbb')."""
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(ca, cb))


def bar_style():
    """The taskbar, one colour from end to end. The mockup sweeps from navy
    on the left to teal on the right, but IceWM draws the taskbar as
    separate panes (the Start button, the window buttons, the clock) and
    stretches the picture over each one on its own, so a sweep would start
    again at every pane and show a seam. The top-to-bottom shading and the
    light top edge stay; the colour is the sweep's, a third of the way
    along, where most of the window buttons sit."""
    return dict(S("Taskbar", "normal"), fill=mix(C["bar_left"], C["bar_right"], 0.3))


def start_size():
    """The Start button on the bar: the design's, scaled to the bar's height."""
    return int(mt.START_W * BAR_H / mt.START_H), BAR_H


def start_picture(state, out):
    """The Start button with the bar painted in behind its tapered end."""
    scale = BAR_H / mt.START_H
    behind = '<g transform="scale(%g)">%s</g>' % (
        1 / scale, mt.draw_bar(bar_style(), 0, 0, mt.START_W * scale, BAR_H))
    w, h = start_size()
    render("start-" + state, behind + mt.start_button(state), mt.START_W, mt.START_H, out, w, h)


def taskbar():
    bar = os.path.join(ICEWM, "taskbar")
    strip = mt.draw_bar(bar_style(), 0, 0, 8, BAR_H)
    render("taskbarbg", strip, 8, BAR_H, os.path.join(bar, "taskbarbg.png"))
    render("toolbuttonbg", strip, 8, BAR_H, os.path.join(bar, "toolbuttonbg.png"))
    # IceWM's Start button only holds the place: myos-menu draws the real
    # one inside it (IceWM puts a Windows-style bevel around every taskbar
    # button in this look, which no setting turns off). IceWM makes a button
    # its picture plus 5 px each way, so this plain piece of bar gives a
    # button exactly the size of the Start picture, and a bar BAR_H high.
    w, h = start_size()
    render("start", mt.draw_bar(bar_style(), 0, 0, w - 5, h - 5), w - 5, h - 5,
           os.path.join(bar, "start.png"), px(w) - 5, px(h) - 5, exact=True)
    for name, state in (("taskbuttonbg", "normal"), ("taskbuttonactive", "active"),
                        ("taskbuttonminimized", "normal")):
        render("taskbutton", mt.draw_bar(S("Task button", state), 0, 0, 160, 30), 160, 30,
               os.path.join(bar, name + ".png"))
    for name, state in (("workspacebuttonbg", "normal"), ("workspacebuttonactive", "active")):
        render("wsbutton", mt.draw_bar(S("Task button", state), 0, 0, 30, 30), 30, 30,
               os.path.join(bar, name + ".png"))


def write_png(path, w, h, channels, rows):
    """A PNG with no extra dependencies. channels is 3 (RGB) or 4 (RGBA);
    rows are packed bytes, one per line."""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))
    raw = b"".join(b"\x00" + row for row in rows)
    color = 6 if channels == 4 else 2
    ihdr = struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def frames():
    """The window's outline, so IceWM's frames follow the mockup.

    The mockup's window is one rounded rectangle: 8 px corners, a 1.5 px
    dark edge, and a soft shadow that shows mostly on the left and right of
    the green bar. IceWM paints a frame out of pictures and clips the window
    to their masks. The mask is one bit — a pixel is in the window or it is
    a hole — so the corner can be round (the wallpaper shows through the
    cut) and the edge can be that dark stroke. The shadow is a blur on
    whatever is behind the window; a one-bit mask cannot darken it. That
    waits for the compositor.

    A corner picture is the quarter-circle only. Its transparent pixels are
    the hole; everything inside the circle is the stroke colour, and the
    title bar and the program cover the inside, leaving the stroke. The
    straight edges are solid strips of the same colour, as wide or as tall
    as the border, so they meet the corners. Dialogs (dframe) get the same
    pictures: a Run box is a window too.

    ShapesProtectClientWindow is off in IceWM's preferences (it is not a
    theme setting, so a line here would be ignored). Left on, IceWM fills
    the corner back in over the program, and the bottom stays square: only
    the thin border follows the curve.
    """
    r = px(RADIUS)
    b = max(px(STROKE), 1)
    # The circle's centre, in a picture of the corner. It sits on the inner
    # corner: top-left's centre is the picture's bottom-right, and so on.
    centre = {"TL": (r, r), "TR": (0, r), "BL": (r, 0), "BR": (0, 0)}

    def corner_rows(name, rgb):
        cx, cy = centre[name]
        rows = []
        for y in range(r):
            row = bytearray()
            for x in range(r):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                    row += rgb + b"\xff"
                else:
                    row += b"\x00\x00\x00\x00"
            rows.append(bytes(row))
        return rows

    for state, letter in (("active", "A"), ("inactive", "I")):
        rgb = bytes(int(S("Window title bar", state)["frame"][i:i + 2], 16) for i in (1, 3, 5))
        for name in centre:
            path = os.path.join(ICEWM, "frame%s%s.png" % (letter, name))
            write_png(path, r, r, 4, corner_rows(name, rgb))
            shutil.copy(path, os.path.join(ICEWM, "dframe%s%s.png" % (letter, name)))
        # Straight edges, solid, b px each way: the top and bottom are b tall
        # and the sides are b wide, which is the border, so they meet the
        # corners. IceWM repeats whichever way the edge runs.
        for name in ("T", "B", "L", "R"):
            path = os.path.join(ICEWM, "frame%s%s.png" % (letter, name))
            write_png(path, b, b, 3, [rgb * b for _ in range(b)])
            shutil.copy(path, os.path.join(ICEWM, "dframe%s%s.png" % (letter, name)))


def menus():
    render("menubg", '<rect width="100%%" height="100%%" fill="%s"/>' % C["panel_white"], 16, 16,
           os.path.join(ICEWM, "menubg.png"))
    render("menusel", mt.draw_bar(dict(S("Menu highlight", "hover"), rx=0), 0, 0, 16, 22), 16, 22,
           os.path.join(ICEWM, "menusel.png"))
    render("dialogbg", '<rect width="100%%" height="100%%" fill="%s"/>' % C["panel_white"], 16, 16,
           os.path.join(ICEWM, "dialogbg.png"))


def rgb(c):
    """'#1f7f86' the way an IceWM theme writes a color."""
    return 'rgb:%s/%s/%s' % (c[1:3], c[3:5], c[5:7])


THEME_FILE = """\
# default.theme — "Golden Hour", the MyOS desktop.
#
# Written by live/theme/make-live-theme.py from the palette in
# assets/themes/golden-hour/make-theme.py; don't edit this file, edit the
# palette. The pictures next to it (the taskbar's gradient, the Start
# button, the round title buttons) come from the same place.
ThemeDescription="Golden Hour — the MyOS desktop, at {pct}%"
ThemeAuthor="MyOS"

# "pixmap" is the only look that uses a theme's pictures at all: the flat
# and metal looks draw plain rectangles from the colours below.
Look="pixmap"
# The title bar's nine pieces are one gradient strip each, so IceWM can
# repeat them side by side and the bar stays one gradient. These others
# are listed as gradients, which means stretch rather than repeat: the
# taskbar's light-to-dark sweep has to cross the whole screen once.
# (IceWM asks for every theme picture by its .xpm name and then tries
# .png, so that is the name to list here, whatever the file is called.)
Gradients="taskbarbg.xpm toolbuttonbg.xpm taskbuttonbg.xpm taskbuttonactive.xpm \
taskbuttonminimized.xpm workspacebuttonbg.xpm workspacebuttonactive.xpm \
menubg.xpm menusel.xpm dialogbg.xpm"

TitleBarHeight={title_h}
TitleBarJustify=0
TitleBarCentered=0
# "s" is the window menu button, which IceWM draws as the window's icon:
# the mockup's 18 px icon at the left of every title bar.
TitleButtonsLeft="s"
TitleButtonsRight="xmi"
TitleButtonsSupported="xmis"
RolloverButtonsSupported=1
# The 1.5 px edge around a window. The corner pictures round it to the
# mockup's 8 px, top and bottom. CornerSize is how big a corner you can
# grab to resize; the round itself is the picture. Whether the program
# inside is cut to that curve, and whether a maximized window drops the
# edge, are preferences (live/overlay/etc/icewm/preferences): IceWM does
# not read those two from a theme file.
BorderSizeX={border}
BorderSizeY={border}
DlgBorderSizeX={border}
DlgBorderSizeY={border}
CornerSizeX={corner}
CornerSizeY={corner}
MenuIconSize={icon16}
SmallIconSize={icon18}
LargeIconSize={icon32}
HugeIconSize={icon48}

# Windows
ColorActiveBorder="{title_edge}"
ColorNormalBorder="{inactive_frame}"
ColorActiveTitleBar="{title_mid}"
ColorNormalTitleBar="{inactive_title}"
ColorActiveTitleBarText="{title_text}"
ColorNormalTitleBarText="{title_text}"
ColorActiveTitleBarShadow="{title_edge}"
ColorDialog="{panel_white}"
ColorNormalButton="{menu_bar}"
ColorNormalButtonText="{text}"
ColorActiveButton="{menu_bar}"
ColorActiveButtonText="{text}"

# Menus
ColorNormalMenu="{panel_white}"
ColorNormalMenuItemText="{text}"
ColorActiveMenuItem="{menu_hi}"
ColorActiveMenuItemText="#ffffff"
ColorDisabledMenuItemText="{text_soft}"
ColorMenuItemText="{text}"

# The taskbar
ColorDefaultTaskBar="{bar}"
ColorNormalTaskBarApp="{task_app}"
ColorNormalTaskBarAppText="#ffffff"
ColorActiveTaskBarApp="{task_active}"
ColorActiveTaskBarAppText="#ffffff"
ColorMinimizedTaskBarApp="{task_app}"
ColorMinimizedTaskBarAppText="#ffffff"
ColorNormalWorkspaceButton="{task_app}"
ColorNormalWorkspaceButtonText="#ffffff"
ColorActiveWorkspaceButton="{task_active}"
ColorActiveWorkspaceButtonText="#ffffff"
ColorToolButton="{bar}"
ColorToolButtonText="#ffffff"
ColorClock="{bar_right}"
ColorClockText="#ffffff"

# Lists, scroll bars and the little status window while a window is moved
ColorListBox="{panel_white}"
ColorListBoxText="{text}"
ColorListBoxSelection="{menu_hi}"
ColorListBoxSelectionText="#ffffff"
ColorScrollBar="{menu_bar}"
ColorScrollBarSlider="{teal_btn_bottom}"
ColorScrollBarButton="{menu_bar}"
ColorScrollBarButtonArrow="{text}"
ColorMoveSizeStatus="{panel_sand}"
ColorMoveSizeStatusText="{text}"
ColorQuickSwitch="{bar_left}"
ColorQuickSwitchText="#ffffff"
ColorQuickSwitchActive="{amber_mid}"
ColorToolTip="#fff7dd"
ColorToolTipText="{text}"

# Verdana is the mockup's face; DejaVu Sans is what the disk has.
TitleFontNameXft="DejaVu Sans:size=10:bold"
MenuFontNameXft="DejaVu Sans:size=10"
NormalTaskBarFontNameXft="DejaVu Sans:size=9"
ActiveTaskBarFontNameXft="DejaVu Sans:size=9:bold"
StatusFontNameXft="DejaVu Sans:size=9"
ListBoxFontNameXft="DejaVu Sans:size=10"
ToolTipFontNameXft="DejaVu Sans:size=9"
QuickSwitchFontNameXft="DejaVu Sans:size=11:bold"
ClockFontNameXft="DejaVu Sans:size=9:bold"
"""


def theme_file():
    text = THEME_FILE.format(
        pct=round(K * 100), title_h=px(TITLE_H), border=px(STROKE), corner=px(20),
        icon16=px(16), icon18=px(18), icon32=px(32), icon48=px(48),
        title_edge=rgb(C["title_edge"]), title_mid=rgb(C["title_mid"]),
        title_text=rgb(C["title_text"]), inactive_title=rgb("#8fb5b9"),
        inactive_frame=rgb("#8aa9ac"), panel_white=rgb(C["panel_white"]),
        menu_bar=rgb(C["menu_bar"]), text=rgb(C["text"]), text_soft=rgb(C["text_soft"]),
        menu_hi=rgb("#1f7f86"), bar_left=rgb(C["bar_left"]), bar_right=rgb(C["bar_right"]), bar=rgb(bar_style()["fill"]),
        task_app=rgb("#1d4a7c"), task_active=rgb("#132e55"),
        teal_btn_bottom=rgb(C["teal_btn_bottom"]), panel_sand=rgb(C["panel_sand"]),
        amber_mid=rgb(C["amber_mid"]))
    open(os.path.join(ICEWM, "default.theme"), "w").write(text)


# --- the login screen's pictures ----------------------------------------------------

def login_background():
    """The wallpaper, blurred and tinted, as one picture. The login program
    scales it to the screen and draws the bands, the logo and the users on
    top itself, so text stays sharp whatever the screen's size."""
    w, h = 1024, 768
    body = ('<image href="wallpaper.jpg" x="-20" y="-20" width="%d" height="%d" '
            'preserveAspectRatio="xMidYMid slice" filter="url(#blur)"/>' % (w + 40, h + 40)
            + mt.draw_bar(S("Login background", "normal"), 0, 0, w, h))
    defs = ('<filter id="blur" x="-5%" y="-5%" width="110%" height="110%">'
            '<feGaussianBlur stdDeviation="6"/></filter>')
    path = os.path.join(WORK, "login-bg.svg")
    open(path, "w").write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
                          'height="%d"><defs>%s</defs>%s</svg>\n' % (w, h, w, h, defs, body))
    os.makedirs(LOGIN, exist_ok=True)
    subprocess.run(["rsvg-convert", path, "-o", os.path.join(LOGIN, "background.png")],
                   check=True, cwd=WORK)


def login_pictures():
    for name, size in LOGIN_ICONS.items():
        subprocess.run(["rsvg-convert", os.path.join(THEME_DIR, "icons/svg/%s.svg" % name),
                        "-w", str(size), "-h", str(size),
                        "-o", os.path.join(LOGIN, "%s.png" % name)], check=True)
    # The picture shown for a user who hasn't chosen one.
    render("user", mt.user_picture("guest", 0, 0, 128), 128, 128, os.path.join(LOGIN, "user.png"))


# --- the start menu ---------------------------------------------------------------
# IceWM's own menu is one list; the mockup's is two columns under a header
# with the user's name, with Log off and Turn off along the bottom. So the
# menu is a program of ours (live/myos-menu), and its pictures are drawn
# here with the design's own functions, in the design's own fonts: the
# whole menu once, and each entry once more as it looks under the mouse.
# The program only adds the user's name and swaps those pieces in.
#
# Only entries that do something are listed. The mockup's Disks, Network,
# Help and All Programs come back as the programs behind them are written.

MENU_W = 390
MENU_PAD = 8            # room above the menu for its shadow
MENU_LEFT = [           # (icon, name, what it does, command)
    ("terminal", "Terminal (bash)", "Command line shell", "xterm"),
    ("my-files", "My Files", "Browse your files", "myos-files"),
    ("web-browser", "Web Browser", "Browse the web", "firefox"),
    ("writeperfect", "WritePerfect", "WordPerfect 5.1 style", "writeperfect"),
    ("text-editor", "AbiWord", "Write documents", "abiword"),
    ("preview", "Preview", "PDFs and pictures", "myos-preview"),
]
MENU_RIGHT = [          # (icon, name, command); "@run" is the menu's own box
    ("my-files", "My Files", "myos-files"),
    ("disks", "Disks", "myos-files :disks"),
    ("settings", "Settings", "myos-settings"),
    ("run", "Run...", "@run"),
]
MENU_FOOT = [           # (icon, name, x, command)
    ("log-off", "Log off", 170, "icesh logout"),
    ("turn-off", "Turn off", 276, "doas /sbin/poweroff"),
]
SAND = ('<linearGradient id="sand" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
        '<stop offset="1" stop-color="%s"/></linearGradient>' % (C["panel_sand"], C["panel_sand_deep"]))
FOOT_HOVER = dict(fill=G("h", (0, "#ffffff", 0.22), (1, "#ffffff", 0.05)), border=C["bar_edge"], rx=6)


def menu_layout():
    """Where everything goes, in the mockup's numbers; y=0 is the menu's top."""
    body = max(10 + 60 * len(MENU_LEFT) + 6, 12 + 52 * len(MENU_RIGHT) + 6, 136)
    mh = 56 + body + 46
    rows = []                       # (x, y, w, h, command, how to draw it hovered)
    for k, row in enumerate(MENU_LEFT):
        y = 56 + 10 + k * 60
        rows.append((6, y - 6, 198, 52, row[3], ("left", k)))
    for k, row in enumerate(MENU_RIGHT):
        y = 56 + 12 + k * 52
        rows.append((216, y - 6, MENU_W - 228, 44, row[2], ("right", k)))
    for k, row in enumerate(MENU_FOOT):
        rows.append((row[2] - 8, mh - 46 + 4, 100, 38, row[3], ("foot", k)))
    return mh, rows


def menu_svg(hover=None):
    """The open menu, with nothing or one entry under the mouse."""
    mh, rows = menu_layout()
    mx, my, mw = 0, MENU_PAD, MENU_W
    hl = S("Menu highlight", "hover")
    head = S("Window title bar", "active")
    o = ['<defs>%s</defs>' % SAND,
         '<g filter="url(#shadow-active)"><rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="%s" '
         'stroke="%s" stroke-width="1.5"/></g>' % (mx, my, mw, mh, C["panel_white"], head["frame"])]
    fill, d = mt.grad(head["fill"])
    o.append(d + '<path d="M%d %d a8 8 0 0 1 8-8 h%d a8 8 0 0 1 8 8 v48 h-%d z" fill="%s" stroke="%s" '
             'stroke-width="1.5"/>' % (mx, my + 8, mw - 16, mw, fill, head["border"]))
    o.append('<rect x="%d" y="%d" width="%d" height="20" rx="8" fill="url(#shine)" opacity="0.3"/>'
             % (mx + 6, my + 3, mw - 12))
    o.append(mt.img("logo", mx + 10, my + 8, 40))
    cy0, cy1 = my + 56, my + mh - 46
    o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="url(#sand)"/>' % (mx + 210, cy0, mw - 211, cy1 - cy0))
    o.append('<path d="M%d %dV%d" stroke="%s"/>' % (mx + 210, cy0, cy1, C["border"]))
    for k, (icon, label, sub, _) in enumerate(MENU_LEFT):
        y = cy0 + 10 + k * 60
        on = hover == ("left", k)
        if on:
            o.append(mt.draw_bar(hl, mx + 6, y - 6, 198, 52))
        o.append(mt.img(icon, mx + 12, y, 40))
        o.append(mt.text(mx + 60, y + 18, label, 13, hl["text"] if on else None, weight="bold"))
        o.append(mt.text(mx + 60, y + 34, sub, 11, "#e0f4f5" if on else C["text_soft"]))
    for k, (icon, label, _) in enumerate(MENU_RIGHT):
        y = cy0 + 12 + k * 52
        on = hover == ("right", k)
        if on:
            o.append(mt.draw_bar(hl, mx + 216, y - 6, mw - 228, 44))
        o.append(mt.img(icon, mx + 222, y, 32))
        o.append(mt.text(mx + 262, y + 21, label, 13, hl["text"] if on else C["label_teal"], "bold"))
    o.append(d + '<path d="M%d %d h%d v38 a8 8 0 0 1-8 8 h-%d a8 8 0 0 1-8-8 z" fill="%s" stroke="%s" '
             'stroke-width="1.5"/>' % (mx, cy1, mw, mw - 16, fill, head["border"]))
    for k, (icon, label, x, _) in enumerate(MENU_FOOT):
        if hover == ("foot", k):
            o.append(mt.draw_bar(FOOT_HOVER, mx + x - 8, cy1 + 4, 100, 38))
        o.append(mt.img(icon, mx + x, cy1 + 8, 30))
        o.append(mt.text(mx + x + 36, cy1 + 28, label, 13, "#ffffff", extra='filter="url(#labelshadow)"'))
    return "".join(o)


def run_svg():
    """Run...: a small window with a box to type a command into."""
    w, h = 380, 150
    x, y = MENU_PAD, MENU_PAD - 2
    st = S("Window title bar", "active")
    # A dialog: only a close button, like the mockup's windows otherwise.
    return ('<g filter="url(#shadow-active)"><rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="%s" '
            'stroke="%s" stroke-width="1.5"/></g>' % (x, y, w, h, C["panel_white"], st["frame"]) +
            mt.title_bar(st, x, y, w, "Run", "run", buttons=False) +
            mt.draw_ball(S("Close button", "normal"), "close", x + w - 30, y + 5, 21) +
            mt.text(x + 16, y + 56, "Type the name of a program, and MyOS will start it.", 12) +
            mt.draw_bar(S("Text box", "focused"), x + 16, y + 70, w - 32, 26) +
            mt.draw_bar(S("Push button", "default"), x + w - 172, y + h - 36, 75, 23, "OK") +
            mt.draw_bar(S("Push button", "normal"), x + w - 91, y + h - 36, 75, 23, "Cancel"))


def tray_picture(out):
    """The right end of the taskbar: its own top-to-bottom color, the dark
    divider, and the network and volume icons. myos-menu draws the time on
    top, so the picture has no clock baked in.

    IceWM can't paint this. Its clock is one flat color, only as wide as the
    text, and it stretches the taskbar picture across the whole bar, so a
    band at the right of that picture would be a fraction of the screen
    rather than these 150 px.

    The divider is the same 1.5 px stroke the mockup centers on the tray's
    left edge, so half of it darkens the bar and half the tray. The picture
    is one pixel wider than the tray for that left half; the pixel is the
    bar's own colour, and the tray (stroke included) starts at x=1. The
    window's right edge sits on the screen's right edge."""
    st = S("Tray", "normal")
    body = (mt.draw_bar(bar_style(), 0, 0, 1 + TRAY_W, BAR_H) +
            mt.draw_bar(st, 1, 0, TRAY_W, BAR_H) +
            mt.img("tray-network", 13, 11, 18) +
            mt.img("tray-volume", 39, 11, 18))
    render("tray", body, 1 + TRAY_W, BAR_H, out)


def start_menu():
    os.makedirs(MENU, exist_ok=True)
    mh, rows = menu_layout()
    cw, ch = MENU_W + 12, mh + MENU_PAD
    render("menu", menu_svg(), cw, ch, os.path.join(MENU, "menu.png"))
    conf = ["# The start menu at %d%%, for live/myos-menu. Written by live/theme/make-live-theme.py."
            % round(K * 100),
            "# Pictures are in this folder; x y are pixels from the top left of menu.png.",
            "menu menu.png",
            "name %d %d %d" % (px(56), px(MENU_PAD + 36), px(18)),
            "typesize %d" % px(13),
            "start start.png start-hover.png start-pressed.png"]
    for k, (x, y, w, h, cmd, where) in enumerate(rows):
        y += MENU_PAD
        # The entry as it looks under the mouse: the menu drawn with it lit,
        # cut down to its own rectangle. Cut at whole pixels of the scaled
        # menu, so it lands exactly on the picture underneath.
        crop = '<g transform="translate(%g,%g)">%s</g>' % (-px(x) / K, -px(y) / K, menu_svg(where))
        render("hl%d" % k, crop, w, h, os.path.join(MENU, "hl%d.png" % k))
        conf.append("item %d %d %d %d hl%d.png %s" % (px(x), px(y), px(w), px(h), k, cmd))

    # The Start button: resting, under the mouse, and held down while the
    # menu is open.
    for state, name in (("normal", "start"), ("hover", "start-hover"), ("pressed", "start-pressed")):
        start_picture(state, os.path.join(MENU, name + ".png"))

    # Run...: the box where the typing goes, and the two buttons.
    rw, rh = 380 + 2 * MENU_PAD, 150 + 2 * MENU_PAD
    render("run", run_svg(), rw, rh, os.path.join(MENU, "run.png"))
    x, y = MENU_PAD, MENU_PAD - 2
    def r4(*v):
        return " ".join(str(px(n)) for n in v)
    conf += ["run run.png",
             "runbox " + r4(x + 16, y + 70, 380 - 32, 26),
             "runok " + r4(x + 380 - 172, y + 150 - 36, 75, 23),
             "runcancel " + r4(x + 380 - 91, y + 150 - 36, 75, 23),
             "runclose " + r4(x + 380 - 30, y + 5, 21, 21)]
    # The hot corner: the wallpaper's top-right corner curled over, 24 px,
    # and 40 px while the mouse is in it. Room for the hover size plus the
    # flap's shadow; transparent elsewhere, so the wallpaper shows through.
    c = 44
    for state, name in (("normal", "corner"), ("hover", "corner-hover")):
        render(name, mt.curl(S("Hot corner", state), c, 0), c, c, os.path.join(MENU, name + ".png"))
    conf.append("corner corner.png corner-hover.png")
    # The tray. clock is where "8:11 PM" sits: the center, the baseline, the
    # size. The picture is one pixel wider than the tray (the divider's left
    # half), so the center is 105, not 104.
    tray_picture(os.path.join(MENU, "tray.png"))
    conf.append("tray tray.png")
    conf.append("clock %d %d %d" % (px(105), px(25), px(12)))
    open(os.path.join(MENU, "menu.conf"), "w").write("\n".join(conf) + "\n")


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    # Everything a drawing refers to, next to the drawings themselves.
    shutil.copy(os.path.join(THEME_DIR, "wallpaper.jpg"), WORK)
    shutil.copytree(os.path.join(THEME_DIR, "icons"), os.path.join(WORK, "icons"))
    own_icons()
    for d in (THEMES, LOGIN, WALL, MENUS, ICONS):
        shutil.rmtree(d, ignore_errors=True)    # no leftovers from an older palette
        os.makedirs(d, exist_ok=True)
    # My Files draws these itself, sharp at any scale.
    for f in os.listdir(os.path.join(WORK, "icons/svg")):
        shutil.copy(os.path.join(WORK, "icons/svg", f), ICONS)

    for pct in SCALES:
        set_scale(pct)
        title_bars()
        frames()
        title_buttons()
        menu_button()
        window_icons()
        taskbar()
        menus()
        theme_file()
        start_menu()
    set_scale(100)              # the login program scales its pictures itself
    login_background()
    login_pictures()
    shutil.copy(os.path.join(THEME_DIR, "wallpaper.jpg"), os.path.join(WALL, "wallpaper.jpg"))
    shutil.rmtree(WORK)

    n = sum(len(f) for _, _, f in os.walk(OUT))
    print("live theme: %d files in %s" % (n, os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
