/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-xsettings — the desktop's scale, told to every program.
 *
 * Modern screens are small and sharp: 1920x1200 on a 13" panel is 170
 * pixels per inch, and a desktop drawn for 96 makes everything a third of
 * the size it should be. So the whole desktop is drawn bigger by a scale
 * factor, 100% to 200%, like the Mac's "Larger Text ... More Space".
 *
 * This program is the one place that knows the scale. It runs for the whole
 * session (started by /etc/myos/xsession, before the window manager) and
 * tells the programs in the ways they listen:
 *
 *   - XSETTINGS (the freedesktop.org protocol GTK listens to): the font DPI,
 *     and a 2x window scale at 200%. GTK programs that are already open
 *     follow a change at once. This is what xsettingsd or a GNOME/Xfce
 *     settings daemon does on other Linux desktops.
 *   - the X resource Xft.dpi: xterm, IceWM and every other Xft program
 *     read it when they start.
 *
 * The scale comes from ~/.config/myos/display.conf ("scale=150", or
 * "scale=auto"), else from the screen's size. Settings (myos-settings)
 * changes it by writing that file and setting the root window property
 * _MYOS_SCALE; this program sees the property change, applies the new scale
 * and restarts IceWM so its fonts follow. _MYOS_SCALE_AUTO holds what
 * "automatic" means on this screen, for Settings to mark as the default.
 *
 * The mouse pointer's size goes the same way: ~/.config/myos/mouse.conf,
 * the root property _MYOS_POINTER (Settings > Mouse & Keyboard), and from
 * here to GTK (Gtk/CursorThemeSize) and every other X program (Xcursor.size).
 *
 * Cost: one sleeping process, ~1 MiB. Nothing runs until the scale changes.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "scale.h"

static Display *dpy;
static Window root, win;
static Atom a_settings, a_scale, a_auto, a_pointer;
static uint32_t serial;

static void set_cardinal(Atom a, long v)
{
    XChangeProperty(dpy, root, a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
}

static long get_cardinal(Atom a, long dflt)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long v = dflt;
    if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
            Success && data && n == 1 && fmt == 32)
        v = *(long *)data;
    if (data)
        XFree(data);
    return v;
}

/* --- XSETTINGS ---------------------------------------------------------------
   The property is a little binary table: byte order, a serial, the number of
   settings, then each setting as type, name and value, padded to 4 bytes. */
static unsigned char buf[512];
static int len;

static void put32(uint32_t v)
{
    memcpy(buf + len, &v, 4);
    len += 4;
}

static void put_name(int type, const char *name)
{
    uint16_t n = strlen(name);
    buf[len] = type;
    buf[len + 1] = 0;
    memcpy(buf + len + 2, &n, 2);
    len += 4;
    memcpy(buf + len, name, n);
    len += (n + 3) & ~3;
    put32(serial);              /* when it last changed */
}

static void put_int(const char *name, int32_t v)
{
    put_name(0, name);
    put32(v);
}

static void put_str(const char *name, const char *s)
{
    uint32_t n = strlen(s);
    put_name(1, name);
    put32(n);
    memcpy(buf + len, s, n);
    len += (n + 3) & ~3;
}

/* IceWM's title bars, buttons and taskbar are pictures, drawn for each of
   100, 125 ... 200% (live/theme/make-live-theme.py): GoldenHour,
   GoldenHour-125 and so on. IceWM reads ~/.icewm/theme after
   /etc/icewm/preferences, so a line there picks the one for this scale. */
static void icewm_theme(int scale)
{
    const char *home = getenv("HOME");
    char path[256];
    if (!home)
        return;
    int s = (scale + 12) / 25 * 25;
    s = s < 100 ? 100 : s > 200 ? 200 : s;
    snprintf(path, sizeof path, "%s/.icewm", home);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/.icewm/theme", home);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    if (s == 100)
        fprintf(f, "Theme=\"GoldenHour/default.theme\"\n");
    else
        fprintf(f, "Theme=\"GoldenHour-%d/default.theme\"\n", s);
    fclose(f);
}

/* Apply a scale and a pointer size; restart says whether IceWM is already
   running and must start over to follow (its fonts, theme and pointers are
   read once, when it starts). */
static void apply(int scale, int pointer, int restart)
{
    icewm_theme(scale);
    int wsf = scale >= 200 ? 2 : 1;             /* GTK draws whole windows 2x */
    int dpi = 96 * scale / 100;
    int cursor = pointer_pixels(scale, pointer);

    memset(buf, 0, sizeof buf);
    len = 0;
    serial++;
    buf[0] = 0;                                  /* LSBFirst: x86 */
    len = 4;
    put32(serial);
    put32(7);
    put_int("Xft/DPI", dpi * 1024);
    put_int("Gdk/WindowScalingFactor", wsf);
    put_int("Gdk/UnscaledDPI", dpi * 1024 / wsf);
    put_int("Xft/Antialias", 1);
    put_str("Xft/HintStyle", "hintslight");
    /* GTK multiplies the pointer by the window scale itself. */
    put_str("Gtk/CursorThemeName", "Adwaita");
    put_int("Gtk/CursorThemeSize", cursor / wsf);
    XChangeProperty(dpy, win, a_settings, a_settings, 8, PropModeReplace, buf, len);

    /* Xft.dpi for xterm, IceWM and the rest, and the pointer for every X
       program (libXcursor reads Xcursor.*; it's how IceWM and xterm get a
       pointer bigger than X's own 16-pixel arrow). xrdb merges them into
       the resources already there; -nocpp because there's nothing to expand. */
    FILE *p = popen("xrdb -nocpp -merge", "w");
    if (p) {
        fprintf(p, "Xft.dpi: %d\nXcursor.theme: Adwaita\nXcursor.size: %d\n", dpi, cursor);
        pclose(p);
    }
    XFlush(dpy);
    if (restart && system("icesh restart >/dev/null 2>&1 &") < 0)
        perror("icesh");
}

/* The selection must be taken with a real timestamp: get one from the
   server by changing a property and reading the event's time. */
static Time server_time(void)
{
    XEvent e;
    unsigned char c = 0;
    XSelectInput(dpy, win, PropertyChangeMask);
    XChangeProperty(dpy, win, XA_STRING, XA_STRING, 8, PropModeAppend, &c, 0);
    XWindowEvent(dpy, win, PropertyChangeMask, &e);
    return e.xproperty.time;
}

int main(void)
{
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "myos-xsettings: no X display\n");
        return 1;
    }
    int scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    win = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    a_settings = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
    a_scale = XInternAtom(dpy, "_MYOS_SCALE", False);
    a_auto = XInternAtom(dpy, "_MYOS_SCALE_AUTO", False);
    a_pointer = XInternAtom(dpy, "_MYOS_POINTER", False);
    char selname[32];
    snprintf(selname, sizeof selname, "_XSETTINGS_S%d", scr);
    Atom sel = XInternAtom(dpy, selname, False);

    Time t = server_time();
    XSetSelectionOwner(dpy, sel, win, t);
    if (XGetSelectionOwner(dpy, sel) != win) {
        fprintf(stderr, "myos-xsettings: another settings manager is running\n");
        return 1;
    }
    /* Announce it, as the spec says: programs waiting for a manager wake up. */
    XClientMessageEvent m = {.type = ClientMessage, .window = root, .format = 32,
                             .message_type = XInternAtom(dpy, "MANAGER", False)};
    m.data.l[0] = t;
    m.data.l[1] = sel;
    m.data.l[2] = win;
    XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent *)&m);

    int chosen = scale_read_conf();
    int scale = chosen ? chosen : auto_scale(DisplayWidth(dpy, scr));
    int pointer = pointer_read_conf();
    set_cardinal(a_auto, auto_scale(DisplayWidth(dpy, scr)));
    set_cardinal(a_scale, chosen);
    set_cardinal(a_pointer, pointer);
    apply(scale, pointer, 0);
    XSelectInput(dpy, root, PropertyChangeMask);
    XSync(dpy, False);

    /* The session waits for us (the window manager must start with the
       right Xft.dpi), so go to the background only now. */
    if (fork() > 0)
        _exit(0);
    setsid();

    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == PropertyNotify && e.xproperty.window == root && e.xproperty.atom == a_scale) {
            long v = get_cardinal(a_scale, 0);
            int s = v ? clamp_scale(v) : auto_scale(DisplayWidth(dpy, scr));
            if (s != scale) {
                scale = s;
                apply(scale, pointer, 1);
            }
        } else if (e.type == PropertyNotify && e.xproperty.window == root && e.xproperty.atom == a_pointer) {
            int p = clamp_pointer(get_cardinal(a_pointer, POINTER_MIN));
            if (p != pointer) {
                pointer = p;
                apply(scale, pointer, 1);
            }
        } else if (e.type == SelectionClear) {
            return 0;               /* another manager took over */
        }
    }
}
