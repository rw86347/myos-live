/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-mission — Mission Control for the X11 desktop.
 *
 * myos-menu starts this: a click on the page curl (its hot corner), a push
 * into that corner, or three fingers sliding up the touchpad. The
 * windows of this desktop glide from where they are to where the Wayland
 * session would put them (live/myos-wm/layout.c: no overlaps, the biggest
 * stays the biggest, each near where it was), the desktop dims, and the
 * Spaces bar drops in from the top: a miniature of each of IceWM's desktops
 * (four, see /etc/icewm/prefoverride) on smoked glass. The window under the
 * mouse gets a blue ring and its title. Click a window to go to it, or a
 * miniature to go to that desktop; the background, Escape, or three fingers
 * sliding down goes back.
 * Drag a window onto a miniature to move it to that desktop: over the bar
 * it shrinks to the miniature's size, and once it is dropped the windows
 * left glide to a new layout; dropped anywhere else, it glides back.
 * Going back glides every window home. The zoom is myos-wm's: 250 ms,
 * cubic ease-out (fast start, soft landing); "reduce motion"
 * (~/.config/myos/motion.conf) skips it.
 *
 * Where the pictures come from: IceWM puts every window inside a frame of
 * its own (title bar, borders), and the frame is a child of the root
 * window. The Composite extension can keep each child of the root in a
 * pixmap of its own ("redirect" it), whole, even the parts other windows
 * cover. So this program redirects the root's children while it runs,
 * asks for each frame's pixmap, and has the X server scale it (XRender).
 * A program's own window, inside the frame, has no pixmap of its own:
 * that is why the frame is used. The taskbar and the desktop's icons are
 * children of the root too; they are drawn where they are, under the
 * dimming, so the first frame of the zoom is the screen as it was.
 * Windows on other desktops are unmapped, and an unmapped window has no
 * pixmap, so their miniatures show each one as a pale pane.
 *
 * Cost: each frame of the zoom is built in the X server (pixman: in
 * software, on most of our machines) in a back pixmap, then copied to the
 * screen in one go. A slow frame makes a bigger step, so the zoom ends at
 * 250 ms however slow the machine is. Once open, only three things draw:
 * the pictures again at 300 and 700 ms (programs repaint into their new
 * pixmaps after the redirect); the ring and title, which moving the mouse
 * copies away and draws anew; and a drag. A drag first keeps everything
 * but the dragged window in a second pixmap, so each move of the mouse is
 * one copy and one window.
 */
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#include "layout.h"

/* The sysroot has libXcomposite, and not its header. These are the calls
   this program uses (Xcomposite.h). Automatic: the X server still puts the
   windows on the screen itself, and keeps a pixmap of each one as well. */
#define COMPOSITE_REDIRECT_AUTOMATIC 0
Bool XCompositeQueryExtension(Display *, int *, int *);
void XCompositeRedirectSubwindows(Display *, Window, int);
Pixmap XCompositeNameWindowPixmap(Display *, Window);

#define MC_MS      250              /* the zoom, as myos-wm's */
#define FRAME_MS   16               /* about 60 frames a second */
#define WAIT_MS    150              /* for IceWM to switch, before this goes away */
#define DRAG_START 6                /* pixels the pointer moves before a click is a drag */
#define MAX_DESKS  16
/* Three fingers down on the touchpad: the kernel's HID_SWIPE_DOWN key (F17,
   include/hid.h), whose X keycode is the Linux code + 8 (myos-menu). */
#define SWIPE_DOWN (187 + 8)
#define MAX_FIXED  32

enum {
    A_STACKING, A_CLIENTS, A_DESKTOP, A_CURRENT, A_NDESKS, A_WORKAREA, A_ACTIVE, A_NAME,
    A_UTF8, A_STATE, A_HIDDEN, A_TYPE, A_ROOTPM, A_SCALE, A_SCALE_AUTO,
    A_SKIP,                         /* from here on: window types nobody picks */
    NATOMS = A_SKIP + 9
};
static const char *const atom_names[NATOMS] = {
    "_NET_CLIENT_LIST_STACKING", "_NET_CLIENT_LIST", "_NET_WM_DESKTOP",
    "_NET_CURRENT_DESKTOP", "_NET_NUMBER_OF_DESKTOPS", "_NET_WORKAREA", "_NET_ACTIVE_WINDOW",
    "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN",
    "_NET_WM_WINDOW_TYPE", "_XROOTPMAP_ID", "_MYOS_SCALE", "_MYOS_SCALE_AUTO",
    "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_DND",
};
static Atom atom[NATOMS];

struct thumb {                      /* a window on this desktop */
    Window client;                  /* the program's window: what gets activated */
    Window frame;                   /* IceWM's frame around it: the picture */
    Picture pic;                    /* or None: a gray pane */
    int op;                         /* PictOpOver if the frame has see-through parts */
    int sticky;                     /* on every desktop */
    struct mc_rect at;              /* on the desktop */
    struct mc_rect show;            /* where Mission Control puts it */
    struct mc_rect now;             /* where this frame drew it */
    struct mc_rect from;            /* where a new layout glides it from */
    char name[96];
};

struct pane {                       /* a window on another desktop */
    int desk;
    struct mc_rect at;
};

struct fixed {                      /* the taskbar, the icons: they stay put */
    Picture pic;
    int op;
    struct mc_rect at;
};

static Display *dpy;
static Window root, win;
static int sw, sh, depth, scale, xerr, composite;
static unsigned long current;       /* the desktop we are on */
static int ndesks;
static struct thumb th[MC_MAX];
static int nth;
static struct pane pane[MC_MAX];
static int npane;
static struct fixed fx[MAX_FIXED];
static int nfx;

static Pixmap back;                 /* the frame being built */
static Picture back_pic, win_pic, wall;
static GC copy_gc;
static XftDraw *xft, *xft_back;     /* on the screen (the title), on back (names) */
static XftFont *font;
static XftColor white;
static int ring_w, label_gap, label_max;
static int mini_w, mini_h, mini_y, bar_h, bar_off, mini_x[MAX_DESKS];

static double p;                    /* 0 = windows where they are, 1 = laid out */
static int dir;                     /* +1 opening, -1 closing, 0 still */
static int pick = -1;               /* the window chosen: drawn on top going home */
static int hover = -1;
static struct mc_rect decor;        /* what the ring and title cover on the screen */
static double re = 1;               /* windows gliding to a new layout: 0 .. 1 */

static int press = -1;              /* the window the button went down on */
static int press_x, press_y, drag_x, drag_y;
static int dragging;                /* it moved: a drag, not a click */
static int drop_desk = -1;          /* the miniature under the dragged window */
static Pixmap base;                 /* while dragging: everything but that window */
static int base_ok;

static const XRenderColor dark = {0x1800, 0x1800, 0x1c00, 0xffff};      /* no wallpaper */
static const XRenderColor gray = {0x3000, 0x3000, 0x3400, 0xffff};      /* no picture */
static const XRenderColor pale = {0xb000, 0xb000, 0xb800, 0xe000};      /* another desktop's */
static const XRenderColor smoke = {0x2900, 0x2900, 0x2e00, 0x6000};     /* the bar's glass */
static const XRenderColor ring_white = {0xe666, 0xe666, 0xe666, 0xe666};
static const XRenderColor blue = {0x0a3d, 0x851e, 0xffff, 0xffff};      /* myos-wm's ring */
static const XRenderColor pill = {0x1400, 0x1400, 0x1400, 0xc000};      /* behind the title */

static int on_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    xerr = 1;
    return 0;
}

/* Up to cap 32-bit items of a property (Xlib hands them over as longs).
   Returns how many there were. */
static int prop32(Window w, Atom a, unsigned long *out, int cap)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, a, 0, cap, False, AnyPropertyType, &type, &fmt, &n, &after,
                           &data) != Success || !data)
        return 0;
    if (fmt != 32)
        n = 0;
    memcpy(out, data, n * sizeof *out);
    XFree(data);
    return (int)n;
}

static int has_atom(Window w, Atom prop, int from, int to)
{
    unsigned long v[8];
    int n = prop32(w, prop, v, 8);
    for (int i = 0; i < n; i++)
        for (int k = from; k < to; k++)
            if (v[i] == atom[k])
                return 1;
    return 0;
}

/* The desktop's scale in percent, as myos-xsettings set it (see myos-menu). */
static int desktop_scale(void)
{
    unsigned long s = 0;
    if (!prop32(root, atom[A_SCALE], &s, 1) || !s)
        if (!prop32(root, atom[A_SCALE_AUTO], &s, 1) || !s)
            s = 100;
    return s < 100 ? 100 : s > 300 ? 300 : (int)s;
}

static int reduce_motion(void)
{
    char path[512], line[64];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config/myos/motion.conf", home ? home : "");
    FILE *f = fopen(path, "r");
    int on = 0;
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "reduce=", 7))
                on = line[7] == '1';
        fclose(f);
    }
    return on;
}

/* IceWM's frame around a program's window: its ancestor that is a child of
   the root. (Frames are a level or two deep, never more than a few.) */
static Window frame_of(Window w)
{
    for (int depth = 0; depth < 8; depth++) {
        Window r, parent, *kids = NULL;
        unsigned n;
        if (!XQueryTree(dpy, w, &r, &parent, &kids, &n))
            return 0;
        if (kids)
            XFree(kids);
        if (parent == root)
            return w;
        w = parent;
    }
    return 0;
}

static void window_name(Window w, char *out, int cap)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    out[0] = 0;
    if (XGetWindowProperty(dpy, w, atom[A_NAME], 0, cap / 4, False, atom[A_UTF8], &type, &fmt,
                           &n, &after, &data) == Success && data && n) {
        snprintf(out, cap, "%.*s", (int)n, (char *)data);
        XFree(data);
        return;
    }
    if (data)
        XFree(data);
    char *old = NULL;
    if (XFetchName(dpy, w, &old) && old) {
        snprintf(out, cap, "%s", old);
        XFree(old);
    }
}

/* A child of the root as a picture the X server can scale, or None. */
static Picture picture_of(Window w, Visual *v, int *op)
{
    XRenderPictFormat *pf = XRenderFindVisualFormat(dpy, v);
    if (!composite || !pf)
        return None;
    xerr = 0;
    Pixmap pm = XCompositeNameWindowPixmap(dpy, w);
    Picture pic = XRenderCreatePicture(dpy, pm, pf, 0, NULL);
    XFreePixmap(dpy, pm);           /* the picture keeps it */
    XRenderSetPictureFilter(dpy, pic, FilterBilinear, NULL, 0);
    XSync(dpy, False);
    if (xerr)
        return None;
    *op = pf->type == PictTypeDirect && pf->direct.alphaMask ? PictOpOver : PictOpSrc;
    return pic;
}

/* Every window, bottom to top: this desktop's get a picture and a place;
   other desktops' are remembered for their miniatures. */
static void collect(void)
{
    unsigned long list[256];
    int n = prop32(root, atom[A_STACKING], list, 256);
    if (!n)
        n = prop32(root, atom[A_CLIENTS], list, 256);
    for (int i = 0; i < n; i++) {
        Window w = list[i];
        if (has_atom(w, atom[A_TYPE], A_SKIP, NATOMS))
            continue;
        unsigned long on = current;
        prop32(w, atom[A_DESKTOP], &on, 1);
        XWindowAttributes a;
        xerr = 0;
        Window f = frame_of(w);
        if (!f || !XGetWindowAttributes(dpy, f, &a) || xerr || a.width < 16 || a.height < 16)
            continue;
        int b = a.border_width;
        struct mc_rect at = {a.x, a.y, a.width + 2 * b, a.height + 2 * b};
        int sticky = on == 0xFFFFFFFFul;
        if (on != current && !sticky) {
            if (on < (unsigned long)ndesks && npane < MC_MAX &&
                !has_atom(w, atom[A_STATE], A_HIDDEN, A_HIDDEN + 1))
                pane[npane++] = (struct pane){(int)on, at};
            continue;
        }
        if (a.map_state != IsViewable || nth >= MC_MAX)
            continue;
        struct thumb *t = &th[nth++];
        t->client = w;
        t->frame = f;
        t->sticky = sticky;
        t->at = t->now = at;
        t->op = PictOpSrc;
        t->pic = picture_of(f, a.visual, &t->op);
        window_name(w, t->name, (int)sizeof t->name);
    }
}

/* The rest of the screen that shows: the taskbar, the icons, the curl. */
static void collect_fixed(void)
{
    Window r, parent, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(dpy, root, &r, &parent, &kids, &n))
        return;
    for (unsigned i = 0; i < n && nfx < MAX_FIXED; i++) {
        int laid_out = 0;
        for (int k = 0; k < nth && !laid_out; k++)
            laid_out = th[k].frame == kids[i];
        XWindowAttributes a;
        xerr = 0;
        if (laid_out || !XGetWindowAttributes(dpy, kids[i], &a) || xerr ||
            a.map_state != IsViewable || a.class != InputOutput)
            continue;
        int op = PictOpSrc;
        Picture pic = picture_of(kids[i], a.visual, &op);
        int b = a.border_width;
        if (pic)
            fx[nfx++] = (struct fixed){pic, op, {a.x, a.y, a.width + 2 * b, a.height + 2 * b}};
    }
    if (kids)
        XFree(kids);
}

/* The Spaces bar: a miniature 11% of the screen wide per desktop, as in
   myos-wm, centered, with its name under it. */
static void bar_geometry(void)
{
    int gap = 24 * scale / 100;
    mini_w = sw * 11 / 100;
    if (ndesks * mini_w + (ndesks + 1) * gap > sw)
        mini_w = (sw - (ndesks + 1) * gap) / ndesks;
    mini_h = mini_w * sh / (sw ? sw : 1);
    mini_y = 12 * scale / 100;
    bar_h = mini_y + mini_h + 6 * scale / 100 + (font ? font->height : 0) + 10 * scale / 100;
    int total = ndesks * mini_w + (ndesks - 1) * gap;
    for (int d = 0; d < ndesks; d++)
        mini_x[d] = (sw - total) / 2 + d * (mini_w + gap);
}

/* Where each window goes: inside the work area (not on the taskbar), under
   the bar, with room around each for the ring and under it for a title. */
static void place(void)
{
    struct mc_rect in[MC_MAX], out[MC_MAX];
    unsigned long wa[4 * MAX_DESKS];
    int nwa = prop32(root, atom[A_WORKAREA], wa, 4 * MAX_DESKS);
    struct mc_rect work = {0, 0, sw, sh};
    if (nwa >= 4) {
        int k = (int)current * 4 + 4 <= nwa ? (int)current * 4 : 0;
        work = (struct mc_rect){(int)wa[k], (int)wa[k + 1], (int)wa[k + 2], (int)wa[k + 3]};
    }
    int edge = 24 * scale / 100 + ring_w;
    int title = font ? font->height * 3 / 2 + label_gap : 0;
    int top = work.y > bar_h ? work.y : bar_h;
    struct mc_rect area = {work.x + edge, top + edge, work.w - 2 * edge,
                           work.y + work.h - top - 2 * edge - title};
    if (area.w < 100 || area.h < 100)
        area = work;
    for (int i = 0; i < nth; i++)
        in[i] = th[i].at;
    if (nth)
        mc_layout(in, out, nth, area, 24 * scale / 100 + 2 * ring_w);
    for (int i = 0; i < nth; i++) {
        th[i].show = out[i];
        th[i].show.w = out[i].w < 1 ? 1 : out[i].w;
        th[i].show.h = out[i].h < 1 ? 1 : out[i].h;
    }
}

static double ease(double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double u = 1 - t;
    return 1 - u * u * u;
}

static int lerp_i(int a, int b, double t)
{
    double v = a + (b - a) * t;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

static struct mc_rect lerp_r(struct mc_rect a, struct mc_rect b, double t)
{
    return (struct mc_rect){lerp_i(a.x, b.x, t), lerp_i(a.y, b.y, t), lerp_i(a.w, b.w, t),
                            lerp_i(a.h, b.h, t)};
}

/* Picture pic (w x h) scaled into r on the back pixmap, only inside clip.
   XRender's transform maps a point of the destination to the source, so
   it is the source's size over the place's size; the source offset is
   given in the scaled picture. */
static void put(Picture pic, int op, int w, int h, struct mc_rect r, struct mc_rect clip)
{
    int x0 = r.x > clip.x ? r.x : clip.x, y0 = r.y > clip.y ? r.y : clip.y;
    int x1 = r.x + r.w < clip.x + clip.w ? r.x + r.w : clip.x + clip.w;
    int y1 = r.y + r.h < clip.y + clip.h ? r.y + r.h : clip.y + clip.h;
    if (r.w < 1 || r.h < 1 || x1 <= x0 || y1 <= y0)
        return;
    XTransform xf = {{
        {XDoubleToFixed((double)w / r.w), 0, 0},
        {0, XDoubleToFixed((double)h / r.h), 0},
        {0, 0, XDoubleToFixed(1)},
    }};
    XRenderSetPictureTransform(dpy, pic, &xf);
    XRenderComposite(dpy, op, pic, None, back_pic, x0 - r.x, y0 - r.y, 0, 0, x0, y0,
                     x1 - x0, y1 - y0);
}

static void fill(const XRenderColor *c, struct mc_rect r, struct mc_rect clip)
{
    int x0 = r.x > clip.x ? r.x : clip.x, y0 = r.y > clip.y ? r.y : clip.y;
    int x1 = r.x + r.w < clip.x + clip.w ? r.x + r.w : clip.x + clip.w;
    int y1 = r.y + r.h < clip.y + clip.h ? r.y + r.h : clip.y + clip.h;
    if (x1 > x0 && y1 > y0)
        XRenderFillRectangle(dpy, c->alpha == 0xffff ? PictOpSrc : PictOpOver, back_pic, c, x0,
                             y0, x1 - x0, y1 - y0);
}

/* The smoked glass, and on it each desktop: the wallpaper with its windows
   shrunk onto it, a white ring around the one we are on, its name. */
static void draw_bar(int off)
{
    struct mc_rect screen = {0, 0, sw, sh};
    fill(&smoke, (struct mc_rect){0, off, sw, bar_h}, screen);
    double k = (double)mini_w / sw;
    for (int d = 0; d < ndesks; d++) {
        struct mc_rect m = {mini_x[d], mini_y + off, mini_w, mini_h};
        if (d == (int)current)
            fill(&ring_white, (struct mc_rect){m.x - 2, m.y - 2, m.w + 4, m.h + 4}, screen);
        if (wall)
            put(wall, PictOpSrc, sw, sh, m, m);
        else
            fill(&dark, m, m);
        for (int i = 0; i < npane; i++)
            if (pane[i].desk == d) {
                struct mc_rect a = pane[i].at;
                fill(&pale, (struct mc_rect){m.x + (int)(a.x * k), m.y + (int)(a.y * k),
                                             (int)(a.w * k), (int)(a.h * k)}, m);
            }
        for (int i = 0; i < nth; i++) {
            if (d != (int)current && !th[i].sticky)
                continue;
            struct mc_rect a = th[i].at;
            struct mc_rect r = {m.x + (int)(a.x * k), m.y + (int)(a.y * k), (int)(a.w * k),
                                (int)(a.h * k)};
            if (th[i].pic)
                put(th[i].pic, th[i].op, a.w, a.h, r, m);
            else
                fill(&gray, r, m);
        }
        if (font && xft_back) {
            char name[24];
            int len = snprintf(name, sizeof name, "Desktop %d", d + 1);
            XGlyphInfo ink;
            XftTextExtentsUtf8(dpy, font, (const FcChar8 *)name, len, &ink);
            XftDrawStringUtf8(xft_back, &white, font, m.x + (m.w - ink.xOff) / 2,
                              m.y + m.h + label_gap * 3 / 4 + font->ascent, (const FcChar8 *)name,
                              len);
        }
    }
}

/* The blue ring around window i and its title under it, drawn straight on
   the screen. The back pixmap never has them, so taking them away again is
   one copy (set_hover). */
static void decorate(int i)
{
    struct mc_rect s = th[i].show;
    int r = ring_w;
    XRenderFillRectangle(dpy, PictOpSrc, win_pic, &blue, s.x - r, s.y - r, s.w + 2 * r, r);
    XRenderFillRectangle(dpy, PictOpSrc, win_pic, &blue, s.x - r, s.y + s.h, s.w + 2 * r, r);
    XRenderFillRectangle(dpy, PictOpSrc, win_pic, &blue, s.x - r, s.y, r, s.h);
    XRenderFillRectangle(dpy, PictOpSrc, win_pic, &blue, s.x + s.w, s.y, r, s.h);
    decor = (struct mc_rect){s.x - r, s.y - r, s.w + 2 * r, s.h + 2 * r};
    if (!font || !xft || !th[i].name[0])
        return;
    const char *name = th[i].name;
    int len = (int)strlen(name);
    XGlyphInfo ink;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)name, len, &ink);
    int pad = font->height / 2;
    int tw = ink.xOff < label_max ? ink.xOff : label_max;
    int bw = tw + 2 * pad, bh = font->height + pad;
    int bx = s.x + (s.w - bw) / 2, by = s.y + s.h + r + label_gap;
    bx = bx < 0 ? 0 : bx + bw > sw ? sw - bw : bx;
    by = by + bh > sh ? sh - bh : by;
    XRenderFillRectangle(dpy, PictOpOver, win_pic, &pill, bx, by, bw, bh);
    XRectangle clip = {(short)(bx + pad), (short)by, (unsigned short)tw, (unsigned short)bh};
    XftDrawSetClipRectangles(xft, 0, 0, &clip, 1);
    XftDrawStringUtf8(xft, &white, font, bx + pad, by + pad / 2 + font->ascent,
                      (const FcChar8 *)name, len);
    int x0 = bx < decor.x ? bx : decor.x;
    int x1 = bx + bw > decor.x + decor.w ? bx + bw : decor.x + decor.w;
    decor = (struct mc_rect){x0, decor.y, x1 - x0, by + bh - decor.y};
}

static int is_open(void)
{
    return !dir && p >= 1 && re >= 1 && !dragging;
}

static void set_hover(int i)
{
    if (i == hover)
        return;
    if (hover >= 0 && is_open())
        XCopyArea(dpy, back, win, copy_gc, decor.x, decor.y, decor.w, decor.h, decor.x, decor.y);
    hover = i;
    if (hover >= 0 && is_open())
        decorate(hover);
}

/* Everything at e (0 = the desktop as it is, 1 = Mission Control) except
   window skip: the wallpaper, the taskbar and icons, the dimming, the bar
   dropping in, and every window on its way, into the back pixmap. */
static void draw_scene(double e, int skip)
{
    struct mc_rect screen = {0, 0, sw, sh};
    if (wall)
        put(wall, PictOpSrc, sw, sh, screen, screen);
    else
        fill(&dark, screen, screen);
    for (int i = 0; i < nfx; i++) {
        struct mc_rect r = fx[i].at;
        XRenderComposite(dpy, fx[i].op, fx[i].pic, None, back_pic, 0, 0, 0, 0, r.x, r.y, r.w,
                         r.h);
    }
    XRenderColor dim = {0, 0, 0, (unsigned short)(0x51eb * e)};    /* up to 32% black */
    if (dim.alpha)
        XRenderFillRectangle(dpy, PictOpOver, back_pic, &dim, 0, 0, sw, sh);
    bar_off = lerp_i(-bar_h, 0, e);
    draw_bar(bar_off);
    for (int n = 0; n < nth; n++) {
        int i = n;
        if (pick >= 0)              /* the chosen one last, on top */
            i = n < pick ? n : n == nth - 1 ? pick : n + 1;
        if (i == skip)
            continue;
        struct thumb *t = &th[i];
        t->now = re < 1 ? lerp_r(t->from, t->show, ease(re)) : lerp_r(t->at, t->show, e);
        if (t->pic)
            put(t->pic, t->op, t->at.w, t->at.h, t->now, screen);
        else
            fill(&gray, t->now, screen);
    }
}

/* A blue ring around desktop d's miniature: a window dropped now goes there. */
static void ring_mini(int d)
{
    struct mc_rect screen = {0, 0, sw, sh}, m = {mini_x[d], mini_y + bar_off, mini_w, mini_h};
    int r = ring_w;
    fill(&blue, (struct mc_rect){m.x - r, m.y - r, m.w + 2 * r, r}, screen);
    fill(&blue, (struct mc_rect){m.x - r, m.y + m.h, m.w + 2 * r, r}, screen);
    fill(&blue, (struct mc_rect){m.x - r, m.y, r, m.h}, screen);
    fill(&blue, (struct mc_rect){m.x + m.w, m.y, r, m.h}, screen);
}

/* The dragged window: under the pointer where it was grabbed; over a
   miniature, shrunk to that desktop's size and centered on the pointer. */
static struct mc_rect dragged_rect(void)
{
    struct thumb *t = &th[press];
    if (drop_desk >= 0) {
        int w = t->at.w * mini_w / sw, h = t->at.h * mini_w / sw;
        return (struct mc_rect){drag_x - w / 2, drag_y - h / 2, w, h};
    }
    return (struct mc_rect){t->show.x + drag_x - press_x, t->show.y + drag_y - press_y,
                            t->show.w, t->show.h};
}

/* One frame at e onto the screen. While dragging, everything else is
   drawn once into base, and each frame after is base plus the window. */
static void render(double e)
{
    int drag = press >= 0 && dragging;
    if (drag && base_ok) {
        XCopyArea(dpy, base, back, copy_gc, 0, 0, sw, sh, 0, 0);
    } else {
        draw_scene(e, drag ? press : -1);
        if (drag) {
            if (!base)
                base = XCreatePixmap(dpy, win, sw, sh, depth);
            XCopyArea(dpy, back, base, copy_gc, 0, 0, sw, sh, 0, 0);
            base_ok = 1;
        }
    }
    if (drag) {
        struct mc_rect screen = {0, 0, sw, sh};
        struct thumb *t = &th[press];
        if (drop_desk >= 0)
            ring_mini(drop_desk);
        t->now = dragged_rect();
        if (t->pic)
            put(t->pic, t->op, t->at.w, t->at.h, t->now, screen);
        else
            fill(&gray, t->now, screen);
    }
    XCopyArea(dpy, back, win, copy_gc, 0, 0, sw, sh, 0, 0);
    if (hover >= 0 && is_open())
        decorate(hover);
}

static int hit_window(int x, int y)
{
    for (int i = nth - 1; i >= 0; i--) {
        struct mc_rect s = th[i].now;
        if (x >= s.x && y >= s.y && x < s.x + s.w && y < s.y + s.h)
            return i;
    }
    return -1;
}

static int hit_desk(int x, int y)
{
    for (int d = 0; d < ndesks; d++)
        if (x >= mini_x[d] && x < mini_x[d] + mini_w && y >= mini_y + bar_off &&
            y < mini_y + bar_off + mini_h)
            return d;
    return -1;
}

/* The wallpaper icewmbg published on the root window, as a picture. */
static Picture wallpaper(XRenderPictFormat *fmt)
{
    unsigned long pm = 0;
    if (!prop32(root, atom[A_ROOTPM], &pm, 1) || !pm)
        return None;
    XRenderPictureAttributes pa = {.repeat = RepeatNormal};
    xerr = 0;
    Picture pic = XRenderCreatePicture(dpy, (Pixmap)pm, fmt, CPRepeat, &pa);
    XRenderSetPictureFilter(dpy, pic, FilterBilinear, NULL, 0);
    XSync(dpy, False);
    return xerr ? None : pic;
}

/* myos-menu still holds the pointer until the click on the curl is over
   (the button comes up), so the grabs are tried for a while, as dmenu
   does. Without the pointer grab the clicks still come here: this window
   covers the screen. */
static void grab(void)
{
    const struct timespec ms = {0, 1000000};
    int kb = 0, ptr = 0;
    for (int i = 0; i < 200 && !(kb && ptr); i++) {
        if (!kb)
            kb = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) ==
                 GrabSuccess;
        if (!ptr)
            ptr = XGrabPointer(dpy, win, True,
                               ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                               GrabModeAsync, GrabModeAsync, win, None, CurrentTime) == GrabSuccess;
        if (!(kb && ptr))
            nanosleep(&ms, NULL);
    }
}

/* An EWMH request to IceWM. "2" in one is the source: a pager asked (the
   user chose it), which IceWM always does. */
static void ask_wm(Window w, Atom what, long l0, long l1)
{
    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = what;
    e.xclient.format = 32;
    e.xclient.data.l[0] = l0;
    e.xclient.data.l[1] = l1;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* Windows glide from where they are now to their places (re: 0 .. 1). */
static void glide_from_now(struct timespec *last)
{
    for (int k = 0; k < nth; k++)
        th[k].from = th[k].now;
    re = 0;
    clock_gettime(CLOCK_MONOTONIC, last);
}

/* Window i goes to desktop d: IceWM moves it there, that desktop's
   miniature shows it from now on, and the windows left on this one glide
   to a new layout without it. */
static void move_to_desk(int i, int d, struct timespec *last)
{
    ask_wm(th[i].client, atom[A_DESKTOP], d, 2);
    if (npane < MC_MAX)
        pane[npane++] = (struct pane){d, th[i].at};
    nth--;
    memmove(&th[i], &th[i + 1], (size_t)(nth - i) * sizeof th[0]);
    glide_from_now(last);
    place();
}

/* Back to the desktop: every window glides home from where it is. */
static void start_closing(struct timespec *last)
{
    if (dir < 0)
        return;
    set_hover(-1);
    if (!dir)                       /* (while opening, it just turns round) */
        for (int k = 0; k < nth; k++)
            th[k].show = th[k].now;
    re = 1;
    press = -1;
    dragging = 0;
    drop_desk = -1;
    dir = -1;
    clock_gettime(CLOCK_MONOTONIC, last);
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Until IceWM has done what was asked (the root's property says so), at
   most WAIT_MS: taking this window away then shows the new desktop or the
   window in front, not the screen as it was a moment before. */
static void wait_root(Atom a, unsigned long want)
{
    const struct timespec step = {0, 5000000};
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        unsigned long v = ~0ul;
        if (prop32(root, a, &v, 1) && v == want)
            return;
        nanosleep(&step, NULL);
    } while (ms_since(&t0) < WAIT_MS);
}

int main(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    XSetErrorHandler(on_error);
    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vis);
    if (!fmt)
        return 1;                   /* no XRender */
    XInternAtoms(dpy, (char **)atom_names, NATOMS, False, atom);
    prop32(root, atom[A_CURRENT], &current, 1);
    unsigned long nd = 4;
    prop32(root, atom[A_NDESKS], &nd, 1);
    ndesks = nd < 1 ? 1 : nd > MAX_DESKS ? MAX_DESKS : (int)nd;

    scale = desktop_scale();
    ring_w = 4 * scale / 100;
    label_gap = 8 * scale / 100;
    label_max = 420 * scale / 100;
    char spec[64];
    snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", 14 * scale / 100);
    font = XftFontOpenName(dpy, screen, spec);

    int evb, erb;
    if (XCompositeQueryExtension(dpy, &evb, &erb)) {
        xerr = 0;
        XCompositeRedirectSubwindows(dpy, root, COMPOSITE_REDIRECT_AUTOMATIC);
        XSync(dpy, False);
        composite = !xerr;
    }
    collect();
    collect_fixed();
    bar_geometry();
    place();
    wall = wallpaper(fmt);

    /* No background: until the first frame is drawn, the screen shows what
       was there, not a black flash. */
    XSetWindowAttributes attr = {
        .override_redirect = True,
        .event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                      ExposureMask,
    };
    win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWOverrideRedirect | CWEventMask, &attr);
    depth = DefaultDepth(dpy, screen);
    back = XCreatePixmap(dpy, win, sw, sh, depth);
    back_pic = XRenderCreatePicture(dpy, back, fmt, 0, NULL);
    win_pic = XRenderCreatePicture(dpy, win, fmt, 0, NULL);
    XGCValues gv = {.graphics_exposures = False};
    copy_gc = XCreateGC(dpy, win, GCGraphicsExposures, &gv);
    xft = XftDrawCreate(dpy, win, vis, cmap);
    xft_back = XftDrawCreate(dpy, back, vis, cmap);
    XRenderColor wc = {0xffff, 0xffff, 0xffff, 0xffff};
    XftColorAllocValue(dpy, vis, cmap, &wc, &white);

    int still = reduce_motion();
    dir = 1;
    XMapRaised(dpy, win);
    grab();

    static const int again_ms[] = {300, 700};
    unsigned again = 0;
    struct timespec t0, last;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    last = t0;
    int fd = ConnectionNumber(dpy), to_desk = -1, dirty = 0;
    Time when = CurrentTime;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose && is_open()) {
                XExposeEvent *x = &e.xexpose;
                XCopyArea(dpy, back, win, copy_gc, x->x, x->y, x->width, x->height, x->x, x->y);
                if (!x->count && hover >= 0)
                    decorate(hover);
            } else if (e.type == MotionNotify) {
                int x = e.xmotion.x_root, y = e.xmotion.y_root;
                if (press < 0) {
                    set_hover(dir >= 0 ? hit_window(x, y) : -1);
                    continue;
                }
                if (!dragging && abs(x - press_x) + abs(y - press_y) > DRAG_START) {
                    set_hover(-1);
                    dragging = 1;
                    base_ok = 0;
                }
                if (dragging) {     /* drawn once the events waiting are read */
                    drag_x = x;
                    drag_y = y;
                    drop_desk = hit_desk(x, y);
                    if (drop_desk == (int)current)  /* it is there already */
                        drop_desk = -1;
                    dirty = 1;
                }
            } else if (e.type == ButtonRelease && e.xbutton.button == Button1 && press >= 0) {
                when = e.xbutton.time;
                int i = press, d = drop_desk;
                if (!dragging) {    /* a click: go to that window */
                    pick = i;
                    start_closing(&last);
                    continue;
                }
                press = -1;
                dragging = 0;
                drop_desk = -1;
                if (d >= 0)
                    move_to_desk(i, d, &last);
                else
                    glide_from_now(&last);  /* only the dropped one moves: back home */
                hover = -1;
            } else if (e.type == KeyPress && (XLookupKeysym(&e.xkey, 0) == XK_Escape ||
                                              e.xkey.keycode == SWIPE_DOWN)) {
                start_closing(&last);
            } else if (e.type == ButtonPress && e.xbutton.button == Button1 && dir >= 0 &&
                       press < 0) {
                when = e.xbutton.time;
                int x = e.xbutton.x_root, y = e.xbutton.y_root;
                int d = hit_desk(x, y);
                if (d >= 0 && d != (int)current) {
                    to_desk = d;
                    goto out;
                }
                int w = d < 0 ? hit_window(x, y) : -1;
                if (w >= 0 && !dir && p >= 1) {     /* a click or a drag: the release says */
                    re = 1;                         /* (a glide still going ends here) */
                    press = w;
                    press_x = drag_x = x;
                    press_y = drag_y = y;
                    continue;
                }
                pick = w;
                start_closing(&last);
            }
        }
        struct timeval tv, *wait = NULL;
        if (dir || re < 1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - last.tv_sec) * 1000.0 + (now.tv_nsec - last.tv_nsec) / 1e6;
            last = now;
            if (dir) {
                p += still ? dir : dir * dt / MC_MS;
                p = p < 0 ? 0 : p > 1 ? 1 : p;
            }
            if (re < 1) {
                re = still || re + dt / MC_MS >= 1 ? 1 : re + dt / MC_MS;
                /* Focus left the moved window, and IceWM may have raised
                   the one it chose above this. */
                if (re >= 1)
                    XRaiseWindow(dpy, win);
            }
            /* Opening lands softly on 1, closing on 0. */
            render(dir > 0 ? ease(p) : dir < 0 ? 1 - ease(1 - p) : 1);
            dirty = 0;
            XSync(dpy, False);      /* the server has drawn it: pace by that */
            if (dir < 0 && p <= 0)
                goto out;
            if (dir > 0 && p >= 1)
                dir = 0;
            if (hover >= 0 && is_open())
                decorate(hover);
            long spent = ms_since(&now);
            tv = (struct timeval){0, (spent < FRAME_MS ? FRAME_MS - spent : 0) * 1000};
            wait = &tv;
        } else if (dirty) {
            render(1);
            dirty = 0;
            XSync(dpy, False);
            continue;
        } else if (again < sizeof again_ms / sizeof again_ms[0]) {
            long left = again_ms[again] - ms_since(&t0);
            if (left <= 0) {
                render(1);
                again++;
                continue;
            }
            tv = (struct timeval){0, left * 1000};
            wait = &tv;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        XFlush(dpy);                /* select does not send what is waiting to go out */
        select(fd + 1, &fds, NULL, NULL, wait);
    }
out:
    if (to_desk >= 0) {
        ask_wm(root, atom[A_CURRENT], to_desk, (long)when);
        wait_root(atom[A_CURRENT], (unsigned long)to_desk);
    } else if (pick >= 0) {
        ask_wm(th[pick].client, atom[A_ACTIVE], 2, (long)when);
        wait_root(atom[A_ACTIVE], th[pick].client);
    }
    /* Closing the display frees every pixmap and picture made here and
       ends the redirection: the windows go back to being drawn straight
       to the screen. */
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
