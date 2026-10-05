#!/usr/bin/env python3
"""
make-theme.py — the "Golden Hour" desktop theme, drawn from one palette.

Writes, next to this file:
  icons/svg/NAME.svg          every icon, 48x48 design grid
  icons/png/SIZE/NAME.png     16, 24, 32, 48 and 96 px
  start-button{,-hover,-pressed}.svg / .png
                              the Start button (shape C, full taskbar height)
  mockup.svg / mockup.png     a 1024x768 desktop built from the pieces
  mockup@2x.png               the same at 2048x1536
  icon-sheet.svg / .png       every icon with its name
  ui-states.svg / .png        every element (taskbar, title bars, buttons...) in every state
  settings.svg / .png         Settings: Users (create account, admin password)
  settings-wifi.svg / .png    Settings: Wi-Fi (page exists; kernel does not yet)
  settings-general.svg / .png Settings: General (categories like the Mac list)
and rewrites the color tables in docs/ui-theme.md (between the colors:begin/end marks).

Needs rsvg-convert (Homebrew: librsvg). Change colors and sizes here, then run this again.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

# --- palette -----------------------------------------------------------------
C = {
    # taskbar: navy on the left to teal on the right
    "bar_left": "#0e2c52", "bar_right": "#1b5d74", "bar_edge": "#7fb6e6",
    # window title bars and the start menu's header and footer
    "title_top": "#2f9aa1", "title_mid": "#1f7f86", "title_bottom": "#0f5560",
    "title_edge": "#0a4048", "title_text": "#ffffff",
    # the Start button
    "green_1": "#7fe36a", "green_2": "#3fb447", "green_3": "#2c9431",
    "green_4": "#1c6b21", "green_rim": "#145a19",
    # round title-bar buttons: light teal (minimize, maximize), amber (close)
    "teal_btn_top": "#d6f6f7", "teal_btn_bottom": "#5cc3cb", "teal_btn_rim": "#2b8a92",
    "amber_top": "#ffc56b", "amber_mid": "#f08a12", "amber_bottom": "#c96500",
    "amber_rim": "#a24f00",
    # panels
    "panel_white": "#fbfbf9", "panel_sand": "#f9e6c1", "panel_sand_deep": "#efd3a0",
    "menu_bar": "#efece4", "border": "#c9c2b2",
    # text
    "text": "#1d2430", "text_soft": "#5b6573", "label_teal": "#1f6d72",
    # icons
    "folder_1": "#fff0a8", "folder_2": "#ffd84d", "folder_3": "#f0b22a",
    "folder_back_1": "#e9b53a", "folder_back_2": "#c98c17", "folder_rim": "#a87412",
    "globe_1": "#9fd8ff", "globe_2": "#1f6fd1", "globe_3": "#0f3f8a",
    "screen": "#0d1a12", "prompt": "#5cf27a",
    "slate_1": "#4a6c8f", "slate_2": "#1d3554", "slate_rim": "#0b1d33",
    "silver_1": "#f4f6f8", "silver_2": "#b8c2cc", "silver_rim": "#6c7a88",
    "paper_1": "#ffffff", "paper_2": "#e3e8ee", "paper_rim": "#7d8a96",
    "ink_blue": "#6a9fd4", "gold": "#e0a52c",
    "steel_1": "#e2edf6", "steel_2": "#7d9bb8", "steel_rim": "#4f6f8f",
}

FONT_UI = "Verdana, DejaVu Sans, sans-serif"
FONT_TITLE = "Trebuchet MS, Verdana, sans-serif"


# --- small SVG helpers ---------------------------------------------------------
def lg(gid, stops, x2=0, y2=1):
    s = "".join('<stop offset="%s" stop-color="%s"/>' % (o, c) for o, c in stops)
    return '<linearGradient id="%s" x1="0" y1="0" x2="%s" y2="%s">%s</linearGradient>' % (gid, x2, y2, s)


def rg(gid, stops, cx=0.35, cy=0.3, r=0.75):
    s = "".join('<stop offset="%s" stop-color="%s"/>' % (o, c) for o, c in stops)
    return '<radialGradient id="%s" cx="%s" cy="%s" r="%s">%s</radialGradient>' % (gid, cx, cy, r, s)


SHINE = lg("shine", [(0, "#ffffff"), (1, "#ffffff")]).replace(
    '<stop offset="1" stop-color="#ffffff"/>', '<stop offset="1" stop-color="#ffffff" stop-opacity="0"/>'
).replace('<stop offset="0" stop-color="#ffffff"/>', '<stop offset="0" stop-color="#ffffff" stop-opacity="0.75"/>')


def icon_svg(defs, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">'
            "<defs>%s%s</defs>%s</svg>\n" % (SHINE, "".join(defs), body))


def ball(fill_id, rim, r=20):
    """A glossy round button: the base of several icons."""
    return ('<circle cx="24" cy="24" r="%d" fill="url(#%s)" stroke="%s" stroke-width="1.6"/>'
            '<ellipse cx="24" cy="15" rx="%d" ry="%d" fill="url(#shine)" opacity="0.8"/>'
            % (r, fill_id, rim, r - 6, r // 2 - 1))


GREEN = lg("g", [(0, C["green_1"]), (0.5, C["green_2"]), (0.55, C["green_3"]), (1, C["green_4"])])
TEAL = lg("t", [(0, C["teal_btn_top"]), (1, C["teal_btn_bottom"])])
AMBER = lg("a", [(0, C["amber_top"]), (0.55, C["amber_mid"]), (1, C["amber_bottom"])])
STEEL = lg("s", [(0, C["steel_1"]), (1, C["steel_2"])])
GLOBE = rg("globe", [(0, C["globe_1"]), (0.6, C["globe_2"]), (1, C["globe_3"])])
FOLDER = [lg("fy", [(0, C["folder_1"]), (0.5, C["folder_2"]), (1, C["folder_3"])]),
          lg("fb", [(0, C["folder_back_1"]), (1, C["folder_back_2"])])]
PAPER = lg("paper", [(0, C["paper_1"]), (1, C["paper_2"])])
DARK = lg("dark", [(0, "#2a3b47"), (1, "#0b141a")])


def arrow(d):
    return ('<path d="%s" fill="none" stroke="#ffffff" stroke-width="4.5" '
            'stroke-linecap="round" stroke-linejoin="round"/>' % d)


FOLDER_BODY = (
    '<path d="M4 12a3 3 0 0 1 3-3h11l4 4h19a3 3 0 0 1 3 3v4H4z" fill="url(#fb)" '
    'stroke="%s" stroke-width="1.2"/>'
    '<path d="M3 18h42l-2 21a3 3 0 0 1-3 3H8a3 3 0 0 1-3-3z" fill="url(#fy)" '
    'stroke="%s" stroke-width="1.2"/>'
    '<path d="M5 19h38l-.6 7c-12 2-25 2-37 0z" fill="#ffffff" opacity="0.35"/>'
    % (C["folder_rim"], C["folder_3"]))

PAGE = ('<path d="M9 4h22l9 9v31a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z" '
        'fill="url(#paper)" stroke="%s" stroke-width="1.3"/>'
        '<path d="M31 4v7a2 2 0 0 0 2 2h7" fill="#d5dde5" stroke="%s" stroke-width="1.3"/>'
        % (C["paper_rim"], C["paper_rim"]))


def globe(cx, cy, r):
    body = '<circle cx="%s" cy="%s" r="%s" fill="url(#globe)" stroke="%s" stroke-width="1.3"/>' % (
        cx, cy, r, C["globe_3"])
    lines = ('<g fill="none" stroke="#ffffff" stroke-opacity="0.7" stroke-width="1.3">'
             '<ellipse cx="%s" cy="%s" rx="%s" ry="%s"/>' % (cx, cy, r * 0.42, r) +
             '<path d="M%s %s h%s"/>' % (cx - r, cy, 2 * r) +
             '<path d="M%s %s q%s 4 %s 0"/>' % (cx - r + 3, cy - r / 2, r - 3, 2 * r - 6) +
             '<path d="M%s %s q%s -4 %s 0"/></g>' % (cx - r + 3, cy + r / 2, r - 3, 2 * r - 6))
    shine = '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="url(#shine)" opacity="0.55"/>' % (
        cx - r * 0.2, cy - r * 0.45, r * 0.6, r * 0.4)
    return body + lines + shine


# --- the icons -------------------------------------------------------------------
ICONS = {}

ICONS["logo"] = icon_svg([], (
    '<circle cx="24" cy="24" r="14" fill="none" stroke="#ffffff" stroke-width="3.5"/>'
    '<circle cx="24" cy="24" r="5.2" fill="#ffffff"/>'))

ICONS["terminal"] = icon_svg([DARK], (
    '<rect x="3" y="6" width="42" height="32" rx="5" fill="url(#dark)" stroke="#05090c" stroke-width="1.5"/>'
    '<rect x="6.5" y="9.5" width="35" height="25" rx="3" fill="%s"/>' % C["screen"] +
    '<path d="M11 16l6 5-6 5" fill="none" stroke="%s" stroke-width="2.8" '
    'stroke-linecap="round" stroke-linejoin="round"/>' % C["prompt"] +
    '<path d="M20 27h9" stroke="%s" stroke-width="2.8" stroke-linecap="round"/>' % C["prompt"] +
    '<path d="M6.5 9.5h35v8c-12 3-23 3-35 0z" fill="#ffffff" opacity="0.10"/>'
    '<rect x="19" y="38" width="10" height="3" fill="#33454f"/>'
    '<rect x="13" y="41" width="22" height="3.5" rx="1.75" fill="#22323b"/>'))

ICONS["run"] = icon_svg([DARK], (
    '<rect x="3" y="6" width="42" height="36" rx="5" fill="url(#dark)" stroke="#05090c" stroke-width="1.5"/>'
    '<rect x="6.5" y="9.5" width="35" height="29" rx="3" fill="%s"/>' % C["screen"] +
    '<path d="M19 16l12 8-12 8z" fill="%s" stroke="%s" stroke-width="1.5" stroke-linejoin="round"/>'
    % (C["prompt"], C["prompt"]) +
    '<path d="M6.5 9.5h35v9c-12 3-23 3-35 0z" fill="#ffffff" opacity="0.10"/>'))

ICONS["folder"] = icon_svg(FOLDER, FOLDER_BODY)

ICONS["my-files"] = icon_svg(FOLDER + [lg("leaf", [(0, "#a6f08a"), (1, C["green_3"])])], FOLDER_BODY + (
    '<path d="M25 41c0-10 7-15 18-15 0 10-6 16-18 15z" fill="url(#leaf)" stroke="%s" stroke-width="1.3"/>'
    % C["green_4"] +
    '<path d="M27.5 38.5L39 29.5" stroke="#d9f8c8" stroke-width="1.4" stroke-linecap="round"/>'))

ICONS["text-file"] = icon_svg([PAPER], PAGE + "".join(
    '<path d="M12 %d h%d" stroke="%s" stroke-width="2" stroke-linecap="round"/>' % (y, w, C["ink_blue"])
    for y, w in [(19, 22), (24, 22), (29, 22), (34, 22), (39, 14)]))

ICONS["binary-file"] = icon_svg([PAPER], PAGE + "".join(
    '<rect x="%d" y="%d" width="4" height="4" fill="%s"/>' % (12 + 5 * (k % 5), 18 + 5 * (k // 5),
                                                              "#4a5a6a" if (k * 7) % 3 else "#aab6c2")
    for k in range(20)))

ICONS["text-editor"] = icon_svg([PAPER, lg("pen", [(0, C["green_1"]), (1, C["green_3"])], x2=1, y2=0)],
    PAGE + "".join('<path d="M12 %d h%d" stroke="%s" stroke-width="2" stroke-linecap="round"/>'
                   % (y, w, C["ink_blue"]) for y, w in [(19, 20), (24, 16), (29, 12)]) + (
    '<g transform="rotate(45 33 26)">'
    '<rect x="30" y="8" width="7" height="24" rx="1" fill="url(#pen)" stroke="%s" stroke-width="1"/>'
    % C["green_4"] +
    '<rect x="30" y="8" width="7" height="4" fill="%s"/>' % C["folder_3"] +
    '<path d="M30 32h7l-3.5 7z" fill="#f3d9a4" stroke="#8a6a3a" stroke-width="0.8"/>'
    '<path d="M32.4 36.8h2.2l-1.1 2.2z" fill="#333"/></g>'))

ICONS["web-browser"] = icon_svg([GLOBE, AMBER], globe(21, 21, 17) + (
    '<circle cx="35" cy="35" r="10" fill="url(#a)" stroke="%s" stroke-width="1.4"/>' % C["amber_rim"] +
    '<path d="M35 27.5l2.6 7.5h-5.2z" fill="#ffffff"/>'
    '<path d="M35 42.5l2.6-7.5h-5.2z" fill="#7a3a00"/>'
    '<circle cx="35" cy="35" r="1.3" fill="#ffffff"/>'))

ICONS["network"] = icon_svg([GLOBE], globe(24, 24, 20) + (
    '<g stroke="#ffffff" stroke-width="1.7">'
    '<path d="M12 17L24 24L31 12M24 24L34 31L19 35L12 17M24 24L19 35" fill="none"/></g>'
    '<g fill="#ffffff">' + "".join('<circle cx="%d" cy="%d" r="2.8"/>' % p
                                   for p in [(12, 17), (31, 12), (34, 31), (19, 35), (24, 24)]) + "</g>"))

ICONS["disks"] = icon_svg([lg("slate", [(0, C["slate_1"]), (1, C["slate_2"])])], (
    '<rect x="4" y="13" width="40" height="23" rx="6" fill="url(#slate)" stroke="%s" stroke-width="1.5"/>'
    % C["slate_rim"] +
    '<rect x="6.5" y="15" width="35" height="7" rx="3.5" fill="#ffffff" opacity="0.25"/>' +
    "".join('<circle cx="%d" cy="29" r="2.1" fill="%s"/>' % (x, C["prompt"]) for x in (12, 18, 24)) +
    '<path d="M30 29h9" stroke="%s" stroke-width="2.2" stroke-linecap="round"/>' % C["slate_rim"]))

ICONS["settings"] = icon_svg([lg("silver", [(0, C["silver_1"]), (1, C["silver_2"])]), TEAL], (
    '<rect x="5" y="5" width="38" height="38" rx="8" fill="url(#silver)" stroke="%s" stroke-width="1.5"/>'
    % C["silver_rim"] +
    "".join('<path d="M%d 12v24" stroke="#5d6b78" stroke-width="2.6" stroke-linecap="round"/>' % x
            for x in (15, 24, 33)) +
    "".join('<rect x="%d" y="%d" width="10" height="7" rx="2" fill="url(#t)" stroke="%s" stroke-width="1.2"/>'
            % (x - 5, y, C["teal_btn_rim"]) for x, y in [(15, 25), (24, 13), (33, 29)])))

ICONS["help"] = icon_svg([lg("help", [(0, "#ffe27a"), (1, "#f2a51a")])], (
    '<path d="M14 5h20a10 10 0 0 1 10 10v10a10 10 0 0 1-10 10H22l-11 8 3-8a10 10 0 0 1-10-10V15'
    'A10 10 0 0 1 14 5z" fill="url(#help)" stroke="#b36f00" stroke-width="1.5"/>'
    '<ellipse cx="22" cy="11" rx="14" ry="4.5" fill="url(#shine)" opacity="0.7"/>'
    '<text x="24" y="30.5" text-anchor="middle" font-family="%s" font-size="24" font-weight="bold" '
    'fill="#ffffff" stroke="#b36f00" stroke-width="1.2" paint-order="stroke">?</text>' % FONT_TITLE))

ICONS["all-programs"] = icon_svg([GREEN], ball("g", C["green_rim"]) + arrow("M14 24h18M25 16l8 8-8 8"))
ICONS["back"] = icon_svg([GREEN], ball("g", C["green_rim"]) + arrow("M34 24H16M23 16l-8 8 8 8"))
ICONS["forward"] = icon_svg([STEEL], ball("s", C["steel_rim"]) + arrow("M14 24h18M25 16l8 8-8 8"))
ICONS["up"] = icon_svg([STEEL], ball("s", C["steel_rim"]) + arrow("M24 34V16M16 23l8-8 8 8"))

ICONS["log-off"] = icon_svg([TEAL], ball("t", C["teal_btn_rim"]) + (
    '<g fill="none" stroke="%s" stroke-width="3.6" stroke-linecap="round">' % C["gold"] +
    '<circle cx="18" cy="19" r="6"/><path d="M22.5 23.5L34 35M29 30l3.5-3.5M32.5 33.5L36 30"/></g>'))

ICONS["turn-off"] = icon_svg([AMBER], ball("a", C["amber_rim"]) + (
    '<g fill="none" stroke="#ffffff" stroke-width="3.8" stroke-linecap="round">'
    '<path d="M16.5 16.5A11 11 0 1 0 31.5 16.5"/><path d="M24 11V24"/></g>'))

ICONS["recycle"] = icon_svg([lg("bin", [(0, "#ffffff"), (1, "#c3ced8")])], (
    '<path d="M11 14h26l-2.6 28a3 3 0 0 1-3 2.7H16.6a3 3 0 0 1-3-2.7z" fill="url(#bin)" '
    'stroke="%s" stroke-width="1.4"/>' % C["paper_rim"] +
    '<rect x="8" y="9" width="32" height="5.5" rx="2.5" fill="#e4eaef" stroke="%s" stroke-width="1.4"/>'
    % C["paper_rim"] +
    '<path d="M20 9V6.5h8V9" fill="none" stroke="%s" stroke-width="1.4"/>' % C["paper_rim"] +
    '<g fill="none" stroke="%s" stroke-width="2.8" stroke-linecap="round">' % C["green_3"] +
    '<path d="M30.5 31a7 7 0 1 1-2.3-7.4"/></g>'
    '<path d="M26 20.5l5.5 2.2-3 4.6z" fill="%s"/>' % C["green_3"]))

ICONS["tray-volume"] = icon_svg([], (
    '<path d="M7 18h7l9-8v28l-9-8H7z" fill="#ffffff"/>'
    '<g fill="none" stroke="#ffffff" stroke-width="3" stroke-linecap="round">'
    '<path d="M29 17a9 9 0 0 1 0 14"/><path d="M34 12a16 16 0 0 1 0 24"/></g>'))

ICONS["tray-network"] = icon_svg([], (
    '<g fill="none" stroke="#ffffff" stroke-width="3">'
    '<rect x="4" y="8" width="18" height="14" rx="2"/><rect x="26" y="20" width="18" height="14" rx="2"/>'
    '<path d="M13 22v6h22v-8" stroke-linejoin="round"/><path d="M9 40h30" stroke-linecap="round"/>'
    '<path d="M24 28v12"/></g>'))

# Settings categories (sidebar icons). Drawn on the same 48x48 grid.
WIFI_ARCS = (
    '<g fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round">'
    '<path d="M8 26a22 22 0 0 1 32 0"/><path d="M13 31a14 14 0 0 1 22 0"/>'
    '<path d="M18 36a8 8 0 0 1 12 0"/></g>'
    '<circle cx="24" cy="42" r="3" fill="%s"/>')
ICONS["wifi"] = icon_svg([GLOBE], WIFI_ARCS % (C["globe_2"], C["globe_2"]))
ICONS["bluetooth"] = icon_svg([STEEL], ball("s", C["steel_rim"]) + (
    '<path d="M24 10l10 8-10 6 10 6-10 8V10m0 14l-8-6m8 6l-8 6" fill="none" stroke="#ffffff" '
    'stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>'))
ICONS["battery"] = icon_svg([lg("bat", [(0, C["green_1"]), (1, C["green_3"])])], (
    '<rect x="4" y="14" width="36" height="20" rx="4" fill="%s" stroke="%s" stroke-width="1.5"/>'
    % (C["silver_1"], C["silver_rim"]) +
    '<rect x="7" y="17" width="24" height="14" rx="2" fill="url(#bat)"/>'
    '<rect x="40" y="20" width="5" height="8" rx="1.5" fill="%s"/>' % C["silver_rim"]))
ICONS["vpn"] = icon_svg([GLOBE, AMBER], globe(24, 22, 16) + (
    '<rect x="14" y="26" width="20" height="16" rx="3" fill="url(#a)" stroke="%s" stroke-width="1.3"/>'
    % C["amber_rim"] +
    '<path d="M19 26v-4a5 5 0 0 1 10 0v4" fill="none" stroke="%s" stroke-width="2.4"/>' % C["amber_rim"] +
    '<circle cx="24" cy="34" r="2.2" fill="#ffffff"/>'))
ICONS["users"] = icon_svg([TEAL, lg("slate", [(0, C["slate_1"]), (1, C["slate_2"])])], (
    '<circle cx="16" cy="16" r="8" fill="url(#t)" stroke="%s" stroke-width="1.3"/>' % C["teal_btn_rim"] +
    '<path d="M4 40c0-8 5-13 12-13s12 5 12 13" fill="url(#t)" stroke="%s" stroke-width="1.3"/>'
    % C["teal_btn_rim"] +
    '<circle cx="32" cy="18" r="7" fill="url(#slate)" stroke="%s" stroke-width="1.3"/>' % C["slate_rim"] +
    '<path d="M22 41c1-7 5-11 11-11s11 4 12 11" fill="url(#slate)" stroke="%s" stroke-width="1.3"/>'
    % C["slate_rim"]))
ICONS["lock"] = icon_svg([AMBER], (
    '<rect x="10" y="20" width="28" height="22" rx="5" fill="url(#a)" stroke="%s" stroke-width="1.5"/>'
    % C["amber_rim"] +
    '<path d="M16 20v-5a8 8 0 0 1 16 0v5" fill="none" stroke="%s" stroke-width="3.2"/>' % C["amber_rim"] +
    '<circle cx="24" cy="31" r="3" fill="#ffffff"/>'
    '<path d="M24 34v5" stroke="#ffffff" stroke-width="2.4" stroke-linecap="round"/>'))
ICONS["display"] = icon_svg([DARK, GLOBE], (
    '<rect x="3" y="6" width="42" height="28" rx="4" fill="url(#dark)" stroke="#05090c" stroke-width="1.5"/>'
    '<rect x="6" y="9" width="36" height="22" rx="2" fill="url(#globe)"/>'
    '<rect x="19" y="34" width="10" height="4" fill="#33454f"/>'
    '<rect x="12" y="38" width="24" height="4" rx="2" fill="#22323b"/>'))
ICONS["wallpaper"] = icon_svg([lg("sky", [(0, "#9fd8ff"), (1, "#f7d08a")]),
                               lg("hill", [(0, C["green_2"]), (1, C["green_4"])])], (
    '<rect x="4" y="8" width="40" height="32" rx="5" fill="url(#sky)" stroke="%s" stroke-width="1.4"/>'
    % C["slate_rim"] +
    '<circle cx="34" cy="16" r="5" fill="#ffe27a"/>'
    '<path d="M4 30c8-8 14-4 20 0s12-6 20-2v12H4z" fill="url(#hill)"/>'))
ICONS["keyboard"] = icon_svg([lg("silver", [(0, C["silver_1"]), (1, C["silver_2"])])], (
    '<rect x="3" y="12" width="42" height="24" rx="5" fill="url(#silver)" stroke="%s" stroke-width="1.5"/>'
    % C["silver_rim"] +
    "".join('<rect x="%d" y="%d" width="5" height="5" rx="1" fill="#ffffff" stroke="%s"/>'
            % (7 + 7 * (k % 5), 16 + 7 * (k // 5), C["silver_rim"]) for k in range(10)) +
    '<rect x="10" y="30" width="28" height="4" rx="1" fill="#ffffff" stroke="%s"/>' % C["silver_rim"]))
ICONS["mouse"] = icon_svg([lg("silver", [(0, C["silver_1"]), (1, C["silver_2"])])], (
    '<rect x="12" y="6" width="24" height="36" rx="12" fill="url(#silver)" stroke="%s" stroke-width="1.5"/>'
    % C["silver_rim"] +
    '<path d="M24 6v14" stroke="%s" stroke-width="1.5"/><path d="M12 20h24" stroke="%s" stroke-width="1.5"/>'
    % (C["silver_rim"], C["silver_rim"]) +
    '<rect x="21" y="10" width="6" height="8" rx="3" fill="%s"/>' % C["teal_btn_bottom"]))
ICONS["printer"] = icon_svg([lg("silver", [(0, C["silver_1"]), (1, C["silver_2"])]), PAPER], (
    '<rect x="10" y="4" width="28" height="12" rx="2" fill="url(#paper)" stroke="%s" stroke-width="1.2"/>'
    % C["paper_rim"] +
    '<rect x="5" y="14" width="38" height="20" rx="4" fill="url(#silver)" stroke="%s" stroke-width="1.5"/>'
    % C["silver_rim"] +
    '<rect x="12" y="28" width="24" height="14" rx="2" fill="url(#paper)" stroke="%s" stroke-width="1.2"/>'
    % C["paper_rim"] +
    '<circle cx="34" cy="22" r="2" fill="%s"/>' % C["green_3"]))
ICONS["privacy"] = icon_svg([TEAL], (
    '<path d="M24 4l16 7v12c0 12-8 18-16 21C16 41 8 35 8 23V11z" fill="url(#t)" stroke="%s" stroke-width="1.5"/>'
    % C["teal_btn_rim"] +
    '<path d="M16 24l5 5 11-12" fill="none" stroke="#ffffff" stroke-width="3.2" '
    'stroke-linecap="round" stroke-linejoin="round"/>'))
ICONS["accessibility"] = icon_svg([TEAL], (
    '<circle cx="24" cy="24" r="20" fill="url(#t)" stroke="%s" stroke-width="1.6"/>' % C["teal_btn_rim"] +
    '<circle cx="24" cy="13" r="4" fill="#ffffff"/>'
    '<path d="M14 20h20M18 20l-2 18m16-18l2 18M16 30h16" fill="none" stroke="#ffffff" '
    'stroke-width="3" stroke-linecap="round"/>'))
ICONS["appearance"] = icon_svg([TEAL, AMBER, GREEN], (
    '<circle cx="18" cy="18" r="11" fill="url(#t)" stroke="%s" stroke-width="1.3"/>' % C["teal_btn_rim"] +
    '<circle cx="32" cy="18" r="11" fill="url(#a)" stroke="%s" stroke-width="1.3"/>' % C["amber_rim"] +
    '<circle cx="24" cy="31" r="11" fill="url(#g)" stroke="%s" stroke-width="1.3"/>' % C["green_rim"]))
ICONS["updates"] = icon_svg([GREEN], ball("g", C["green_rim"]) + (
    '<path d="M16 20a9 9 0 0 1 16 3M32 28a9 9 0 0 1-16-3" fill="none" stroke="#ffffff" '
    'stroke-width="3" stroke-linecap="round"/>'
    '<path d="M30 14l4 9h-8zM18 34l-4-9h8z" fill="#ffffff"/>'))
ICONS["about"] = icon_svg([TEAL], ball("t", C["teal_btn_rim"]) + (
    '<circle cx="24" cy="16" r="3" fill="#ffffff"/>'
    '<path d="M24 23v12" stroke="#ffffff" stroke-width="4" stroke-linecap="round"/>'))
ICONS["notifications"] = icon_svg([AMBER], (
    '<path d="M12 20a12 12 0 0 1 24 0c0 10 3 12 3 12H9s3-2 3-12z" fill="url(#a)" stroke="%s" stroke-width="1.5"/>'
    % C["amber_rim"] +
    '<path d="M20 36a4 4 0 0 0 8 0" fill="none" stroke="%s" stroke-width="2.4"/>' % C["amber_rim"]))
ICONS["password"] = icon_svg([AMBER], (
    '<circle cx="16" cy="24" r="10" fill="url(#a)" stroke="%s" stroke-width="1.6"/>' % C["amber_rim"] +
    '<circle cx="16" cy="24" r="3.5" fill="#ffffff"/>'
    '<path d="M24 24h16l-4 4 4 4" fill="none" stroke="%s" stroke-width="3.2" '
    'stroke-linecap="round" stroke-linejoin="round"/>' % C["amber_rim"]))
ICONS["desktop"] = icon_svg([TEAL, lg("barh", [(0, C["bar_left"]), (1, C["bar_right"])], x2=1, y2=0)], (
    '<rect x="4" y="6" width="40" height="30" rx="4" fill="url(#t)" stroke="%s" stroke-width="1.4"/>'
    % C["teal_btn_rim"] +
    '<rect x="7" y="12" width="20" height="14" rx="2" fill="%s"/>' % C["panel_white"] +
    '<rect x="24" y="16" width="16" height="12" rx="2" fill="%s"/>' % C["panel_sand"] +
    '<rect x="4" y="36" width="40" height="6" rx="2" fill="url(#barh)"/>'
    '<path d="M4 36h10c4 0 6 3 10 3" fill="%s"/>' % C["green_3"]))
ICONS["sound"] = icon_svg([STEEL], (
    '<path d="M7 18h7l9-8v28l-9-8H7z" fill="url(#s)" stroke="%s" stroke-width="1.3"/>' % C["steel_rim"] +
    '<g fill="none" stroke="%s" stroke-width="3" stroke-linecap="round">'
    '<path d="M29 17a9 9 0 0 1 0 14"/><path d="M34 12a16 16 0 0 1 0 24"/></g>' % C["steel_rim"]))
ICONS["datetime"] = icon_svg([STEEL], ball("s", C["steel_rim"]) + (
    '<path d="M24 14v11l7 5" fill="none" stroke="#ffffff" stroke-width="3.2" stroke-linecap="round"/>'))

# --- every element and its states ----------------------------------------------------
# One table drives the mockup, the states sheet (ui-states.png) and the color tables
# in docs/ui-theme.md. A fill is either one hex color (flat) or a gradient G().
def G(direction, *stops):
    """A gradient: direction "v" (top to bottom) or "h" (left to right); stops are
    (offset, color) or (offset, color, opacity)."""
    return {"dir": direction, "stops": stops}


TASKBAR_SHADE = G("v", (0, "#ffffff", 0.30), (0.15, "#ffffff", 0.06), (1, "#000000", 0.18))

UI = [
    ("Taskbar", "the bar along the bottom of the screen, 40 px high", "", [
        ("normal", dict(kind="bar", w=200, rx=0, fill=G("h", (0, C["bar_left"]), (1, C["bar_right"])),
                        over=TASKBAR_SHADE, top=C["bar_edge"],
                        note="the shade is drawn over the base; a 1.5 px line along the top in `bar_edge` at 80%")),
    ]),
    ("Tray", "the clock and status icons at the right end of the taskbar", "8:11 PM", [
        ("normal", dict(kind="bar", rx=0, fill=G("v", (0, "#2d7c8f"), (1, "#16546a")), over=TASKBAR_SHADE,
                        top=C["bar_edge"], left="#0b2346", text="#ffffff",
                        note="the same shade and top line as the taskbar; a 1.5 px `#0b2346` divider on its left")),
    ]),
    ("Task button", "one per open window, on the taskbar; 160x30 px, 4 px corners", "My Files", [
        ("normal", dict(fill=G("v", (0, "#3a6fa8"), (1, "#1d4a7c")), border="#0b2346", shine=0.25, text="#ffffff")),
        ("hover", dict(fill=G("v", (0, "#4d86c2"), (1, "#275a90")), border="#0b2346", shine=0.35, text="#ffffff")),
        ("active", dict(fill=G("v", (0, "#132e55"), (1, "#1f4f80")), border="#06162e", inset=True, bold=True,
                        text="#ffffff", note="its window has the focus: looks pressed in, a 3 px black inner "
                                             "shadow at 30% along the top; bold text")),
        ("attention", dict(fill=G("v", (0, C["amber_top"]), (0.55, C["amber_mid"]), (1, C["amber_bottom"])),
                           border=C["amber_rim"], shine=0.35, text="#ffffff",
                           note="the window wants you: flashes 3 times (0.5 s on, 0.5 s off), then stays amber")),
    ]),
    ("Start button", "shape C, the full taskbar height (see Layout)", "Start", [
        ("normal", dict(kind="start", fill=G("v", (0, C["green_1"]), (0.5, C["green_2"]), (0.55, C["green_3"]),
                                             (1, C["green_4"])), border=C["green_rim"], shine=0.7, text="#ffffff",
                        note="the hard step at 50-55% is the gloss crease; text shadow `#0b3d0f` at 75%")),
        ("hover", dict(kind="start", fill=G("v", (0, "#9df08a"), (0.5, "#52c95a"), (0.55, "#3aa940"), (1, "#22802a")),
                       border=C["green_rim"], shine=0.8, text="#ffffff")),
        ("pressed", dict(kind="start", fill=G("v", (0, "#5cc954"), (0.5, "#2c9431"), (0.55, "#22802a"),
                                              (1, "#145a19")), border="#0e4512", shine=0.35, text="#ffffff",
                         note="also while the start menu is open; the logo and text move 1 px down")),
    ]),
    ("Window title bar", "30 px; also the start menu's header and footer", "My Files", [
        ("active", dict(kind="title", fill=G("v", (0, C["title_top"]), (0.5, C["title_mid"]), (1, C["title_bottom"])),
                        border=C["title_edge"], frame=C["title_edge"], shine=0.35, text=C["title_text"],
                        buttons="normal", shadow=(3, 5, 0.35),
                        note="title text has a 1 px black shadow at 80%; window shadow 0/3 px, blur 5, black 35%")),
        ("inactive", dict(kind="title", fill=G("v", (0, "#a9cacd"), (0.5, "#8fb5b9"), (1, "#769ea3")),
                          border="#628a8f", frame="#8aa9ac", shine=0.25, text="#ffffff", buttons="inactive",
                          shadow=(2, 3, 0.20),
                          note="no text shadow; the icon at 75%; window shadow 0/2 px, blur 3, black 20%")),
    ]),
    ("Minimize and maximize buttons", "round, 21 px, 3 px apart, in the title bar", "", [
        ("normal", dict(kind="ball", glyph="maximize", fill=G("v", (0, C["teal_btn_top"]), (1, C["teal_btn_bottom"])),
                        border=C["teal_btn_rim"], text="#ffffff")),
        ("hover", dict(kind="ball", glyph="maximize", fill=G("v", (0, "#effdfd"), (1, "#7fd8de")),
                       border=C["teal_btn_rim"], text="#ffffff")),
        ("pressed", dict(kind="ball", glyph="maximize", fill=G("v", (0, "#5cc3cb"), (1, "#3c9fa7")),
                         border="#1f6d72", text="#e8fbfb")),
        ("inactive", dict(kind="ball", glyph="maximize", fill=G("v", (0, "#e6eff0"), (1, "#a9c6c9")),
                          border="#8aa9ac", text="#f7fafa", note="the window doesn't have the focus")),
    ]),
    ("Close button", "round, 21 px, the last button in the title bar", "", [
        ("normal", dict(kind="ball", glyph="close", fill=G("v", (0, C["amber_top"]), (0.55, C["amber_mid"]),
                                                           (1, C["amber_bottom"])), border=C["amber_rim"],
                        text="#ffffff")),
        ("hover", dict(kind="ball", glyph="close", fill=G("v", (0, "#ffd994"), (0.55, "#ff9e30"), (1, "#db7600")),
                       border=C["amber_rim"], text="#ffffff")),
        ("pressed", dict(kind="ball", glyph="close", fill=G("v", (0, "#f08a12"), (0.55, "#c96500"), (1, "#934800")),
                         border="#7a3a00", text="#ffe9cc")),
        ("inactive", dict(kind="ball", glyph="close", fill=G("v", (0, "#f6e0c6"), (0.55, "#e6b98a"), (1, "#cf9c6a")),
                          border="#b8916a", text="#fffaf3")),
    ]),
    ("Menu highlight", "the row under the mouse in the start menu, menus and lists", "Settings", [
        ("hover", dict(fill=G("v", (0, "#3aa7ae"), (1, "#1f7f86")), border="#17666c", rx=3, text="#ffffff",
                       note="the same on the white and the sand column; its icon stays as it is")),
    ]),
    ("Menu bar item", "File, Edit, View... at the top of a window", "File", [
        ("normal", dict(fill=C["menu_bar"], rx=3, text=C["text"])),
        ("hover", dict(fill="#d7ecee", border="#8fc7cb", rx=3, text=C["text"])),
        ("open", dict(fill=G("v", (0, "#3aa7ae"), (1, "#1f7f86")), border="#17666c", rx=3, text="#ffffff",
                      note="while its menu is open; the menu is drawn like the start menu's white column")),
    ]),
    ("Selected file", "a selected item in a file window or on the desktop", "hello.txt", [
        ("focused", dict(fill="#cdeef0", border="#5cc3cb", rx=3, text=C["text"],
                         note="the name under the icon gets a `#1f7f86` background and white text; "
                              "on the desktop the name background is `#1f7f86` at 85%")),
        ("unfocused", dict(fill="#e4e6e2", border=C["border"], rx=3, text=C["text"],
                           note="selection in a window that doesn't have the focus")),
    ]),
    ("Push button", "OK, Cancel and the like in dialogs; 75x23 px, 4 px corners", "OK", [
        ("normal", dict(fill=G("v", (0, "#ffffff"), (1, "#e6e2d8")), border="#a59d8c", rx=4, text=C["text"])),
        ("hover", dict(fill=G("v", (0, "#ffffff"), (1, "#e6e2d8")), border=C["amber_mid"], bw=2, rx=4,
                       text=C["text"], note="an amber 2 px border")),
        ("pressed", dict(fill=G("v", (0, "#e2ddd2"), (1, "#f6f4ef")), border="#a59d8c", rx=4, text=C["text"])),
        ("default", dict(fill=G("v", (0, "#ffffff"), (1, "#e6e2d8")), border=C["teal_btn_rim"], bw=2, rx=4,
                         text=C["text"], note="the button Enter presses")),
        ("disabled", dict(fill=C["menu_bar"], border="#d4cfc4", rx=4, text="#9aa3ad")),
    ]),
    ("Text box", "address bar, file names, dialogs; 22 px high", "/data", [
        ("normal", dict(fill="#ffffff", border=C["border"], rx=3, text=C["text"])),
        ("focused", dict(fill="#ffffff", border=C["teal_btn_rim"], bw=1.5, ring="#5cc3cb", rx=3, text=C["text"],
                         note="plus a 2 px `#5cc3cb` ring at 40% just outside; caret `#1d2430`")),
        ("disabled", dict(fill=C["menu_bar"], border="#d4cfc4", rx=3, text="#9aa3ad")),
    ]),
    ("Scroll bar", "17 px wide; track `menu_bar` with a `border` line on its inner side", "", [
        ("thumb", dict(w=60, fill=G("h", (0, C["teal_btn_top"]), (1, C["teal_btn_bottom"])),
                       border=C["teal_btn_rim"], rx=4, note="the gradient runs across the thumb")),
        ("thumb hover", dict(w=60, fill=G("h", (0, "#effdfd"), (1, "#7fd8de")), border=C["teal_btn_rim"], rx=4)),
    ]),
    ("Tooltip", "the little note that appears under the mouse", "Turn off the computer", [
        ("normal", dict(w=170, fill="#fff7dd", border="#c9a24a", rx=3, text=C["text"],
                        note="11 px Verdana; shadow 0/2 px, blur 2, black 25%")),
    ]),
    ("Hot corner", "the top-right corner of the desktop: the wallpaper curls over; move the mouse "
                   "into the corner to see all windows (like Mission Control on the Mac)", "", [
        ("normal", dict(kind="curl", size=24, fill=G("v", (0, C["title_bottom"]), (1, C["bar_left"])),
                        flap=G("v", (0, "#fffaf0"), (1, "#d8c7a4")), border="#b9a37a",
                        note="fill is what shows under the curl; the flap is the back of the wallpaper, "
                             "lit along the fold, `#b9a37a` edge, shadow -1/1.5 px, blur 1.5, black 35%")),
        ("hover", dict(kind="curl", size=40, fill=G("v", (0, C["title_mid"]), (1, C["bar_left"])),
                       flap=G("v", (0, "#ffffff"), (1, "#e3d3b0")), border="#b9a37a",
                       note="the mouse is in the corner: the curl grows to 40 px in 120 ms, then the "
                            "windows spread out")),
    ]),
    ("Login background", "the middle of the login screen", "", [
        ("normal", dict(fill=G("v", (0, "#0e2c52", 0.55), (0.5, "#0f5560", 0.35), (1, "#0e2c52", 0.70)),
                        on_dark=True, note="drawn over the wallpaper, which is blurred (Gaussian, 6 px)")),
    ]),
    ("Login bands", "the bands at the top (64 px) and bottom (80 px) of the login screen", "", [
        ("normal", dict(rx=0, fill=G("v", (0, "#0e2c52"), (1, "#133f63")), glow=True,
                        note="the bottom band runs the other way; a 2 px golden line where each band meets "
                             "the middle, left to right: clear → `#ffc56b` → clear")),
    ]),
    ("User tile", "one per user on the login screen; 380x84 px, 10 px corners", "rodger", [
        ("normal", dict(fill="none", rx=10, text="#ffffff", on_dark=True, note="the picture at 85%")),
        ("hover", dict(fill=G("h", (0, "#ffffff", 0.16), (1, "#ffffff", 0.03)), border=C["bar_edge"], rx=10,
                       text="#ffffff", on_dark=True)),
        ("selected", dict(fill=G("h", (0, "#ffffff", 0.28), (1, "#ffffff", 0.06)), border=C["amber_top"], bw=1.5,
                          rx=10, text="#ffffff", bold=True, on_dark=True,
                          note="the golden border is the theme's name; shows the password box (a focused "
                               "text box) and the green go button")),
    ]),
]


def S(element, state):
    for name, _where, _label, states in UI:
        if name == element:
            return dict(states)[state]
    raise KeyError(element)


_ids = [0]


def grad(g):
    """Return (id, <defs> markup) for a fill; a flat color gives (color, "")."""
    if isinstance(g, str):
        return g, ""
    _ids[0] += 1
    gid = "u%d" % _ids[0]
    x2, y2 = (1, 0) if g["dir"] == "h" else (0, 1)
    stops = "".join('<stop offset="%s" stop-color="%s"%s/>' % (st[0], st[1], ' stop-opacity="%s"' % st[2]
                                                              if len(st) > 2 else "") for st in g["stops"])
    return "url(#%s)" % gid, '<defs><linearGradient id="%s" x1="0" y1="0" x2="%s" y2="%s">%s</linearGradient></defs>' % (
        gid, x2, y2, stops)


def draw_bar(st, x, y, w, h, label=None, size=12):
    fill, d = grad(st["fill"])
    o = [d]
    if st.get("ring"):
        o.append('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-width="2" '
                 'stroke-opacity="0.4"/>' % (x - 2, y - 2, w + 4, h + 4, st.get("rx", 4) + 2, st["ring"]))
    o.append('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" stroke="%s" stroke-width="%s"/>'
             % (x, y, w, h, st.get("rx", 4), fill, st.get("border", "none"), st.get("bw", 1)))
    if st.get("over"):
        over, d = grad(st["over"])
        o.append(d + '<rect x="%s" y="%s" width="%s" height="%s" fill="%s"/>' % (x, y, w, h, over))
    if st.get("shine"):
        o.append('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="url(#shine)" opacity="%s"/>'
                 % (x + 2, y + 1, w - 4, h * 0.42, st.get("rx", 4), st["shine"]))
    if st.get("inset"):
        o.append('<rect x="%s" y="%s" width="%s" height="3" rx="2" fill="#000000" opacity="0.3"/>' % (x + 1, y + 1, w - 2))
    if st.get("glow"):
        o.append('<rect x="%s" y="%s" width="%s" height="2" fill="url(#goldline)"/>' % (x, y + h - 2, w))
    if st.get("top"):
        o.append('<rect x="%s" y="%s" width="%s" height="1.5" fill="%s" opacity="0.8"/>' % (x, y, w, st["top"]))
    if st.get("left"):
        o.append('<path d="M%s %sv%s" stroke="%s" stroke-width="1.5"/>' % (x, y + 1.5, h - 1.5, st["left"]))
    if label:
        o.append(text(x + w / 2, y + h / 2 + size * 0.36, label, size, st.get("text"),
                      "bold" if st.get("bold") else "normal", anchor="middle"))
    return "".join(o)


GLYPH = {
    "minimize": '<path d="M15 31h18" stroke="%s" stroke-width="5" stroke-linecap="round"/>',
    "maximize": '<rect x="15" y="15" width="18" height="18" rx="2" fill="none" stroke="%s" stroke-width="4"/>',
    "close": '<path d="M16 16l16 16M32 16L16 32" stroke="%s" stroke-width="5" stroke-linecap="round"/>',
}


def draw_ball(st, glyph, x, y, size):
    fill, d = grad(st["fill"])
    return ('<g transform="translate(%s,%s) scale(%s)">%s' % (x, y, size / 48, d) +
            '<circle cx="24" cy="24" r="20" fill="%s" stroke="%s" stroke-width="1.6"/>' % (fill, st["border"]) +
            '<ellipse cx="24" cy="15" rx="14" ry="9" fill="url(#shine)" opacity="0.8"/>' +
            GLYPH[glyph] % st.get("text", "#ffffff") + "</g>")


ICONS["minimize"] = icon_svg([], draw_ball(S("Minimize and maximize buttons", "normal"), "minimize", 0, 0, 48))
ICONS["maximize"] = icon_svg([], draw_ball(S("Minimize and maximize buttons", "normal"), "maximize", 0, 0, 48))
ICONS["close"] = icon_svg([], draw_ball(S("Close button", "normal"), "close", 0, 0, 48))


# --- the Start button --------------------------------------------------------------
# Shape C ("ramp"): flat on the left (the screen edge), the full height of the
# taskbar — it touches the bar's top edge and the bottom of the screen — the
# top slopes down on the right to a rounded nose. Design height 64; drawn at
# the taskbar's height (BAR_H) by scaling.
START_PATH = "M0,0 H136 C150,0 158,2 170,6 L238,30 C252,35 252,64 236,64 H0 Z"
START_W, START_H = 250, 64
START_DEFS = ('<clipPath id="startclip"><path d="%s"/></clipPath>' % START_PATH +
              '<filter id="startdrop" x="-10%" y="-20%" width="130%" height="150%">'
              '<feDropShadow dx="2" dy="0" stdDeviation="2" flood-color="#06142c" flood-opacity="0.6"/></filter>'
              '<filter id="textshadow" x="-10%" y="-20%" width="130%" height="150%">'
              '<feDropShadow dx="1" dy="1.5" stdDeviation="1" flood-color="#0b3d0f" flood-opacity="0.75"/></filter>')


def start_button(state="normal"):
    st = S("Start button", state)
    fill, d = grad(st["fill"])
    dy = 1 if state == "pressed" else 0
    return (d + '<g filter="url(#startdrop)"><path d="%s" fill="%s"/></g>' % (START_PATH, fill) +
            '<rect width="260" height="30" fill="url(#shine)" opacity="%s" clip-path="url(#startclip)"/>' % st["shine"] +
            '<path d="%s" fill="none" stroke="%s" stroke-width="1.5" clip-path="url(#startclip)"/>'
            % (START_PATH, st["border"]) +
            '<g transform="translate(0,%d)">' % dy +
            '<circle cx="32" cy="32" r="12.5" fill="none" stroke="#ffffff" stroke-width="3.2"/>'
            '<circle cx="32" cy="32" r="4.6" fill="#ffffff"/>'
            '<text x="55" y="43" font-family="%s" font-size="30" font-weight="bold" font-style="italic" '
            'fill="%s" filter="url(#textshadow)">Start</text></g>' % (FONT_TITLE, st["text"]))


# --- the mockup ----------------------------------------------------------------------
W, H, BAR_H = 1024, 768, 40


def img(name, x, y, size, opacity=1):
    return '<image href="icons/png/96/%s.png" x="%s" y="%s" width="%s" height="%s"%s/>' % (
        name, x, y, size, size, ' opacity="%s"' % opacity if opacity != 1 else "")


def text(x, y, s, size=12, color=None, weight="normal", font=FONT_UI, anchor="start", extra=""):
    s = s.replace("&", "&amp;").replace("<", "&lt;")
    return ('<text x="%s" y="%s" font-family="%s" font-size="%s" font-weight="%s" fill="%s" '
            'text-anchor="%s" %s>%s</text>' % (x, y, font, size, weight, color or C["text"], anchor, extra, s))


COMMON_DEFS = (SHINE + START_DEFS +
               '<filter id="curlshadow" x="-30%" y="-30%" width="160%" height="160%">'
               '<feDropShadow dx="-1" dy="1.5" stdDeviation="1.5" flood-color="#000" flood-opacity="0.35"/></filter>' +
               '<linearGradient id="goldline" x1="0" y1="0" x2="1" y2="0">'
               '<stop offset="0" stop-color="#ffc56b" stop-opacity="0"/><stop offset="0.5" stop-color="#ffc56b"/>'
               '<stop offset="1" stop-color="#ffc56b" stop-opacity="0"/></linearGradient>' +
               '<filter id="labelshadow" x="-20%" y="-20%" width="140%" height="160%">'
               '<feDropShadow dx="1" dy="1" stdDeviation="0.8" flood-color="#000" flood-opacity="0.8"/></filter>' +
               "".join('<filter id="shadow-%s" x="-5%%" y="-5%%" width="115%%" height="115%%">'
                       '<feDropShadow dx="0" dy="%s" stdDeviation="%s" flood-color="#000" flood-opacity="%s"/></filter>'
                       % ((state,) + S("Window title bar", state)["shadow"]) for state in ("active", "inactive")))


def title_bar(st, x, y, w, title, icon, buttons=True):
    """The 30 px top of a window, rounded on top, with its icon, title and buttons."""
    fill, d = grad(st["fill"])
    o = [d, '<path d="M%s %s a8 8 0 0 1 8-8 h%s a8 8 0 0 1 8 8 v22 h-%s z" fill="%s" stroke="%s" '
            'stroke-width="1.5"/>' % (x, y + 8, w - 16, w, fill, st["border"]),
         '<rect x="%s" y="%s" width="%s" height="12" rx="6" fill="url(#shine)" opacity="%s"/>'
         % (x + 6, y + 2, w - 12, st["shine"])]
    active = st["buttons"] == "normal"
    o.append(img(icon, x + 8, y + 6, 18, 1 if active else 0.75))
    o.append(text(x + 32, y + 20, title, 13, st["text"], "bold", FONT_TITLE,
                  extra='filter="url(#labelshadow)"' if active else ""))
    if buttons:
        for k, (name, element) in enumerate([("minimize", "Minimize and maximize buttons"),
                                             ("maximize", "Minimize and maximize buttons"),
                                             ("close", "Close button")]):
            o.append(draw_ball(S(element, st["buttons"]), name, x + w - 78 + k * 24, y + 5, 21))
    return "".join(o)


def curl(st, x, y):
    """The page curl whose top-right corner is at (x, y). The flap is the cut-off
    corner folded back over the line from (x-s, y) to (x, y+s), with a rounded tip."""
    s = st["size"]
    under, d1 = grad(st["fill"])
    flap, d2 = grad(st["flap"])
    return (d1 + d2 +
            '<path d="M%s %s H%s V%s Z" fill="%s"/>' % (x - s, y, x, y + s, under) +
            '<g filter="url(#curlshadow)"><path d="M%s %s L%s %s Q%s %s %s %s Q%s %s %s %s Z" fill="%s" '
            'stroke="%s" stroke-width="0.8"/></g>'
            % (x - s, y, x, y + s, x - s * 0.62, y + s * 1.02, x - s * 0.9, y + s * 0.9,
               x - s * 1.02, y + s * 0.62, x - s, y, flap, st["border"]) +
            '<path d="M%s %s L%s %s" stroke="#ffffff" stroke-opacity="0.7" stroke-width="1"/>'
            % (x - s + 1, y + 0.5, x - 0.5, y + s - 1))


def window(x, y, w, h, title, icon, state):
    st = S("Window title bar", state)
    return ('<g filter="url(#shadow-%s)"><rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="%s" '
            'stroke="%s" stroke-width="1.5"/></g>' % (state, x, y, w, h, C["panel_white"], st["frame"]) +
            title_bar(st, x, y, w, title, icon))


def mockup():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s'
         '<linearGradient id="sand" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
         '<stop offset="1" stop-color="%s"/></linearGradient></defs>'
         % (W, H, W, H, COMMON_DEFS, C["panel_sand"], C["panel_sand_deep"])]
    o.append('<image href="wallpaper.jpg" width="%d" height="%d" preserveAspectRatio="xMidYMid slice"/>' % (W, H))

    # desktop icons
    for k, (name, label) in enumerate([("my-files", "My Files"), ("disks", "Disks"), ("network", "Network"),
                                       ("terminal", "Terminal"), ("recycle", "Recycle")]):
        y = 18 + k * 82
        o.append(img(name, 26, y, 48))
        o.append(text(50, y + 64, label, 12, "#ffffff", anchor="middle", extra='filter="url(#labelshadow)"'))
    o.append(curl(S("Hot corner", "normal"), W, 0))

    # a window without the focus: Terminal
    tx, ty, tw, th = 470, 470, 470, 236
    o.append(window(tx, ty, tw, th, "Terminal - bash", "terminal", "inactive"))
    o.append('<path d="M%s %s h%s v%s a7 7 0 0 1-7 7 h-%s a7 7 0 0 1-7-7 z" fill="%s"/>'
             % (tx + 1, ty + 30, tw - 2, th - 38, tw - 16, C["screen"]))
    for k, (line, color) in enumerate([("rodger@myos:~$ ls /data", "#d8f5dc"),
                                       ("dir  hello.txt  lost+found  numbers.txt", "#9fd8ff"),
                                       ("pattern.bin  persist.txt", "#9fd8ff"),
                                       ("rodger@myos:~$ cat /data/hello.txt", "#d8f5dc"),
                                       ("hello from MyOS", "#d8f5dc"),
                                       ("rodger@myos:~$ _", C["prompt"])]):
        o.append(text(tx + 12, ty + 52 + k * 18, line, 12, color, font="Menlo, monospace",
                      extra='xml:space="preserve"'))

    # the window with the focus: My Files
    wx, wy, ww, wh = 420, 48, 570, 400
    o.append(window(wx, wy, ww, wh, "My Files - /data", "folder", "active"))
    # menu bar + toolbar
    o.append('<rect x="%d" y="%d" width="%d" height="22" fill="%s"/>' % (wx + 1, wy + 30, ww - 2, C["menu_bar"]))
    for k, m in enumerate(["File", "Edit", "View", "Tools", "Help"]):
        o.append(text(wx + 12 + k * 50, wy + 45, m, 12))
    o.append('<rect x="%d" y="%d" width="%d" height="34" fill="%s"/>' % (wx + 1, wy + 52, ww - 2, C["menu_bar"]))
    o.append('<path d="M%d %dh%d" stroke="%s"/>' % (wx + 1, wy + 52, ww - 2, C["border"]))
    for k, (name, label) in enumerate([("back", "Back"), ("forward", "Forward"), ("up", "Up")]):
        x = wx + 10 + k * 86
        o.append(img(name, x, wy + 57, 24))
        o.append(text(x + 29, wy + 74, label, 12))
    o.append(draw_bar(S("Text box", "normal"), wx + 270, wy + 58, ww - 282, 22))
    o.append(img("folder", wx + 275, wy + 61, 16))
    o.append(text(wx + 296, wy + 74, "/data", 12))
    # task pane
    px, py, pw, ph = wx + 1, wy + 86, 170, wh - 86 - 24
    o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="url(#sand)"/>' % (px, py, pw, ph))
    for gy, title, items in [(py + 10, "File tasks", [("folder", "Make a new folder"), ("text-file", "Copy this file"),
                                                       ("recycle", "Delete this file")]),
                             (py + 128, "Other places", [("disks", "Disks"), ("network", "Network"),
                                                         ("terminal", "Terminal")])]:
        o.append('<rect x="%d" y="%d" width="%d" height="108" rx="6" fill="#ffffff" fill-opacity="0.55" '
                 'stroke="%s"/>' % (px + 8, gy, pw - 16, C["border"]))
        o.append(img("forward", px + 14, gy + 6, 16))
        o.append(text(px + 36, gy + 19, title, 12, C["label_teal"], "bold"))
        for k, (name, label) in enumerate(items):
            o.append(img(name, px + 16, gy + 30 + k * 25, 18))
            o.append(text(px + 40, gy + 44 + k * 25, label, 11))
    # files; hello.txt is selected
    for k, (name, label) in enumerate([("folder", "dir"), ("folder", "lost+found"), ("text-file", "hello.txt"),
                                       ("text-file", "numbers.txt"), ("binary-file", "pattern.bin"),
                                       ("text-file", "persist.txt")]):
        x = wx + 200 + (k % 3) * 120
        y = py + 16 + (k // 3) * 100
        if label == "hello.txt":
            o.append(draw_bar(S("Selected file", "focused"), x + 22, y - 6, 60, 60))
            o.append('<rect x="%s" y="%s" width="76" height="18" rx="3" fill="%s"/>' % (x + 14, y + 53, C["title_mid"]))
        o.append(img(name, x + 28, y, 48))
        o.append(text(x + 52, y + 66, label, 12, "#ffffff" if label == "hello.txt" else None, anchor="middle"))
    # status bar
    o.append('<path d="M%d %d h%d v15 a7 7 0 0 1-7 7 h-%d a7 7 0 0 1-7-7 z" fill="%s"/>'
             % (wx + 1, wy + wh - 23, ww - 2, ww - 16, C["menu_bar"]))
    o.append('<path d="M%d %dh%d" stroke="%s"/>' % (wx + 1, wy + wh - 24, ww - 2, C["border"]))
    o.append(text(wx + 10, wy + wh - 8, "6 objects, 289 KB", 11, C["text_soft"]))
    # Two strokes in the corner: the place you drag to resize the window.
    o.append('<g stroke="#8a8478" stroke-width="1.25" stroke-linecap="round">'
             '<path d="M%d %dl-8-8"/><path d="M%d %dl-8-8"/></g>'
             % (wx + ww - 8, wy + wh - 6, wx + ww - 12, wy + wh - 6))

    # the start menu (open, so the Start button is drawn pressed)
    mw, mh = 390, 430
    mx, my = 0, H - BAR_H - mh
    head = S("Window title bar", "active")
    o.append('<g filter="url(#shadow-active)"><rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="%s" '
             'stroke="%s" stroke-width="1.5"/></g>' % (mx, my, mw, mh, C["panel_white"], head["frame"]))
    fill, d = grad(head["fill"])
    o.append(d + '<path d="M%d %d a8 8 0 0 1 8-8 h%d a8 8 0 0 1 8 8 v48 h-%d z" fill="%s" stroke="%s" '
             'stroke-width="1.5"/>' % (mx, my + 8, mw - 16, mw, fill, head["border"]))
    o.append('<rect x="%d" y="%d" width="%d" height="20" rx="8" fill="url(#shine)" opacity="0.3"/>'
             % (mx + 6, my + 3, mw - 12))
    o.append(img("logo", mx + 10, my + 8, 40))
    o.append(text(mx + 56, my + 36, "rodger", 18, "#ffffff", "bold", FONT_TITLE, extra='filter="url(#labelshadow)"'))
    cy0, cy1 = my + 56, my + mh - 46
    o.append('<rect x="%d" y="%d" width="%d" height="%d" fill="url(#sand)"/>' % (mx + 210, cy0, mw - 211, cy1 - cy0))
    o.append('<path d="M%d %dV%d" stroke="%s"/>' % (mx + 210, cy0, cy1, C["border"]))
    for k, (name, label, sub) in enumerate([("terminal", "Terminal (bash)", "Command line shell"),
                                            ("my-files", "My Files", "Browse your files"),
                                            ("text-editor", "Text Editor", "Edit plain text"),
                                            ("web-browser", "Web Browser", "Browse the web")]):
        y = cy0 + 10 + k * 60
        o.append(img(name, mx + 12, y, 40))
        o.append(text(mx + 60, y + 18, label, 13, weight="bold"))
        o.append(text(mx + 60, y + 34, sub, 11, C["text_soft"]))
    o.append('<path d="M%d %dh%d" stroke="%s"/>' % (mx + 10, cy1 - 44, 190, C["border"]))
    o.append(text(mx + 16, cy1 - 17, "All Programs", 13, weight="bold"))
    o.append(img("all-programs", mx + 160, cy1 - 37, 28))
    hl = S("Menu highlight", "hover")
    for k, (name, label) in enumerate([("my-files", "My Files"), ("disks", "Disks"), ("network", "Network"),
                                       ("settings", "Settings"), ("help", "Help"), ("run", "Run...")]):
        y = cy0 + 12 + k * 52
        if label == "Settings":     # the mouse is here
            o.append(draw_bar(hl, mx + 216, y - 6, mw - 228, 44))
        o.append(img(name, mx + 222, y, 32))
        o.append(text(mx + 262, y + 21, label, 13, hl["text"] if label == "Settings" else C["label_teal"], "bold"))
    o.append(d + '<path d="M%d %d h%d v38 a8 8 0 0 1-8 8 h-%d a8 8 0 0 1-8-8 z" fill="%s" stroke="%s" '
             'stroke-width="1.5"/>' % (mx, cy1, mw, mw - 16, fill, head["border"]))
    for name, label, x in [("log-off", "Log off", mx + 170), ("turn-off", "Turn off", mx + 276)]:
        o.append(img(name, x, cy1 + 8, 30))
        o.append(text(x + 36, cy1 + 28, label, 13, "#ffffff", extra='filter="url(#labelshadow)"'))

    # the taskbar
    o.append(draw_bar(S("Taskbar", "normal"), 0, H - BAR_H, W, BAR_H))
    for k, (name, label, state) in enumerate([("folder", "My Files", "active"), ("terminal", "Terminal", "normal")]):
        x = 176 + k * 170
        st = S("Task button", state)
        o.append(draw_bar(st, x, H - BAR_H + 5, 160, 30))
        o.append(img(name, x + 8, H - BAR_H + 11, 18))
        o.append(text(x + 32, H - BAR_H + 25, label, 12, st["text"], "bold" if st.get("bold") else "normal"))
    trx = W - 150
    o.append(draw_bar(S("Tray", "normal"), trx, H - BAR_H, 150, BAR_H))
    o.append(img("tray-network", trx + 12, H - BAR_H + 11, 18))
    o.append(img("tray-volume", trx + 38, H - BAR_H + 11, 18))
    o.append(text(trx + 104, H - BAR_H + 25, "8:11 PM", 12, "#ffffff", anchor="middle"))
    o.append('<g transform="translate(0,%d) scale(%s)">%s</g>' % (H - BAR_H, BAR_H / START_H, start_button("pressed")))
    o.append("</svg>\n")
    return "".join(o)


# --- the login screen ------------------------------------------------------------------
def user_picture(kind, x, y, size, opacity=1, tag=None):
    """rodger's picture is a piece of the wallpaper; guest gets a plain silhouette."""
    tag = tag or kind
    o = ['<g opacity="%s"><clipPath id="pic-%s"><rect x="%s" y="%s" width="%s" height="%s" rx="8"/></clipPath>'
         % (opacity, tag, x, y, size, size)]
    if kind == "rodger":
        o.append('<image href="wallpaper.jpg" x="%s" y="%s" width="%s" height="%s" preserveAspectRatio="xMidYMid slice" '
                 'clip-path="url(#pic-%s)"/>' % (x - size * 1.6, y - size * 1.5, size * 4.2, size * 3.15, tag))
    else:
        fill, d = grad(G("v", (0, C["teal_btn_bottom"]), (1, C["title_bottom"])))
        o.append(d + '<rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="%s"/>' % (x, y, size, size, fill))
        s = size / 64
        o.append('<g transform="translate(%s,%s) scale(%s)" fill="#ffffff" opacity="0.9">'
                 '<circle cx="32" cy="24" r="11"/><path d="M12 61c0-13 9-21 20-21s20 8 20 21z"/></g>'
                 % (x, y, s))
    o.append('<rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="none" stroke="#ffffff" stroke-width="2"/></g>'
             % (x, y, size, size))
    return "".join(o)


def login():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s'
         '<filter id="blur" x="-5%%" y="-5%%" width="110%%" height="110%%"><feGaussianBlur stdDeviation="6"/></filter>'
         '<linearGradient id="divider" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity="0"/>'
         '<stop offset="0.5" stop-color="#ffffff" stop-opacity="0.6"/><stop offset="1" stop-color="#ffffff" '
         'stop-opacity="0"/></linearGradient></defs>' % (W, H, W, H, COMMON_DEFS)]
    o.append('<image href="wallpaper.jpg" x="-20" y="-20" width="%d" height="%d" preserveAspectRatio="xMidYMid slice" '
             'filter="url(#blur)"/>' % (W + 40, H + 40))
    o.append(draw_bar(S("Login background", "normal"), 0, 0, W, H))

    # top and bottom bands
    band = S("Login bands", "normal")
    o.append(draw_bar(band, 0, 0, W, 64))
    o.append('<g transform="translate(0,%d) scale(1,-1)">%s</g>' % (H, draw_bar(band, 0, 0, W, 80)))
    o.append(text(W - 32, 40, "Welcome", 22, "#ffffff", "bold", FONT_TITLE, anchor="end",
                  extra='font-style="italic" opacity="0.9"'))

    # left: the logo and the hint
    o.append(img("logo", 362, 262, 108))
    o.append(text(470, 410, "MyOS", 54, "#ffffff", "bold", FONT_TITLE, anchor="end",
                  extra='font-style="italic" filter="url(#labelshadow)"'))
    o.append(text(470, 446, "To begin, click your user name", 15, C["teal_btn_top"], anchor="end"))
    o.append('<rect x="511" y="170" width="1.5" height="430" fill="url(#divider)"/>')

    # right: the users; rodger is selected
    tx = 548
    sel = S("User tile", "selected")
    o.append(draw_bar(sel, tx, 262, 400, 120))
    o.append(user_picture("rodger", tx + 14, 276, 72))
    o.append(text(tx + 102, 306, "rodger", 22, "#ffffff", "bold", FONT_TITLE, extra='filter="url(#labelshadow)"'))
    o.append(text(tx + 102, 328, "Type your password", 12, C["teal_btn_top"]))
    o.append(draw_bar(S("Text box", "focused"), tx + 102, 340, 220, 26))
    o.append(text(tx + 112, 359, "\u2022" * 8, 14, C["text"], extra='letter-spacing="2"'))
    o.append('<rect x="%s" y="345" width="1.5" height="16" fill="%s"/>' % (tx + 192, C["text"]))
    o.append(img("all-programs", tx + 332, 338, 30))
    o.append(user_picture("guest", tx + 14, 410, 56, 0.85))
    o.append(text(tx + 86, 438, "guest", 20, "#ffffff", font=FONT_TITLE, extra='opacity="0.85"'))
    o.append(text(tx + 86, 456, "No password", 12, C["teal_btn_top"], extra='opacity="0.85"'))

    # bottom band: Turn off on the left, a note on the right
    o.append(img("turn-off", 30, H - 58, 36))
    o.append(text(76, H - 34, "Turn off MyOS", 15, "#ffffff", extra='filter="url(#labelshadow)"'))
    o.append(text(W - 32, H - 44, "After you log on, you can add or change users.", 12, C["teal_btn_top"], anchor="end"))
    o.append(text(W - 32, H - 26, "Go to Settings and click Users.", 12, C["teal_btn_top"], anchor="end"))
    o.append("</svg>\n")
    return "".join(o)


# --- Settings: categories follow macOS System Settings, minus Apple-only items ----
# Wi-Fi sits in the same place as on the Mac: after Software Update, before Bluetooth.
SETTINGS_SIDEBAR = [
    ("updates", "Software Update"),
    None,
    ("wifi", "Wi-Fi"),
    ("bluetooth", "Bluetooth"),
    ("network", "Network"),
    ("vpn", "VPN"),
    ("battery", "Battery"),
    None,
    ("settings", "General"),
    ("accessibility", "Accessibility"),
    ("appearance", "Appearance"),
    ("desktop", "Desktop & Taskbar"),
    ("display", "Displays"),
    ("wallpaper", "Wallpaper"),
    ("notifications", "Notifications"),
    ("sound", "Sound"),
    ("lock", "Lock Screen"),
    ("privacy", "Privacy & Security"),
    ("password", "Password"),
    ("users", "Users"),
    None,
    ("keyboard", "Keyboard"),
    ("mouse", "Mouse"),
    ("printer", "Printers"),
]

SETTINGS_GENERAL = [
    ("about", "About"),
    ("updates", "Software Update"),
    ("disks", "Storage"),
    ("datetime", "Date & Time"),
    ("web-browser", "Language"),
    ("network", "Sharing"),
    ("disks", "Startup Disk"),
    ("recycle", "Transfer or Reset"),
]


def settings_chrome():
    """Wallpaper, curl, and the taskbar with Settings as the focused task."""
    o = ['<image href="wallpaper.jpg" width="%d" height="%d" preserveAspectRatio="xMidYMid slice"/>' % (W, H),
         curl(S("Hot corner", "normal"), W, 0)]
    o.append(draw_bar(S("Taskbar", "normal"), 0, H - BAR_H, W, BAR_H))
    for k, (name, label, state) in enumerate([("settings", "Settings", "active"), ("folder", "My Files", "normal")]):
        x = 176 + k * 170
        st = S("Task button", state)
        o.append(draw_bar(st, x, H - BAR_H + 5, 160, 30))
        o.append(img(name, x + 8, H - BAR_H + 11, 18))
        o.append(text(x + 32, H - BAR_H + 25, label, 12, st["text"], "bold" if st.get("bold") else "normal"))
    trx = W - 150
    o.append(draw_bar(S("Tray", "normal"), trx, H - BAR_H, 150, BAR_H))
    o.append(img("tray-network", trx + 12, H - BAR_H + 11, 18))
    o.append(img("tray-volume", trx + 38, H - BAR_H + 11, 18))
    o.append(text(trx + 104, H - BAR_H + 25, "8:11 PM", 12, "#ffffff", anchor="middle"))
    o.append('<g transform="translate(0,%d) scale(%s)">%s</g>' % (H - BAR_H, BAR_H / START_H, start_button("normal")))
    return "".join(o)


def settings_sidebar(wx, wy, sw, sh, selected):
    o = ['<rect x="%d" y="%d" width="%d" height="%d" fill="url(#sand)"/>' % (wx, wy, sw, sh)]
    o.append(draw_bar(S("Text box", "normal"), wx + 10, wy + 10, sw - 20, 22))
    o.append(text(wx + 18, wy + 26, "Search", 11, C["text_soft"]))
    o.append(user_picture("rodger", wx + 10, wy + 42, 28, tag="sb-rodger"))
    o.append(text(wx + 46, wy + 54, "rodger", 12, weight="bold"))
    o.append(text(wx + 46, wy + 68, "Administrator", 10, C["text_soft"]))
    y, hl = wy + 80, S("Menu highlight", "hover")
    for item in SETTINGS_SIDEBAR:
        if item is None:
            o.append('<path d="M%s %sh%s" stroke="%s"/>' % (wx + 12, y + 4, sw - 24, C["border"]))
            y += 10
            continue
        icon, label = item
        if icon == selected:
            o.append(draw_bar(hl, wx + 6, y, sw - 12, 22))
        o.append(img(icon, wx + 12, y + 3, 16))
        o.append(text(wx + 34, y + 16, label, 11, hl["text"] if icon == selected else C["text"]))
        y += 22
    return "".join(o)


def settings_row(x, y, w, icon, label, extra=""):
    o = ['<rect x="%s" y="%s" width="%s" height="36" rx="6" fill="#ffffff" fill-opacity="0.7" stroke="%s"/>'
         % (x, y, w, C["border"])]
    o.append(img(icon, x + 10, y + 6, 24))
    o.append(text(x + 44, y + 23, label, 13))
    if extra:
        o.append(text(x + w - 28, y + 23, extra, 12, C["text_soft"], anchor="end"))
    o.append(img("forward", x + w - 28, y + 8, 20))
    return "".join(o)


def settings_pane_general(px, py, pw):
    o = [img("settings", px, py, 48),
         text(px + 58, py + 22, "General", 22, weight="bold", font=FONT_TITLE),
         text(px + 58, py + 42, "About this computer, storage, date and language.", 12, C["text_soft"])]
    for k, (icon, label) in enumerate(SETTINGS_GENERAL):
        o.append(settings_row(px, py + 60 + k * 42, pw, icon, label))
    return "".join(o)


def settings_pane_wifi(px, py, pw):
    o = [img("wifi", px, py, 48),
         text(px + 58, py + 22, "Wi-Fi", 22, weight="bold", font=FONT_TITLE),
         text(px + 58, py + 42, "Join a wireless network.", 12, C["text_soft"])]
    o.append('<rect x="%s" y="%s" width="%s" height="56" rx="8" fill="#ffffff" fill-opacity="0.7" stroke="%s"/>'
             % (px, py + 58, pw, C["border"]))
    o.append(text(px + 16, py + 82, "Wi-Fi", 13, weight="bold"))
    o.append(text(px + 16, py + 100, "Off", 11, C["text_soft"]))
    o.append('<rect x="%s" y="%s" width="44" height="24" rx="12" fill="%s"/>' % (px + pw - 60, py + 74, C["border"]))
    o.append('<circle cx="%s" cy="%s" r="10" fill="#ffffff"/>' % (px + pw - 50, py + 86))
    o.append('<rect x="%s" y="%s" width="%s" height="200" rx="8" fill="#ffffff" fill-opacity="0.55" stroke="%s"/>'
             % (px, py + 128, pw, C["border"]))
    o.append(text(px + pw / 2, py + 200, "Wi-Fi is not available yet.", 13, C["text"], "bold", anchor="middle"))
    o.append(text(px + pw / 2, py + 222, "The kernel does not support it. This page will list", 12, C["text_soft"],
                  anchor="middle"))
    o.append(text(px + pw / 2, py + 240, "nearby networks when it does.", 12, C["text_soft"], anchor="middle"))
    return "".join(o)


def settings_pane_users(px, py, pw, dialog=True):
    o = [img("users", px, py, 48),
         text(px + 58, py + 22, "Users", 22, weight="bold", font=FONT_TITLE),
         text(px + 58, py + 42, "Manage who can use this computer.", 12, C["text_soft"])]
    for k, (kind, name, role, note, tag) in enumerate([
            ("rodger", "rodger", "Administrator", "Current user", "pane-rodger"),
            ("guest", "guest", "Standard", "No password", "pane-guest")]):
        y = py + 62 + k * 72
        o.append('<rect x="%s" y="%s" width="%s" height="64" rx="8" fill="#ffffff" fill-opacity="0.7" stroke="%s"/>'
                 % (px, y, pw, C["border"]))
        o.append(user_picture(kind, px + 12, y + 8, 48, tag=tag))
        o.append(text(px + 74, y + 28, name, 15, weight="bold", font=FONT_TITLE))
        o.append(text(px + 74, y + 48, "%s  ·  %s" % (role, note), 12, C["text_soft"]))
        o.append(img("forward", px + pw - 32, y + 22, 20))
    o.append(draw_bar(S("Push button", "default"), px, py + 214, 160, 26, "Create New User..."))
    o.append(text(px + 172, py + 232, "Requires the administrator password.", 11, C["text_soft"]))
    if dialog:
        dw, dh, dx, dy = 340, 196, px + 40, py + 250
        o.append('<rect x="%s" y="%s" width="%s" height="%s" fill="#000000" opacity="0.18"/>'
                 % (px - 16, py - 16, pw + 32, 560))
        o.append(window(dx, dy, dw, dh, "Create New User", "users", "active"))
        o.append(img("lock", dx + 16, dy + 44, 36))
        o.append(text(dx + 62, dy + 58, "Administrator password required", 13, weight="bold"))
        o.append(text(dx + 62, dy + 76, "Type the password for rodger to add", 11, C["text_soft"]))
        o.append(text(dx + 62, dy + 92, "a user account to this computer.", 11, C["text_soft"]))
        o.append(text(dx + 20, dy + 128, "Password:", 12))
        o.append(draw_bar(S("Text box", "focused"), dx + 100, dy + 110, 220, 26))
        o.append(text(dx + 110, dy + 129, "\u2022" * 8, 14, C["text"], extra='letter-spacing="2"'))
        o.append('<rect x="%s" y="%s" width="1.5" height="16" fill="%s"/>' % (dx + 190, dy + 115, C["text"]))
        o.append(draw_bar(S("Push button", "normal"), dx + 148, dy + 154, 80, 24, "Cancel"))
        o.append(draw_bar(S("Push button", "default"), dx + 240, dy + 154, 80, 24, "Continue"))
    return "".join(o)


def settings(page="users"):
    """A 1024x768 desktop with Settings open on `page` (wifi, users, or general)."""
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s'
         '<linearGradient id="sand" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
         '<stop offset="1" stop-color="%s"/></linearGradient></defs>'
         % (W, H, W, H, COMMON_DEFS, C["panel_sand"], C["panel_sand_deep"])]
    o.append(settings_chrome())
    wx, wy, ww, wh, sw = 18, 14, 988, 706, 220
    o.append(window(wx, wy, ww, wh, "Settings", "settings", "active"))
    o.append(settings_sidebar(wx + 1, wy + 30, sw, wh - 31, page))
    o.append('<path d="M%s %sV%s" stroke="%s"/>' % (wx + 1 + sw, wy + 30, wy + wh - 1, C["border"]))
    px, py, pw = wx + sw + 24, wy + 48, ww - sw - 48
    if page == "wifi":
        o.append(settings_pane_wifi(px, py, pw))
    elif page == "general":
        o.append(settings_pane_general(px, py, pw))
    else:
        o.append(settings_pane_users(px, py, pw))
    o.append("</svg>\n")
    return "".join(o)


# --- the icon sheet ---------------------------------------------------------------------
ON_DARK = {"logo", "tray-volume", "tray-network"}     # white icons: shown on the taskbar color


def sheet():
    names = sorted(ICONS)
    cols, cw, ch = 7, 128, 120
    rows = (len(names) + cols - 1) // cols
    w, h = cols * cw, rows * ch + 20
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">'
         '<rect width="100%%" height="100%%" fill="#eceff1"/>' % (w, h, w, h)]
    for k, name in enumerate(names):
        x, y = (k % cols) * cw, (k // cols) * ch + 10
        if name in ON_DARK:
            o.append('<rect x="%d" y="%d" width="80" height="80" rx="10" fill="%s"/>' % (x + 24, y + 2, C["bar_left"]))
        o.append(img(name, x + 40, y + 18, 48))
        o.append(text(x + cw / 2, y + 102, name, 12, C["text"], anchor="middle"))
    o.append("</svg>\n")
    return "".join(o)


# --- the states sheet: every element in every state -------------------------------------
def states_sheet():
    colw, x0, rowh = 180, 250, 74
    w = x0 + colw * max(len(st) for *_x, st in UI) + 10
    h = 20 + rowh * len(UI)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s</defs>'
         '<rect width="100%%" height="100%%" fill="#eceff1"/>' % (w, h, w, h, COMMON_DEFS)]
    for r, (element, where, label, states) in enumerate(UI):
        y = 14 + r * rowh
        o.append(text(16, y + 24, element, 13, C["text"], "bold"))
        if r:
            o.append('<path d="M10 %sh%s" stroke="#d3d8dc"/>' % (y - 4, w - 20))
        for k, (state, st) in enumerate(states):
            x = x0 + k * colw
            kind = st.get("kind", "bar")
            if st.get("on_dark"):
                o.append('<rect x="%s" y="%s" width="170" height="44" rx="6" fill="%s"/>' % (x - 5, y + 1, C["bar_left"]))
            if kind == "bar":
                bw = st.get("w", 150)
                o.append(draw_bar(st, x + (160 - bw) / 2, y + 8, bw, 30, label))
            elif kind == "ball":
                o.append(draw_ball(st, st["glyph"], x + 66, y + 7, 30))
            elif kind == "start":
                o.append('<g transform="translate(%s,%s) scale(0.625)">%s</g>' % (x + 2, y + 3, start_button(state)))
            elif kind == "curl":
                o.append('<svg x="%s" y="%s" width="70" height="44" viewBox="%d 0 70 44">'
                         '<image href="wallpaper.jpg" width="1152" height="864"/></svg>' % (x + 45, y + 4, 1040 - 70))
                o.append(curl(st, x + 115, y + 4))
            elif kind == "title":
                o.append('<rect x="%s" y="%s" width="170" height="8" fill="%s" stroke="%s"/>'
                         % (x - 4, y + 30, C["panel_white"], st["frame"]))
                o.append(title_bar(st, x - 4, y + 2, 170, label, "folder"))
            o.append(text(x + 80, y + 58, state, 11, C["text_soft"], anchor="middle"))
    o.append("</svg>\n")
    return "".join(o)


# --- the color tables in docs/ui-theme.md -------------------------------------------------
def describe(fill):
    if fill == "none":
        return "none (clear)"
    if isinstance(fill, str):
        return "`%s` flat" % fill
    stops = fill["stops"]

    def one(st):
        c = "`%s`" % st[1] + (" (%d%% opaque)" % round(st[2] * 100) if len(st) > 2 else "")
        return c if len(stops) == 2 and len(st) == 2 else "%s at %d%%" % (c, round(st[0] * 100))
    way = "left to right" if fill["dir"] == "h" else "top to bottom"
    return "%s: %s" % (way, " → ".join(one(st) for st in stops))


def color_tables():
    o = ["<!-- Made by make-theme.py from its UI table; edit there, not here. -->", ""]
    for element, where, _label, states in UI:
        o += ["### %s" % element, "", where[0].upper() + where[1:] + ".", "",
              "| state | fill | border | text / glyph | notes |", "|---|---|---|---|---|"]
        for state, st in states:
            notes = []
            if st.get("over"):
                notes.append("shade over it, %s" % describe(st["over"]))
            if st.get("shine"):
                notes.append("gloss %d%%" % round(st["shine"] * 100))
            if st.get("note"):
                notes.append(st["note"])
            if st.get("kind") == "ball":
                border = "`%s` rim, 1/30 of the size" % st["border"]
            elif st.get("border"):
                border = "`%s` %s px" % (st["border"], 1.5 if st.get("kind") in ("start", "title") else st.get("bw", 1))
            else:
                border = "none"
            if st.get("kind") == "title":
                border += "; window frame `%s` 1.5 px" % st["frame"]
            o.append("| %s | %s | %s | %s | %s |" % (state, describe(st["fill"]), border,
                                                       "`%s`" % st["text"] if st.get("text") else "-",
                                                       "; ".join(notes) or ""))
        o.append("")
    return "\n".join(o)


DOC = os.path.join(HERE, "..", "..", "..", "docs", "ui-theme.md")
BEGIN, END = "<!-- colors:begin -->", "<!-- colors:end -->"


def update_doc():
    doc = open(DOC).read()
    a, b = doc.index(BEGIN) + len(BEGIN), doc.index(END)
    open(DOC, "w").write(doc[:a] + "\n" + color_tables() + "\n" + doc[b:])


# --- write everything ----------------------------------------------------------------
def render(svg, png, size=None, zoom=None):
    cmd = ["rsvg-convert", svg, "-o", png]
    if size:
        cmd[1:1] = ["-w", str(size), "-h", str(size)]
    if zoom:
        cmd[1:1] = ["-z", str(zoom)]
    subprocess.run(cmd, check=True, cwd=HERE)


def write(name, svg, **kw):
    open(os.path.join(HERE, name + ".svg"), "w").write(svg)
    render(name + ".svg", name + ".png", **kw)


def main():
    os.makedirs(os.path.join(HERE, "icons/svg"), exist_ok=True)
    for name, svg in ICONS.items():
        path = os.path.join(HERE, "icons/svg/%s.svg" % name)
        open(path, "w").write(svg)
        for size in (16, 24, 32, 48, 96):
            os.makedirs(os.path.join(HERE, "icons/png/%d" % size), exist_ok=True)
            render(path, os.path.join(HERE, "icons/png/%d/%s.png" % (size, name)), size=size)

    for state, suffix in [("normal", ""), ("hover", "-hover"), ("pressed", "-pressed")]:
        write("start-button" + suffix,
              '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><defs>%s</defs>%s</svg>\n'
              % (START_W, START_H, START_W, START_H, COMMON_DEFS, start_button(state)), zoom=2)
    write("mockup", mockup())
    render("mockup.svg", "mockup@2x.png", zoom=2)
    write("login", login())
    render("login.svg", "login@2x.png", zoom=2)
    write("settings", settings("users"))
    render("settings.svg", "settings@2x.png", zoom=2)
    write("settings-wifi", settings("wifi"))
    render("settings-wifi.svg", "settings-wifi@2x.png", zoom=2)
    write("settings-general", settings("general"))
    render("settings-general.svg", "settings-general@2x.png", zoom=2)
    write("icon-sheet", sheet())
    write("ui-states", states_sheet())
    update_doc()
    print("%d icons, start button (3 states), mockup, icon sheet, states sheet, doc tables" % len(ICONS))


if __name__ == "__main__":
    main()
