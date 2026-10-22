/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-menu — the Start menu of the MyOS desktop (the Golden Hour theme).
 *
 * IceWM draws the taskbar and the window buttons, but its own Start menu
 * is a plain list and its clock is a flat rectangle the width of the time.
 * The theme's menu has two columns under a header with your name, and Sleep /
 * Log off / Turn off along the bottom, so this program draws it, and the Start
 * button too. It also draws the tray at the right of the bar: the mockup's
 * color, the divider, the network and volume icons, the battery (on a
 * machine that has one: /sys/class/power_supply), and the time. How it
 * hooks in, without IceWM knowing:
 *
 *   - IceWM's Start button only holds the place (the theme gives it a
 *     plain piece of bar as its picture). We put a window of our own inside
 *     it, the way a system-tray icon sits inside a panel, and draw the real
 *     button there. Being IceWM's child, it moves, stacks and hides with the
 *     taskbar, and its clicks come to us, not to IceWM;
 *   - Ctrl+Esc and the Windows key are grabbed on the root window (IceWM's
 *     own KeySysMenu and Win95Keys are switched off in its preferences).
 *
 * It also draws the theme's hot corner, the curl at the top right of the
 * desktop (see "the hot corner" below). A click there, or pushing the
 * mouse into that corner over any window, starts myos-mission, which
 * spreads this desktop's windows out. (Not F3, as on
 * the Mac: a key grabbed here is taken from every program, and F3 is
 * WritePerfect's Help.) Three fingers on the touchpad do the Mac's
 * swipes: up starts myos-mission, left and right change desktop (see
 * swiped()). It is also the tray's notification
 * area: the Wi-Fi icon (myos-wifi) docks there, where the mockup has its
 * network icon, and other programs' icons (IBus's) open above the tray
 * under the mouse (see "the notification area" below).
 *
 * Everything you see is drawn at build time by live/theme/make-live-theme.py
 * from the theme's design, into /usr/share/myos/menu/<scale>: the whole
 * menu, each entry as it looks under the mouse, the Start button in its
 * three states, the Run box, and the tray. menu.conf there says where each
 * entry is and what it runs, and where the clock sits in the tray. This
 * program adds only your name, the entry under the mouse, what you type,
 * and the time. All Programs is the exception: its row is drawn with the
 * menu, and the folders that open from it are drawn here, from the Programs
 * directories (see programs.c). The scale is the desktop's (100 to 200%,
 * see myos-xsettings); when it changes, the program starts itself over.
 *
 * Cost: opening the menu reads the screen under it once (XGetImage, so the
 * round corners and the shadow blend into whatever is there) and sends it
 * back once (XPutImage). Moving over the entries only copies small
 * pixmaps that were made at start-up.
 *
 * Started from /etc/icewm/startup, which may run before IceWM has made its
 * taskbar, so the Start button is looked for until it turns up. Runs as
 * the logged-in user.
 */
#include <Imlib2.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "xpic.h"
#include "programs.h"
#include "power_supply.h"

/* Pushing into the hot corner uses XFixes 5's pointer barriers and XInput
   2.3's barrier events. The sysroot has their libraries (GTK brings them)
   and their protocol headers, but not the libraries' headers: what is used
   here, as in Xfixes.h and XInput2.h. */
#include <X11/extensions/XI2.h>
#include <X11/extensions/xfixeswire.h>
typedef XID PointerBarrier;
Bool XFixesQueryExtension(Display *, int *, int *);
Status XFixesQueryVersion(Display *, int *, int *);
PointerBarrier XFixesCreatePointerBarrier(Display *, Window, int, int, int, int, int, int, int *);
typedef struct {
    int deviceid;
    int mask_len;
    unsigned char *mask;
} XIEventMask;
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    int extension;
    int evtype;
    Time time;
    int deviceid;
    int sourceid;
    Window event;
    Window root;
    double root_x;
    double root_y;
    double dx;
    double dy;
    int dtime;
    int flags;
    PointerBarrier barrier;
    unsigned int eventid;
} XIBarrierEvent;
Status XIQueryVersion(Display *, int *, int *);
int XISelectEvents(Display *, Window, XIEventMask *, int);

#define MAX_ITEMS 16

struct pic {                        /* a picture, 0xAARRGGBB, not premultiplied */
    uint32_t *px;
    int w, h;
};

struct rect {
    int x, y, w, h;
};

struct item {
    struct rect r;                  /* where it is in menu.png */
    char cmd[200];                  /* what it runs; "@run" opens the Run box */
    Pixmap lit;                     /* how it looks under the mouse */
};

enum { START_NORMAL, START_HOVER, START_PRESSED };

static Display *dpy;
static Window root;
static Window start_win;            /* ours, inside IceWM's Start button; 0 until found */
static Window tray_win;             /* ours, over the right end of the taskbar; 0 until found */
static Window tray_frame;           /* IceWM's taskbar window, the tray's parent */
static Pixmap tray_bg;              /* the tray as it looks this minute */
static Visual *vis;
static Colormap cmap;
static int screen, depth, sw, sh;
static XftFont *f_name, *f_type, *f_clock;
static XftFont *f_prog;             /* All Programs: half again the Run box's type */

static struct pic menu_pic, run_pic, tray_pic;
static struct pic corner_pic[2];    /* the hot corner's curl: resting, under the mouse */
static Window corner_win;
static Pixmap corner_pm[2];
static Atom a_rootpm;               /* _XROOTPMAP_ID: the wallpaper, as icewmbg drew it */
static Pixmap start_pm[3];
static int start_w, start_h;
static struct item items[MAX_ITEMS];
static int nitems;
static int name_x, name_y, name_size, type_size = 13;
static int clock_x, clock_y, clock_size;   /* where the time sits in the tray */
static int slot_x, slot_y, slot_size;      /* where a docked icon sits in the tray */
static int vol_x, vol_y, vol_size;         /* the speaker in the tray (0 size: none) */
static int bat_x, bat_y, bat_size;         /* the battery in the tray (0 size: none) */
static struct pic bat_pic, bolt_pic;       /* its outline; the bolt while charging */
static struct rect bat_fill;               /* the inside, from the icon's top left */
static struct power_supply bat;            /* as last read; bat.present 0: not drawn */
static time_t bat_read;                    /* when */
static Atom a_tray_sel, a_tray_opcode, a_xembed;   /* (the notification area) */
static Window tray_owner;           /* holds the tray selection; dock requests come to it */
#define MAX_DOCK 8
struct docked {
    Window w;                       /* the program's icon window */
    Window in;                      /* where it sits now: tray_win or panel_inner (0 not yet) */
    int x, y;
    int rank;                       /* its order in the tray (rank_of) */
};
static struct docked docked[MAX_DOCK];     /* the Wi-Fi icon first, then as they came */
static int ndocked;
static Window panel_win;            /* the icons that don't fit, above the tray, under the mouse */
static Window panel_inner;          /* the whole panel, inside panel_win (see "grows" below) */
static Pixmap panel_bg;
static int panel_rows;
static int panel_h;                 /* how much of it shows now; 0 closed */
static int anim_from, anim_to, anim_ms;    /* a grow or shrink under way (anim_ms 0: none) */
static long anim_start;
static Atom a_tray_top;             /* _MYOS_TRAY_TOP: where the tray's top is now (tray_follow.h) */
static long tray_top_said = -1;
static Window bar_window;           /* the taskbar's top-level window, the one the tray sits on */
static void redraw_icons(void);
static void lay_out_icons(void);
static void undock(Window w);
static void close_panel(void);
static int tray_seam(void);
static int panel_full(void);
static time_t tray_minute;                 /* the minute currently drawn; 0 none */
static struct rect run_box, run_ok, run_cancel, run_close;
static char user[64];
static char dir[48];                /* this scale's pictures: /usr/share/myos/menu/150/ */
static int scale;                   /* percent */
static Atom a_scale, a_scale_auto;  /* set on the root window by myos-xsettings */

static void die(const char *what)
{
    fprintf(stderr, "myos-menu: %s\n", what);
    exit(1);
}

/* Nothing this program does to the server is worth dying over (a window
   that went away between two requests, say), so errors are only noted. */
static int x_error(Display *d, XErrorEvent *e)
{
    char msg[80];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    fprintf(stderr, "myos-menu: X error: %s (request %d)\n", msg, e->request_code);
    return 0;
}

static int inside(const struct rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* --- pictures -------------------------------------------------------------------- */

/* Imlib2 only decodes the PNG; we keep our own copy of the pixels. */
static void load(struct pic *p, const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "%s%s", dir, name);
    Imlib_Image im = imlib_load_image(path);
    if (!im)
        die(path);
    imlib_context_set_image(im);
    p->w = imlib_image_get_width();
    p->h = imlib_image_get_height();
    p->px = malloc((size_t)p->w * p->h * 4);
    if (!p->px)
        die("out of memory");
    memcpy(p->px, imlib_image_get_data_for_reading_only(), (size_t)p->w * p->h * 4);
    imlib_free_image();
}

static Pixmap pixmap_from(const uint32_t *px, int w, int h)
{
    Pixmap p = xpic_pixmap(dpy, vis, depth, root, px, w, h);
    if (!p)
        die("out of memory");
    return p;
}

/* One channel of a screen pixel, as 0..255 (for screens that aren't 888). */
static int unchannel(unsigned long pixel, unsigned long mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    for (unsigned long m = mask; !(m & 1); m >>= 1)
        shift++;
    unsigned long v = (pixel & mask) >> shift, max = mask >> shift;
    return (int)(v * 255 / max);
}

/* A rectangle of the screen (or of another drawable), in a w x h buffer. */
static uint32_t *grab(Drawable d, int x, int y, int w, int h)
{
    uint32_t *px = calloc((size_t)w * h, 4);
    XImage *xi = XGetImage(dpy, d, x, y, w, h, AllPlanes, ZPixmap);
    if (!px)
        die("out of memory");
    if (!xi)
        return px;                  /* black is a fine fallback */
    int direct = xpic_direct(xi, vis);
    for (int j = 0; j < h; j++) {
        uint32_t *row = px + (size_t)j * w;
        if (direct) {
            memcpy(row, xi->data + (size_t)j * xi->bytes_per_line, (size_t)w * 4);
            continue;
        }
        for (int i = 0; i < w; i++) {
            unsigned long v = XGetPixel(xi, i, j);
            row[i] = (uint32_t)unchannel(v, vis->red_mask) << 16 |
                     (uint32_t)unchannel(v, vis->green_mask) << 8 |
                     (uint32_t)unchannel(v, vis->blue_mask);
        }
    }
    XDestroyImage(xi);
    return px;
}

/* Lay picture p over the buffer at (ox, oy), honouring its transparency. */
static void over(uint32_t *dst, int dw, int dh, const struct pic *p, int ox, int oy)
{
    for (int j = 0; j < p->h && oy + j < dh; j++)
        for (int i = 0; i < p->w && ox + i < dw; i++) {
            uint32_t s = p->px[(size_t)j * p->w + i], a = s >> 24;
            uint32_t *d = &dst[(size_t)(oy + j) * dw + ox + i];
            if (a == 255)
                *d = s;
            else if (a) {
                uint32_t r = ((s >> 16 & 0xff) * a + (*d >> 16 & 0xff) * (255 - a)) / 255;
                uint32_t g = ((s >> 8 & 0xff) * a + (*d >> 8 & 0xff) * (255 - a)) / 255;
                uint32_t b = ((s & 0xff) * a + (*d & 0xff) * (255 - a)) / 255;
                *d = r << 16 | g << 8 | b;
            }
        }
}

/* --- text ------------------------------------------------------------------------ */

static void text(Drawable d, XftFont *f, uint32_t rgb, int alpha, int x, int y, const char *s)
{
    XftDraw *draw = XftDrawCreate(dpy, d, vis, cmap);
    XRenderColor rc = { .red = (rgb >> 16 & 0xff) * 257, .green = (rgb >> 8 & 0xff) * 257,
                        .blue = (rgb & 0xff) * 257, .alpha = alpha * 257 };
    XftColor col;
    XftColorAllocValue(dpy, vis, cmap, &rc, &col);
    XftDrawStringUtf8(draw, &col, f, x, y, (const FcChar8 *)s, strlen(s));
    XftColorFree(dpy, vis, cmap, &col);
    XftDrawDestroy(draw);
}

static int text_width(XftFont *f, const char *s)
{
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &ext);
    return ext.xOff;
}

/* --- setting up ------------------------------------------------------------------ */

static void read_conf(void)
{
    char path[64];
    snprintf(path, sizeof path, "%smenu.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        die(path);
    char line[300], key[16], name[64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        struct rect r;
        int n = 0;
        /* "word rest...": in a scanf format a space also matches no space,
           so the first word is compared whole, not matched as a prefix. */
        if (sscanf(line, "%15s %n", key, &n) != 1 || key[0] == '#')
            continue;
        const char *rest = line + n;
        int nr = sscanf(rest, "%d %d %d %d", &r.x, &r.y, &r.w, &r.h);
        if (!strcmp(key, "menu") && sscanf(rest, "%63s", name) == 1)
            load(&menu_pic, name);
        else if (!strcmp(key, "corner")) {
            char n0[64], n1[64];
            if (sscanf(rest, "%63s %63s", n0, n1) == 2)
                load(&corner_pic[0], n0), load(&corner_pic[1], n1);
        } else if (!strcmp(key, "start")) {
            char names[3][64];
            if (sscanf(rest, "%63s %63s %63s", names[0], names[1], names[2]) != 3)
                die("menu.conf: start needs three pictures");
            for (int i = 0; i < 3; i++) {
                struct pic p;
                load(&p, names[i]);
                start_pm[i] = pixmap_from(p.px, p.w, p.h);
                start_w = p.w, start_h = p.h;
                free(p.px);
            }
        }
        else if (!strcmp(key, "run") && sscanf(rest, "%63s", name) == 1)
            load(&run_pic, name);
        else if (!strcmp(key, "tray") && sscanf(rest, "%63s", name) == 1)
            load(&tray_pic, name);
        else if (!strcmp(key, "clock")) {
            int x, y, sz;
            if (sscanf(rest, "%d %d %d", &x, &y, &sz) == 3)
                clock_x = x, clock_y = y, clock_size = sz;
        }
        else if (!strcmp(key, "icon")) {
            int x, y, sz;
            if (sscanf(rest, "%d %d %d", &x, &y, &sz) == 3)
                slot_x = x, slot_y = y, slot_size = sz;
        }
        else if (!strcmp(key, "volume")) {
            int x, y, sz;
            if (sscanf(rest, "%d %d %d", &x, &y, &sz) == 3)
                vol_x = x, vol_y = y, vol_size = sz;
        }
        else if (!strcmp(key, "battery")) {
            int x, y, sz;
            struct rect f;
            char bolt[64];
            if (sscanf(rest, "%d %d %d %63s %63s %d %d %d %d", &x, &y, &sz, name, bolt,
                       &f.x, &f.y, &f.w, &f.h) == 9) {
                load(&bat_pic, name);
                load(&bolt_pic, bolt);
                bat_x = x, bat_y = y, bat_size = sz, bat_fill = f;
            }
        }
        else if (!strcmp(key, "name") && nr >= 3)
            name_x = r.x, name_y = r.y, name_size = r.w;
        else if (!strcmp(key, "typesize") && nr >= 1)
            type_size = r.x;
        else if (!strcmp(key, "runbox") && nr == 4)
            run_box = r;
        else if (!strcmp(key, "runok") && nr == 4)
            run_ok = r;
        else if (!strcmp(key, "runcancel") && nr == 4)
            run_cancel = r;
        else if (!strcmp(key, "runclose") && nr == 4)
            run_close = r;
        else if (!strcmp(key, "item") && nitems < MAX_ITEMS &&
                 sscanf(rest, "%d %d %d %d %63s %n", &r.x, &r.y, &r.w, &r.h, name, &n) == 5) {
            struct item *it = &items[nitems++];
            it->r = r;
            snprintf(it->cmd, sizeof it->cmd, "%s", rest + n);
            struct pic lit;
            load(&lit, name);
            it->lit = pixmap_from(lit.px, lit.w, lit.h);
            free(lit.px);
        }
    }
    fclose(f);
    if (!menu_pic.px || !start_pm[0] || !run_pic.px || !nitems)
        die("menu.conf is incomplete");
}

/* The name in the header: the user's full name if the account has one. */
static void read_user(void)
{
    struct passwd *pw = getpwuid(getuid());
    const char *s = pw ? pw->pw_name : "user";
    if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',')
        s = pw->pw_gecos;
    snprintf(user, sizeof user, "%s", s);
    user[strcspn(user, ",")] = 0;   /* gecos is "Full Name,room,phone,..." */
}

/* The key combinations that open the menu, whatever Num Lock and Caps Lock say. */
static void grab_key(KeySym sym, unsigned mods)
{
    KeyCode kc = XKeysymToKeycode(dpy, sym);
    if (!kc)
        return;
    unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (int i = 0; i < 4; i++)
        XGrabKey(dpy, kc, mods | locks[i], root, True, GrabModeAsync, GrabModeAsync);
}

static int opens_menu(XKeyEvent *e)
{
    KeySym k = XLookupKeysym(e, 0);
    return k == XK_Super_L || k == XK_Super_R || (k == XK_Escape && (e->state & ControlMask));
}

/* An override-redirect window: the window manager leaves it alone. */
static Window popup(int x, int y, int w, int h, Pixmap bg)
{
    XSetWindowAttributes a = { .override_redirect = True, .background_pixmap = bg,
                               .event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                             KeyPressMask | ExposureMask | StructureNotifyMask };
    Window w_ = XCreateWindow(dpy, root, x, y, w, h, 0, depth, InputOutput, vis,
                              CWOverrideRedirect | CWBackPixmap | CWEventMask, &a);
    XMapRaised(dpy, w_);
    XEvent ev;
    do
        XWindowEvent(dpy, w_, StructureNotifyMask, &ev);
    while (ev.type != MapNotify);
    return w_;
}

/* Take the mouse and the keyboard while a popup is open. Someone else (an
   IceWM menu, a dragged window) may hold them for a moment, so retry briefly. */
/* owner_events: the All Programs flyouts are our windows too, and they
   need the motion and clicks while this grab is held. The Run box is the
   only window up, so it keeps the events itself. */
static int grab_input(Window w, int owner_events)
{
    struct timespec t = { 0, 10 * 1000 * 1000 };
    Bool own = owner_events ? True : False;
    for (int i = 0; i < 50; i++) {
        if (XGrabPointer(dpy, w, own, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) {
            if (XGrabKeyboard(dpy, w, own, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
                return 1;
            XUngrabPointer(dpy, CurrentTime);
        }
        nanosleep(&t, NULL);
    }
    return 0;
}

static void close_popup(Window w, Pixmap bg)
{
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XDestroyWindow(dpy, w);
    XFreePixmap(dpy, bg);
    XFlush(dpy);
}

/* The next event. If IceWM's taskbar went away (it restarts, say), our
   Start button and tray went with it; the main loop then looks for the new ones. */
static void next_event(XEvent *ev)
{
    XNextEvent(dpy, ev);
    if (ev->type != DestroyNotify)
        return;
    if (ev->xdestroywindow.window == start_win)
        start_win = 0;
    undock(ev->xdestroywindow.window);     /* if it was an icon: its program quit */
    if (ev->xdestroywindow.window == tray_win)
        tray_win = 0;
    /* The tray is ours, on the root window, so it outlives the bar: it is
       only hidden until the new bar turns up. */
    if (ev->xdestroywindow.window == tray_frame || ev->xdestroywindow.window == bar_window) {
        tray_frame = bar_window = 0;
        if (tray_win)
            XUnmapWindow(dpy, tray_win);
        close_panel();
    }
}

/* --- doing things ---------------------------------------------------------------- */

/* Start a command, detached: the grandchild runs it and is adopted by init,
   so nothing waits on us and we never collect zombies. */
static void launch(const char *cmd)
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

/* The speaker in the tray (myos-volume does the work): a click opens the
   volume pop-up above it, the wheel turns it up or down a step, a middle
   click mutes. The picture is the theme's; the hit area is a little wider
   than the icon, as a finger-sized target. */
static void speaker_click(const XButtonEvent *b)
{
    int pad = vol_size / 3;
    if (b->x < vol_x - pad || b->x >= vol_x + vol_size + pad)
        return;                     /* elsewhere in the tray: ignored, as before */
    char cmd[96];
    switch (b->button) {
    case Button1:
        /* Its bottom centre above the icon: the icon's middle, the tray's
           top (the panel's, while the tray is grown). */
        snprintf(cmd, sizeof cmd, "exec myos-volume --at %d %d",
                 b->x_root - b->x + vol_x + vol_size / 2, b->y_root - b->y - panel_h);
        break;
    case Button2:
        snprintf(cmd, sizeof cmd, "exec myos-volume --mute");
        break;
    case Button4:
        snprintf(cmd, sizeof cmd, "exec myos-volume --up");
        break;
    case Button5:
        snprintf(cmd, sizeof cmd, "exec myos-volume --down");
        break;
    default:
        return;
    }
    launch(cmd);
}

/* The battery in the tray (myos-battery does the work): a click opens its
   pop-up, the charge, the time left, health and the power-saving slider. */
static int battery_click(const XButtonEvent *b)
{
    int pad = bat_size / 3, left = bat_x - pad;
    if (vol_size && left < (vol_x + vol_size + bat_x) / 2)
        left = (vol_x + vol_size + bat_x) / 2;      /* the speaker has the other half */
    if (!bat.present || b->button != Button1 || b->x < left || b->x >= bat_x + bat_size + pad)
        return 0;
    char cmd[96];
    snprintf(cmd, sizeof cmd, "exec myos-battery --at %d %d",
             b->x_root - b->x + bat_x + bat_size / 2, b->y_root - b->y - panel_h);
    launch(cmd);
    return 1;
}

/* Run...: type a command, Enter (or OK) starts it. */
static void run_dialog(void)
{
    int w = run_pic.w, h = run_pic.h, x = (sw - w) / 2, y = (sh - h) / 3;
    uint32_t *px = grab(root, x, y, w, h);
    over(px, w, h, &run_pic, 0, 0);
    Pixmap bg = pixmap_from(px, w, h);
    free(px);
    Window win = popup(x, y, w, h, bg);
    if (!grab_input(win, 0)) {
        close_popup(win, bg);
        return;
    }
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, xpic_channel(0x1d, vis->red_mask) | xpic_channel(0x24, vis->green_mask) |
                                xpic_channel(0x30, vis->blue_mask));
    char cmd[200] = "";
    size_t len = 0;
    int go = -1;                    /* -1 still typing, 0 cancel, 1 run */
    while (go < 0) {
        /* The box: what was typed, ending at the caret; the start scrolls
           out of sight on the left when it gets long. */
        struct rect b = run_box;
        int room = b.w - 16, tw = text_width(f_type, cmd);
        int tx = b.x + 8 - (tw > room ? tw - room : 0);
        XClearArea(dpy, win, b.x + 2, b.y + 2, b.w - 4, b.h - 4, False);
        XftDraw *draw = XftDrawCreate(dpy, win, vis, cmap);
        XRectangle clip = { b.x + 4, b.y + 2, b.w - 8, b.h - 4 };
        XftDrawSetClipRectangles(draw, 0, 0, &clip, 1);
        XRenderColor rc = { 0x1d1d, 0x2424, 0x3030, 0xffff };
        XftColor col;
        XftColorAllocValue(dpy, vis, cmap, &rc, &col);
        XftDrawStringUtf8(draw, &col, f_type, tx, b.y + b.h / 2 + f_type->ascent / 2 - 1,
                          (const FcChar8 *)cmd, len);
        XftColorFree(dpy, vis, cmap, &col);
        XftDrawDestroy(draw);
        XFillRectangle(dpy, win, gc, tx + tw + 1, b.y + 5, 1, b.h - 10);

        XEvent ev;
        next_event(&ev);
        if (ev.type == KeyPress) {
            char buf[16];
            KeySym k;
            int n = XLookupString(&ev.xkey, buf, sizeof buf, &k, NULL);
            if (k == XK_Return || k == XK_KP_Enter)
                go = 1;
            else if (k == XK_Escape)
                go = 0;
            else if (k == XK_BackSpace) {
                while (len && (cmd[len - 1] & 0xc0) == 0x80)    /* a whole UTF-8 letter */
                    len--;
                if (len)
                    len--;
                cmd[len] = 0;
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f &&
                       len + n < sizeof cmd) {
                memcpy(cmd + len, buf, n);
                cmd[len += n] = 0;
            }
        } else if (ev.type == ButtonPress) {
            int bx = ev.xbutton.x, by = ev.xbutton.y;
            if (bx < 0 || by < 0 || bx >= w || by >= h)
                go = 0;             /* a click somewhere else */
        } else if (ev.type == ButtonRelease) {
            int bx = ev.xbutton.x, by = ev.xbutton.y;
            if (inside(&run_ok, bx, by))
                go = 1;
            else if (inside(&run_cancel, bx, by) || inside(&run_close, bx, by))
                go = 0;
        }
    }
    XFreeGC(dpy, gc);
    close_popup(win, bg);
    if (go == 1 && len)
        launch(cmd);
}

/* --- the menu -------------------------------------------------------------------- */

static int item_at(int x, int y)
{
    for (int i = 0; i < nitems; i++)
        if (inside(&items[i].r, x, y))
            return i;
    return -1;
}

/* Light entry `now` instead of `was` (either may be -1, none). */
static void light(Window win, GC gc, int was, int now)
{
    if (was >= 0) {
        struct rect *r = &items[was].r;
        XClearArea(dpy, win, r->x, r->y, r->w, r->h, False);
    }
    if (now >= 0) {
        struct rect *r = &items[now].r;
        XCopyArea(dpy, items[now].lit, win, gc, 0, 0, r->w, r->h, r->x, r->y);
    }
}

/* --- the Start button ------------------------------------------------------------ */

static void show_start(int state)
{
    if (!start_win)
        return;
    XSetWindowBackgroundPixmap(dpy, start_win, start_pm[state]);
    XClearWindow(dpy, start_win);
}

/* A window by its name (IceWM names its Start button "TaskBarMenu"). The
   taskbar sits a few levels down: root, IceWM's frame, the bar, the button. */
static Window find_named(Window w, const char *name, int levels)
{
    char *n = NULL;
    if (XFetchName(dpy, w, &n) && n) {
        int same = !strcmp(n, name);
        XFree(n);
        if (same)
            return w;
    }
    Window r, parent, *kids = NULL, found = 0;
    unsigned nkids = 0;
    if (levels == 0 || !XQueryTree(dpy, w, &r, &parent, &kids, &nkids))
        return 0;
    for (unsigned i = 0; i < nkids && !found; i++)
        found = find_named(kids[i], name, levels - 1);
    if (kids)
        XFree(kids);
    return found;
}

/* Put our Start button inside IceWM's, once IceWM has made it. */
static void attach_start(void)
{
    Window b = find_named(root, "TaskBarMenu", 6);
    if (!b)
        return;
    XSetWindowAttributes a = { .background_pixmap = start_pm[START_NORMAL],
                               .event_mask = ButtonPressMask | EnterWindowMask | LeaveWindowMask |
                                             StructureNotifyMask };
    start_win = XCreateWindow(dpy, b, 0, 0, start_w, start_h, 0, depth, InputOutput, vis,
                              CWBackPixmap | CWEventMask, &a);
    XMapWindow(dpy, start_win);
}

/* The tray, drawn over the right end of IceWM's taskbar. The picture is the
   mockup's color, divider and icons; the time is drawn on a copy of it, so
   the picture itself stays clean and the next minute starts from it again.
   "%I" writes a leading zero (musl has no "%l"); the mockup's clock doesn't
   have one. */
static void paint_tray(void)
{
    if (!tray_pic.px)
        return;
    Pixmap pm;
    if (bat.present && bat_pic.px) {
        /* The battery: its outline, and the inside filled to the charge
           left, green on the charger, red when nearly empty; a lightning
           bolt over it while charging. */
        size_t n = (size_t)tray_pic.w * tray_pic.h;
        uint32_t *px = malloc(n * 4);
        if (!px)
            die("out of memory");
        memcpy(px, tray_pic.px, n * 4);
        over(px, tray_pic.w, tray_pic.h, &bat_pic, bat_x, bat_y);
        int pct = bat.percent < 0 ? 0 : bat.percent;
        uint32_t color = bat.charging || bat.full || bat.on_ac ? 0x7ee07e :
                         pct <= 10 ? 0xff5a4a : 0xffffff;
        int w = (bat_fill.w * pct + 50) / 100;
        if (pct > 0 && w < 1)
            w = 1;                  /* something, until it is really empty */
        for (int j = 0; j < bat_fill.h; j++) {
            int y = bat_y + bat_fill.y + j;
            for (int i = 0; i < w && y < tray_pic.h; i++) {
                int x = bat_x + bat_fill.x + i;
                if (x < tray_pic.w)
                    px[(size_t)y * tray_pic.w + x] = color;
            }
        }
        if (bat.charging)
            over(px, tray_pic.w, tray_pic.h, &bolt_pic, bat_x, bat_y);
        pm = pixmap_from(px, tray_pic.w, tray_pic.h);
        free(px);
    } else
        pm = pixmap_from(tray_pic.px, tray_pic.w, tray_pic.h);
    if (f_clock) {
        char buf[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (strftime(buf, sizeof buf, "%I:%M %p", &tm)) {
            if (buf[0] == '0')
                memmove(buf, buf + 1, strlen(buf));
            text(pm, f_clock, 0xffffff, 255, clock_x - text_width(f_clock, buf) / 2, clock_y, buf);
            tray_minute = now / 60;
        }
    }
    if (tray_win) {
        XSetWindowBackgroundPixmap(dpy, tray_win, pm);
        XClearWindow(dpy, tray_win);
        lay_out_icons();            /* the battery came or went: its place changes hands */
        redraw_icons();
        XFlush(dpy);
    }
    if (tray_bg)
        XFreePixmap(dpy, tray_bg);
    tray_bg = pm;
}

/* --- the notification area -------------------------------------------------------- */

/* Programs show icons in the tray the way they do on every Linux desktop,
   freedesktop.org's "system tray" protocol: the tray owns the selection
   _NET_SYSTEM_TRAY_S0 and says so; a program asks to dock with a message
   naming its icon's window, and the tray takes that window in as a child
   (XEmbed). The tray is four places wide: an icon where the mockup draws
   its network icon (the Wi-Fi icon gets it, whenever it comes), the
   speaker, the battery, the time. A machine without a battery lends its
   place to a second icon. The rest (the language's, IBus: EN, 中) wait in
   a panel that grows up out of the tray while the mouse is over it.

   An icon's background is ParentRelative (GTK's choice when the tray has
   no alpha visual), so it shows our picture through, and is told to
   redraw when the picture does. IceWM's own tray is off
   (TaskBarEnableSystemTray=0), or it would hold the selection first.

   When this program goes (a new scale starts it over), the X server puts
   each icon back on the root window (the save set), the program sees its
   tray is gone and docks again with the next one. That is why the tray
   is a window of the root and not of IceWM's bar: the server gives an
   icon to the nearest window that isn't ours, and IceWM restarts with
   the scale too, destroying its bar and everything in it. */
#define DOCK_REQUEST 0              /* SYSTEM_TRAY_REQUEST_DOCK */
#define XEMBED_EMBEDDED_NOTIFY 0

static void redraw_icons(void)
{
    for (int i = 0; i < ndocked; i++)
        XClearArea(dpy, docked[i].w, 0, 0, 0, 0, True);
}

/* The places for icons in the tray itself, left to right. */
static int tray_places(int *xs)
{
    int n = 0;
    xs[n++] = slot_x;
    if (bat_size && !bat.present)
        xs[n++] = bat_x;
    return n;
}

/* The panel's grid: the tray's step between icons, its height per row. */
static int panel_step(void)
{
    return vol_x > slot_x ? vol_x - slot_x : slot_size + 2 * slot_y;
}

static int panel_cols(void)
{
    int c = (tray_pic.w - slot_x) / panel_step();
    return c < 1 ? 1 : c;
}

/* The panel grows out of the tray: panel_win is the part that shows, its
   bottom on the tray's top, and it gets taller; panel_inner, inside it,
   is the whole panel with the icons, kept with its bottom on panel_win's
   bottom, so the rows rise into view rather than unrolling from the top. */
#define GROW_MS 500
#define SHRINK_MS 250

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* Tell the tray's pop-ups where its top is now, in screen pixels, so they
   move with it (live/common/tray_follow.h). Only when it changed. */
static void say_tray_top(int tray_y)
{
    long top = panel_h ? tray_y - panel_h + tray_seam() : tray_y;
    if (top == tray_top_said)
        return;
    tray_top_said = top;
    XChangeProperty(dpy, root, a_tray_top, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&top, 1);
}

static void set_panel_height(int h)
{
    if (!panel_win || h == panel_h)
        return;
    Window child;
    int x, y;
    XTranslateCoordinates(dpy, tray_win, root, 0, 0, &x, &y, &child);
    if (h <= 0) {
        XUnmapWindow(dpy, panel_win);
        panel_h = 0;
    } else {
        XMoveResizeWindow(dpy, panel_win, x, y - h + tray_seam(), (unsigned)tray_pic.w, (unsigned)h);
        XMoveWindow(dpy, panel_inner, 0, h - panel_full());
        if (!panel_h)
            XMapRaised(dpy, panel_win);
        panel_h = h;
    }
    say_tray_top(y);
    XFlush(dpy);
}

/* One frame: where the height should be by now, easing out (fast, then
   slowing as it arrives). Called from the main loop, which wakes every
   15 ms while a move is on (nap_ms). */
static void step_panel(void)
{
    if (!anim_ms)
        return;
    long t = now_ms() - anim_start;
    if (t >= anim_ms) {
        anim_ms = 0;
        set_panel_height(anim_to);
        return;
    }
    long f = 1000 - (1000 - t * 1000 / anim_ms) * (1000 - t * 1000 / anim_ms) / 1000;
    set_panel_height(anim_from + (int)((anim_to - anim_from) * f / 1000));
}

static void animate_panel(int to, int ms)
{
    if (to == (anim_ms ? anim_to : panel_h))
        return;                     /* already there, or on its way */
    anim_from = panel_h, anim_to = to;
    anim_start = now_ms(), anim_ms = ms;
    step_panel();
}

/* c moved toward white by n/256. */
static uint32_t lighter(uint32_t c, int n)
{
    uint32_t r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    r += (255 - r) * n / 256, g += (255 - g) * n / 256, b += (255 - b) * n / 256;
    return r << 16 | g << 8 | b;
}

/* The tray picture's top rows are a light highlight line (1 px at 100%).
   The grown panel covers them, so tray and panel read as one piece, and
   puts the same line along its own top instead. */
static int tray_seam(void)
{
    int s = tray_pic.h / 40;
    return s < 1 ? 1 : s;
}

/* How tall the whole panel is: its rows, and the seam it covers. */
static int panel_full(void)
{
    return panel_rows * tray_pic.h + tray_seam();
}

/* The panel's picture, `rows` high: the tray grown taller. Its colour is
   the tray's own teal where it meets the tray (the column under the
   clock, which is plain: the time is drawn at run time, just below the
   highlight line), getting a little lighter toward the top; the divider
   runs down the left as on the bar, and the highlight line closes the top. */
static void size_panel(int rows)
{
    if (rows == panel_rows && panel_win)
        return;
    int seam = tray_seam();
    int w = tray_pic.w, h = rows * tray_pic.h + seam;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px)
        die("out of memory");
    int plain = clock_x > 0 && clock_x < tray_pic.w ? clock_x : tray_pic.w - 1;
    const uint32_t *below = tray_pic.px + (size_t)(seam * 4) * tray_pic.w;
    uint32_t base = below[plain] & 0xffffff;
    uint32_t hi = tray_pic.px[plain] & 0xffffff, hi0 = tray_pic.px[0] & 0xffffff;
    uint32_t edge = tray_pic.px[(size_t)(tray_pic.h / 2) * tray_pic.w] & 0xffffff;
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (size_t)y * w;
        int top = y < seam;
        uint32_t c = top ? hi : lighter(base, 40 * (h - y) / h);
        row[0] = top ? hi0 : edge;
        for (int x = 1; x < w; x++)
            row[x] = c;
    }
    Pixmap pm = pixmap_from(px, w, h);
    free(px);
    if (!panel_win) {
        XSetWindowAttributes a = { .override_redirect = True, .background_pixmap = None,
                                   .event_mask = EnterWindowMask | LeaveWindowMask | ButtonPressMask };
        panel_win = XCreateWindow(dpy, root, 0, 0, w, 1, 0, depth, InputOutput, vis,
                                  CWOverrideRedirect | CWBackPixmap | CWEventMask, &a);
        a.background_pixmap = pm;
        panel_inner = XCreateWindow(dpy, panel_win, 0, 0, w, h, 0, depth, InputOutput, vis,
                                    CWBackPixmap, &a);
        XMapWindow(dpy, panel_inner);
    } else {
        XResizeWindow(dpy, panel_inner, w, h);
        XSetWindowBackgroundPixmap(dpy, panel_inner, pm);
        XClearWindow(dpy, panel_inner);
    }
    if (panel_bg)
        XFreePixmap(dpy, panel_bg);
    panel_bg = pm;
    panel_rows = rows;
    if (panel_h) {                  /* open, and an icon came or went: the new height at once */
        panel_h = 0;
        anim_ms = 0;
        set_panel_height(panel_full());
    }
}

/* Hand an icon to the tray or the panel, at (x, y) there. */
static void embed(struct docked *d, Window in, int x, int y)
{
    if (d->in == in && d->x == x && d->y == y)
        return;
    XReparentWindow(dpy, d->w, in, x, y);
    XResizeWindow(dpy, d->w, (unsigned)slot_size, (unsigned)slot_size);
    XClientMessageEvent m = { .type = ClientMessage, .window = d->w, .format = 32,
                              .message_type = a_xembed };
    m.data.l[0] = CurrentTime;
    m.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
    m.data.l[3] = (long)in;
    XSendEvent(dpy, d->w, False, NoEventMask, (XEvent *)&m);
    XMapRaised(dpy, d->w);
    d->in = in, d->x = x, d->y = y;
}

/* Every icon to its place: the tray's places first, the rest in the panel,
   its bottom row nearest the tray. Icons already in place aren't touched. */
static void lay_out_icons(void)
{
    if (!tray_win)
        return;
    int xs[3], nplaces = tray_places(xs);
    int extra = ndocked > nplaces ? ndocked - nplaces : 0;
    int cols = panel_cols(), rows = (extra + cols - 1) / cols;
    if (extra)
        size_panel(rows);
    else {
        anim_ms = 0;
        set_panel_height(0);
    }
    for (int i = 0; i < ndocked; i++) {
        if (i < nplaces) {
            embed(&docked[i], tray_win, xs[i], slot_y);
            continue;
        }
        int k = i - nplaces;
        embed(&docked[i], panel_inner, slot_x + k % cols * panel_step(),
              (rows - 1 - k / cols) * tray_pic.h + slot_y);
    }
}

/* Where the pointer is: 1 over the tray or the open panel above it. */
static int pointer_in_tray(void)
{
    Window r, c;
    int rx, ry, x, y;
    unsigned mask;
    if (!tray_win || !XQueryPointer(dpy, tray_win, &r, &c, &rx, &ry, &x, &y, &mask))
        return 0;
    return x >= 0 && x < tray_pic.w && y >= -panel_h && y < tray_pic.h;
}

/* The mouse came onto the tray: grow the panel out of it, if anything is in it. */
static void show_panel(void)
{
    int xs[3];
    if (!panel_win || ndocked <= tray_places(xs))
        return;
    animate_panel(panel_full(), GROW_MS);
}

/* The mouse left the tray: fold the panel away, unless it only went from one to the other. */
static void hide_panel(void)
{
    if ((panel_h || anim_ms) && !pointer_in_tray())
        animate_panel(0, SHRINK_MS);
}

/* At once, for a click and for the bar going away. */
static void close_panel(void)
{
    anim_ms = 0;
    set_panel_height(0);
}

/* Become the tray, unless something else already is. */
static void own_tray(void)
{
    if (!slot_size)
        return;                     /* (a theme without a slot) */
    char name[32];
    snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", screen);
    a_tray_sel = XInternAtom(dpy, name, False);
    a_tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    a_xembed = XInternAtom(dpy, "_XEMBED", False);
    if (XGetSelectionOwner(dpy, a_tray_sel) != None)
        return;
    tray_owner = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    long horizontal = 0;
    XChangeProperty(dpy, tray_owner, XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False), XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)&horizontal, 1);
    XSetSelectionOwner(dpy, a_tray_sel, tray_owner, CurrentTime);
    if (XGetSelectionOwner(dpy, a_tray_sel) != tray_owner) {
        XDestroyWindow(dpy, tray_owner);
        tray_owner = 0;
        return;
    }
    /* "There is a tray now": programs already waiting for one dock. */
    XClientMessageEvent m = { .type = ClientMessage, .window = root, .format = 32,
                              .message_type = XInternAtom(dpy, "MANAGER", False) };
    m.data.l[0] = CurrentTime;
    m.data.l[1] = (long)a_tray_sel;
    m.data.l[2] = (long)tray_owner;
    XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent *)&m);
}

/* Which place an icon wants: the Wi-Fi icon first, the language (IBus's
   panel, ibus-ui-gtk3) next, then the rest as they come. */
static int rank_of(Window w)
{
    XClassHint ch = { 0 };
    int rank = 2;
    if (XGetClassHint(dpy, w, &ch) && ch.res_name) {
        if (!strcmp(ch.res_name, "myos-wifi"))
            rank = 0;
        else if (!strncmp(ch.res_name, "ibus", 4))
            rank = 1;
    }
    if (ch.res_name)
        XFree(ch.res_name);
    if (ch.res_class)
        XFree(ch.res_class);
    return rank;
}

static void dock(Window w)
{
    for (int i = 0; i < ndocked; i++)
        if (docked[i].w == w)
            return;                 /* asked twice */
    if (ndocked == MAX_DOCK)
        return;
    int rank = rank_of(w), at = ndocked;
    while (at > 0 && docked[at - 1].rank > rank)
        at--;
    memmove(&docked[at + 1], &docked[at], (size_t)(ndocked - at) * sizeof docked[0]);
    docked[at] = (struct docked){ .w = w, .rank = rank };
    ndocked++;
    XSelectInput(dpy, w, StructureNotifyMask);
    XAddToSaveSet(dpy, w);          /* if we go, it goes back to the root, not away */
    lay_out_icons();
}

static void undock(Window w)
{
    for (int i = 0; i < ndocked; i++)
        if (docked[i].w == w) {
            memmove(&docked[i], &docked[i + 1], (size_t)(ndocked - i - 1) * sizeof docked[0]);
            ndocked--;
            lay_out_icons();
            return;
        }
}

/* The window that contains w, or 0. */
static Window parent_of(Window w)
{
    Window r, p, *kids = NULL;
    unsigned n = 0;
    if (!w || w == root || !XQueryTree(dpy, w, &r, &p, &kids, &n))
        return 0;
    if (kids)
        XFree(kids);
    return p;
}

/* A wide, short window: the taskbar, not a program and not the clock. */
static int is_bar(Window w)
{
    XWindowAttributes wa;
    if (!w || !tray_pic.h || !XGetWindowAttributes(dpy, w, &wa))
        return 0;
    return wa.map_state == IsViewable && wa.width >= sw / 2 &&
           wa.height >= tray_pic.h / 2 && wa.height < sw / 4;
}

/* IceWM's taskbar. Its own name ("TaskBar") is set with XSetWMProperties,
   and XFetchName does not see that, so looking the bar up by name never
   found it and the tray was never drawn. The Start button and the clock
   are named with XStoreName, which we can see (the button is how the
   Start picture already gets onto the bar). The bar is the first wide
   strip above either of them, and the clock is one of its children. */
static Window taskbar_window(void)
{
    Window seeds[2];
    int nseeds = 0;
    if (start_win)
        seeds[nseeds++] = start_win;
    else {
        Window menu = find_named(root, "TaskBarMenu", 8);
        if (menu)
            seeds[nseeds++] = menu;
    }
    Window clock = find_named(root, "Clock", 8);
    if (clock)
        seeds[nseeds++] = clock;

    for (int s = 0; s < nseeds; s++) {
        Window cur = seeds[s];
        for (int i = 0; i < 8 && cur && cur != root; i++) {
            Window p = parent_of(cur);
            if (!p || p == root)
                break;
            if (is_bar(p))
                return p;
            cur = p;
        }
    }
    return 0;
}

/* Put the tray over the bottom-right of the taskbar, and in the stacking
   order just above the bar's top-level window: over the bar and its clock,
   under anything that covers the bar (a full-screen window). Hidden while
   the bar is. Moving and restacking only when it isn't already right: each
   sends another ConfigureNotify, and following that blindly would chase
   itself. */
static void place_tray(void)
{
    XWindowAttributes frame, self;
    if (!tray_win || !tray_frame || !bar_window)
        return;
    if (!XGetWindowAttributes(dpy, tray_frame, &frame) || !XGetWindowAttributes(dpy, tray_win, &self))
        return;
    if (frame.map_state != IsViewable) {
        if (self.map_state != IsUnmapped)
            XUnmapWindow(dpy, tray_win);
        close_panel();
        return;
    }
    Window child;
    int fx, fy;
    if (!XTranslateCoordinates(dpy, tray_frame, root, 0, 0, &fx, &fy, &child))
        return;
    int x = fx + frame.width - tray_pic.w, y = fy + frame.height - tray_pic.h;
    if (self.x != x || self.y != y)
        XMoveWindow(dpy, tray_win, x, y);
    say_tray_top(y);

    Window r, parent, *kids = NULL;
    unsigned n = 0;
    int bar = -1, me = -1;
    if (XQueryTree(dpy, root, &r, &parent, &kids, &n))
        for (unsigned i = 0; i < n; i++) {
            if (kids[i] == bar_window)
                bar = (int)i;
            else if (kids[i] == tray_win)
                me = (int)i;
        }
    if (kids)
        XFree(kids);
    if (bar >= 0 && me != bar + 1) {
        XWindowChanges c = { .sibling = bar_window, .stack_mode = Above };
        XConfigureWindow(dpy, tray_win, CWSibling | CWStackMode, &c);
    }
    if (self.map_state == IsUnmapped)
        XMapWindow(dpy, tray_win);
    hide_panel();                   /* a leave we didn't see (a menu held the pointer) */
}

/* Once IceWM has made the taskbar (again, after a restart). The tray is
   override-redirect, so IceWM leaves it alone. Clicks are taken and
   ignored, so they don't fall through onto a task button. */
static void attach_tray(void)
{
    if (tray_frame || !tray_pic.px)
        return;
    Window frame = taskbar_window();
    if (!frame)
        return;
    Window top = frame;
    for (Window p; (p = parent_of(top)) && p != root;)
        top = p;
    if (!tray_win) {
        paint_tray();
        if (!tray_bg)
            return;
        XSetWindowAttributes a = { .background_pixmap = tray_bg, .override_redirect = True,
                                   .event_mask = StructureNotifyMask | ButtonPressMask |
                                                 EnterWindowMask | LeaveWindowMask };
        tray_win = XCreateWindow(dpy, root, 0, 0, tray_pic.w, tray_pic.h, 0, depth, InputOutput, vis,
                                 CWBackPixmap | CWOverrideRedirect | CWEventMask, &a);
        if (!tray_win)
            return;
    }
    XSelectInput(dpy, frame, StructureNotifyMask);
    if (top != frame)
        XSelectInput(dpy, top, StructureNotifyMask);
    /* The bar can go between finding it and here: then try again later. */
    XSync(dpy, False);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, frame, &wa) || !XGetWindowAttributes(dpy, top, &wa))
        return;
    tray_frame = frame, bar_window = top;
    place_tray();
    lay_out_icons();                /* ones that docked before the bar was there */
}

/* Read the battery again every 10 seconds, as often as the kernel does.
   1 if what the icon shows changed. */
static int battery_changed(void)
{
    time_t now = time(NULL);
    if (!bat_size || now - bat_read < 10)
        return 0;
    bat_read = now;
    struct power_supply was = bat;
    ps_read(PS_DIR, &bat);
    return bat.present != was.present || bat.percent != was.percent ||
           bat.charging != was.charging || bat.full != was.full || bat.on_ac != was.on_ac;
}

/* Redraw when the minute changes, or the battery does. */
static void refresh_tray(void)
{
    if (!tray_win)
        return;
    if (!battery_changed() && (!f_clock || time(NULL) / 60 == tray_minute))
        return;
    paint_tray();
}

/* How long to sleep. Until the taskbar appears, look often. Once the tray
   is up, wake at least once a second: IceWM restacks its clock on the
   second, and the tray has to stay above it. Also wake for the new minute,
   which is when the time is drawn again. No tray picture means there is
   nothing to wait for on that side. While the panel grows or shrinks, the
   next frame is 15 ms away. */
static int nap_ms(void)
{
    if (anim_ms)
        return 15;
    if (!start_win || (tray_pic.px && !tray_frame))
        return 250;
    struct timeval now;
    gettimeofday(&now, NULL);
    int ms = 1000 - (int)(now.tv_usec / 1000);
    if (f_clock) {
        int to_min = 60000 - (int)(now.tv_sec % 60) * 1000 - (int)(now.tv_usec / 1000);
        if (to_min < ms)
            ms = to_min;
    }
    if (ms < 200)
        ms = 200;
    return ms;
}

/* Where the top of the taskbar is: the top of the button's top-level window. */
static int bar_top(void)
{
    if (!start_win)
        return sh - start_h;
    Window w = start_win, r, parent, *kids;
    unsigned n;
    for (;;) {
        if (!XQueryTree(dpy, w, &r, &parent, &kids, &n))
            return sh - start_h;
        if (kids)
            XFree(kids);
        if (parent == root)
            break;
        w = parent;
    }
    XWindowAttributes wa;
    return XGetWindowAttributes(dpy, w, &wa) ? wa.y : sh - start_h;
}

/* --- the hot corner -------------------------------------------------------------- */
/* The top-right corner of the desktop, curled over like a page. It lives in
   a small window of the "desktop" type, which IceWM keeps below every
   other window, so it is above the wallpaper and under windows, as the
   theme says. A click opens Mission Control for this desktop
   (myos-mission). The desktops along the top are the Wayland session.

   So a window there (a maximized one always is: IceWM hides its border and
   its close button is in that corner, so the curl can't be on top) can't
   hide it, pushing the mouse into the corner opens it too, as on the Mac.
   The edge stops the pointer, so the push is felt with two pointer
   barriers (XFixes), invisible lines along the top and right edges for the
   curl's length: the X server tells us each time the pointer runs into one
   and how far it would have gone. That costs nothing until the pointer is
   pressed against the corner. Pressure is counted as GNOME's hot corner
   does: at most PUSH_STEP per event, so a flick at the close button
   counts for little, leaking away at PUSH_PX per PUSH_MS, and PUSH_PX
   opens it, once per push: a push ends when the pointer leaves the
   corner or rests there for PUSH_MS. A push while a button is held or a
   menu has the pointer (the server says "grabbed") doesn't count. */

#define PUSH_PX   100               /* pixels of push that open Mission Control */
#define PUSH_MS   1000              /* ... if pushed within this long */
#define PUSH_STEP 15                /* the most one event counts */

static pid_t mission_pid;
static int xi_opcode;               /* XInput's events, or 0: no pushing */
static PointerBarrier push_right;   /* the vertical barrier; the other is the top */
static double push;                 /* pixels pushed, leaking away */
static Time push_time;
static int pushed;                  /* opened: not again until the pointer leaves */

static void open_mission(void)
{
    if (mission_pid > 0 && waitpid(mission_pid, NULL, WNOHANG) == 0)
        return;                         /* already open */
    mission_pid = fork();
    if (mission_pid == 0) {
        execl("/usr/bin/myos-mission", "myos-mission", (char *)NULL);
        _exit(127);
    }
}

/* The two barriers, c pixels each way from the corner. The right one lies
   just off the screen (x = sw), so the pointer still reaches the last
   column; the top one on row 0, which the pointer stays on. Each lets the
   pointer through going away from its edge. A server without XFixes 5 or
   XInput 2.3 has no barriers: then only the click works. */
static void make_push(int c)
{
    int ev, err, maj = 5, min = 0;
    if (!XFixesQueryExtension(dpy, &ev, &err) || !XFixesQueryVersion(dpy, &maj, &min) || maj < 5)
        return;
    int op;
    maj = 2, min = 3;
    if (!XQueryExtension(dpy, "XInputExtension", &op, &ev, &err) ||
        XIQueryVersion(dpy, &maj, &min) != Success || maj * 100 + min < 203)
        return;
    unsigned char bits[XI_BarrierLeave / 8 + 1] = {0};
    bits[XI_BarrierHit / 8] |= 1 << (XI_BarrierHit % 8);
    bits[XI_BarrierLeave / 8] |= 1 << (XI_BarrierLeave % 8);
    XIEventMask m = { XIAllMasterDevices, sizeof bits, bits };
    XISelectEvents(dpy, root, &m, 1);
    push_right = XFixesCreatePointerBarrier(dpy, root, sw, 0, sw, c, BarrierNegativeX, 0, NULL);
    XFixesCreatePointerBarrier(dpy, root, sw - c, 0, sw, 0, BarrierPositiveY, 0, NULL);
    xi_opcode = op;
}

/* An event from the barriers: count the push, or start over. */
static void corner_push(XEvent *ev)
{
    XGenericEventCookie *ck = &ev->xcookie;
    if (!xi_opcode || ck->extension != xi_opcode || !XGetEventData(dpy, ck))
        return;
    const XIBarrierEvent *b = ck->data;
    if (ck->evtype == XI_BarrierLeave) {
        push = 0;
        pushed = 0;
    } else if (ck->evtype == XI_BarrierHit && !(b->flags & XIBarrierDeviceIsGrabbed)) {
        int right = b->barrier == push_right;
        double across = right ? b->dx : -b->dy;     /* into the edge */
        double along = right ? b->dy : b->dx;
        if (along < 0)
            along = -along;
        if (across > 0 && along <= across) {        /* (not sliding along the edge) */
            push -= (double)(uint32_t)(b->time - push_time) * PUSH_PX / PUSH_MS;
            push_time = b->time;
            /* All leaked away: a new push, even without a leave (the
               server sends none after an absolute jump, a touch screen's). */
            if (push <= 0) {
                push = 0;
                pushed = 0;
            }
            push += across < PUSH_STEP ? across : PUSH_STEP;
            if (push >= PUSH_PX) {
                push = PUSH_PX;     /* (so a second's rest always ends it) */
                if (!pushed) {
                    pushed = 1;
                    open_mission();
                }
            }
        }
    }
    XFreeEventData(dpy, ck);
}

/* Draw both curls over the wallpaper behind them. The wallpaper comes from
   the pixmap icewmbg publishes on the root window, not from the screen, so
   a window that happens to be over the corner never gets baked in. */
static void build_corner(void)
{
    int c = corner_pic[0].w;
    Drawable from = root;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, a_rootpm, 0, 1, False, AnyPropertyType, &type, &fmt,
                           &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32 && *(Pixmap *)data)
            from = *(Pixmap *)data;
        XFree(data);
    }
    uint32_t *under = grab(from, sw - c, 0, c, c);
    uint32_t *px = malloc((size_t)c * c * 4);
    if (!px)
        die("out of memory");
    for (int i = 0; i < 2; i++) {
        memcpy(px, under, (size_t)c * c * 4);
        over(px, c, c, &corner_pic[i], 0, 0);
        if (corner_pm[i])
            XFreePixmap(dpy, corner_pm[i]);
        corner_pm[i] = pixmap_from(px, c, c);
    }
    free(px);
    free(under);
}

static void show_corner(int hover)
{
    XSetWindowBackgroundPixmap(dpy, corner_win, corner_pm[hover]);
    XClearWindow(dpy, corner_win);
}

static void make_corner(void)
{
    if (!corner_pic[0].px)
        return;                     /* not in this theme */
    int c = corner_pic[0].w;
    XSetWindowAttributes a = { .event_mask = EnterWindowMask | LeaveWindowMask | ButtonPressMask };
    corner_win = XCreateWindow(dpy, root, sw - c, 0, c, c, 0, depth, InputOutput, vis,
                               CWEventMask, &a);
    Atom wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desk = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, corner_win, wtype, XA_ATOM, 32, PropModeReplace, (unsigned char *)&desk, 1);
    XSizeHints *h = XAllocSizeHints();
    h->flags = USPosition | USSize | PMinSize | PMaxSize;
    h->x = sw - c, h->y = 0;
    h->width = h->min_width = h->max_width = c;
    h->height = h->min_height = h->max_height = c;
    XSetWMNormalHints(dpy, corner_win, h);
    XFree(h);
    XStoreName(dpy, corner_win, "hot corner");
    a_rootpm = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    build_corner();
    show_corner(0);
    XMapWindow(dpy, corner_win);
    make_push(c);
}

/* --- the menu, again ------------------------------------------------------------- */

static char run_cmd[240];          /* a program chosen from All Programs */

static int is_programs(int i)
{
    return i >= 0 && !strcmp(items[i].cmd, "@programs");
}

/* The next event, or 0 if `ms` passed with none (ms < 0 waits forever).
   All Programs opens a folder a moment after the pointer rests on it, so
   the menu cannot block inside XNextEvent. */
static int wait_input(XEvent *ev, int ms)
{
    int fd = ConnectionNumber(dpy);
    if (XPending(dpy)) {
        next_event(ev);
        return 1;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv, *tp = NULL;
    if (ms >= 0) {
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        tp = &tv;
    }
    XFlush(dpy);
    if (select(fd + 1, &fds, NULL, NULL, tp) > 0 && XPending(dpy)) {
        next_event(ev);
        return 1;
    }
    return 0;
}

/* Show the menu until something is chosen; returns the entry, or -1.
   -3 means All Programs chose run_cmd. The menu stands on the taskbar,
   at the left, with Start held down below it. */
static int menu(void)
{
    int w = menu_pic.w, h = menu_pic.h, y = bar_top() - h;
    if (y < 0)
        y = 0;
    uint32_t *px = grab(root, 0, y, w, h);
    over(px, w, h, &menu_pic, 0, 0);
    Pixmap bg = pixmap_from(px, w, h);
    free(px);
    text(bg, f_name, 0x000000, 204, name_x + 1, name_y + 1, user);     /* the theme's label shadow */
    text(bg, f_name, 0xffffff, 255, name_x, name_y, user);

    Window win = popup(0, y, w, h, bg);
    if (!grab_input(win, 1)) {
        close_popup(win, bg);
        return -1;
    }
    struct programs_env pe = {
        .dpy = dpy, .vis = vis, .cmap = cmap, .root = root,
        .screen = screen, .depth = depth, .sw = sw, .sh = sh,
        .scale = scale, .font = f_prog ? f_prog : f_type,
    };
    programs_load(&pe);
    for (int i = 0; i < nitems; i++)
        if (is_programs(i))
            programs_place(0, y, w, h, items[i].r.x, items[i].r.y, items[i].r.w, items[i].r.h);

    show_start(START_PRESSED);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    int cur = -1, chosen = -2;      /* -2 still open */
    while (chosen == -2) {
        XEvent ev;
        if (!wait_input(&ev, programs_arm_ms())) {
            programs_tick();
            continue;
        }
        if (programs_owns(ev.xany.window)) {
            int a = programs_event(&ev, run_cmd, (int)sizeof run_cmd);
            if (a == 1)
                chosen = -3;
            else if (a == -1)
                chosen = -1;
            continue;
        }
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.window == win && ev.xexpose.count == 0)
                light(win, gc, -1, cur);
            break;
        case MotionNotify: {
            int i;
            if (ev.xmotion.window != win)
                break;
            i = item_at(ev.xmotion.x, ev.xmotion.y);
            if (i != cur)
                light(win, gc, cur, i), cur = i;
            if (is_programs(i))
                programs_hover_item(1);
            else if (i >= 0)
                programs_hover_item(0);
            else
                programs_hover_item(-1);
            break;
        }
        case ButtonPress:
            /* A click outside the menu (Start included) closes it. A click
               on an entry runs it when the button comes up, so you can
               also press Start, slide up to an entry and let go. The
               flyouts are not "outside". */
            if (programs_hit(ev.xbutton.x_root, ev.xbutton.y_root))
                break;
            if (ev.xbutton.window != win ||
                !inside(&(struct rect){ 0, 0, w, h }, ev.xbutton.x, ev.xbutton.y))
                chosen = -1;
            break;
        case ButtonRelease:
            if (ev.xbutton.window != win || ev.xbutton.button != Button1)
                break;
            {
                int i = item_at(ev.xbutton.x, ev.xbutton.y);
                if (is_programs(i))
                    programs_toggle();
                else if (i >= 0)
                    chosen = i;
            }
            break;
        case KeyPress: {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            int next = cur;
            if (programs_open()) {
                int a = programs_key((unsigned long)k, run_cmd, (int)sizeof run_cmd);
                if (a == 1)
                    chosen = -3;
                else if (a == -1)
                    chosen = -1;
                break;
            }
            if (k == XK_Escape || opens_menu(&ev.xkey))
                chosen = -1;
            else if ((k == XK_Return || k == XK_KP_Enter) && cur >= 0) {
                if (is_programs(cur))
                    programs_toggle();
                else
                    chosen = cur;
            } else if (k == XK_Right && is_programs(cur))
                programs_toggle();
            else if (k == XK_Down || k == XK_Tab)
                next = (cur + 1) % nitems;
            else if (k == XK_Up)
                next = cur <= 0 ? nitems - 1 : cur - 1;
            if (next != cur)
                light(win, gc, cur, next), cur = next;
            break;
        }
        }
    }
    programs_close();
    XFreeGC(dpy, gc);
    close_popup(win, bg);
    show_start(START_NORMAL);
    return chosen;
}

static void open_menu(void)
{
    int i = menu();
    if (i == -3)
        launch(run_cmd);
    if (i < 0)
        return;
    if (strcmp(items[i].cmd, "@run") == 0)
        run_dialog();
    else
        launch(items[i].cmd);
}

static long root_cardinal(Atom a, long dflt)
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

/* Three fingers sliding on the touchpad come as F14-F17: the kernel's
   HID_SWIPE_* keys (include/hid.h); X's keycode is the Linux code + 8,
   which holds whatever names the keymap gives them. As on a Mac, the
   desktops follow the fingers: sliding left brings the one on the right,
   with no wrapping round, and slide there: the change goes to whoever owns
   _MYOS_SLIDE (myos-mission --keep), which covers the screen and asks
   IceWM itself; with nobody there, straight to IceWM. Up opens Mission
   Control. Down closes it, which myos-mission sees itself: it has the
   keyboard while it is open. */
#define SWIPE_LEFT  (184 + 8)
#define SWIPE_RIGHT (185 + 8)
#define SWIPE_UP    (186 + 8)
#define SWIPE_DOWN  (187 + 8)

static void swiped(unsigned kc, Time t)
{
    static Atom a_cur, a_num, a_slide;
    if (kc == SWIPE_UP) {
        open_mission();
        return;
    }
    if (kc != SWIPE_LEFT && kc != SWIPE_RIGHT)
        return;
    if (!a_cur) {
        a_cur = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
        a_num = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
        a_slide = XInternAtom(dpy, "_MYOS_SLIDE", False);
    }
    long cur = root_cardinal(a_cur, -1), to = cur + (kc == SWIPE_LEFT ? 1 : -1);
    if (cur < 0 || to < 0 || to >= root_cardinal(a_num, 0))
        return;
    Window slider = XGetSelectionOwner(dpy, a_slide);
    XEvent e = { .xclient = { .type = ClientMessage, .window = slider ? slider : root,
                              .message_type = slider ? a_slide : a_cur,
                              .format = 32, .data.l = { to, (long)t } } };
    if (slider)                     /* (no mask: to the program that made the window) */
        XSendEvent(dpy, slider, False, NoEventMask, &e);
    else
        XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

/* The desktop's scale, as myos-xsettings decided it: _MYOS_SCALE, or 0
   for "automatic", which is _MYOS_SCALE_AUTO. The theme has pictures for
   100, 125 ... 300%, so it's rounded to the nearest of those. */
static int desktop_scale(void)
{
    long s = root_cardinal(a_scale, 0);
    if (s <= 0)
        s = root_cardinal(a_scale_auto, 100);
    s = (s + 12) / 25 * 25;
    return s < 100 ? 100 : s > 300 ? 300 : (int)s;
}

int main(int argc, char **argv)
{
    (void)argc;
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        die("cannot open the display");
    XSetErrorHandler(x_error);
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);     /* not into the programs we start */
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    vis = DefaultVisual(dpy, screen);
    cmap = DefaultColormap(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);

    a_scale = XInternAtom(dpy, "_MYOS_SCALE", False);
    a_scale_auto = XInternAtom(dpy, "_MYOS_SCALE_AUTO", False);
    a_tray_top = XInternAtom(dpy, "_MYOS_TRAY_TOP", False);
    XSelectInput(dpy, root, PropertyChangeMask);    /* the scale, the wallpaper */
    scale = desktop_scale();
    snprintf(dir, sizeof dir, "/usr/share/myos/menu/%d/", scale);

    read_conf();
    if (bat_size) {
        ps_read(PS_DIR, &bat);
        bat_read = time(NULL);
    }
    read_user();
    char spec[96];
    snprintf(spec, sizeof spec, "Trebuchet MS,DejaVu Sans:bold:pixelsize=%d", name_size);
    f_name = XftFontOpenName(dpy, screen, spec);
    snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", type_size);
    f_type = XftFontOpenName(dpy, screen, spec);
    snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", type_size * 3 / 2);
    f_prog = XftFontOpenName(dpy, screen, spec);
    if (clock_size) {
        snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", clock_size);
        f_clock = XftFontOpenName(dpy, screen, spec);
    }
    if (!f_name || !f_type)
        die("no fonts");

    grab_key(XK_Escape, ControlMask);
    grab_key(XK_Super_L, 0);
    grab_key(XK_Super_R, 0);
    for (int kc = SWIPE_LEFT; kc <= SWIPE_DOWN; kc++)    /* (no program sees them) */
        XGrabKey(dpy, kc, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    make_corner();
    own_tray();

    int fd = ConnectionNumber(dpy);
    for (;;) {
        step_panel();
        /* Until IceWM's taskbar exists (it may still be starting, or
           restarting), look for it four times a second. Once the tray is
           up, keep it at the right end and above the clock. */
        if (!XPending(dpy)) {
            if (!start_win)
                attach_start();
            if (!tray_frame)
                attach_tray();
            else if (!anim_ms) {    /* (not every frame of the panel's move) */
                place_tray();
                refresh_tray();
            }
            if (!XPending(dpy)) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                int ms = nap_ms();
                /* select does not send what is waiting to go out, and the
                   tray was just created above. */
                XFlush(dpy);
                select(fd + 1, &fds, NULL, NULL, &(struct timeval){ ms / 1000, (ms % 1000) * 1000 });
                continue;
            }
        }
        XEvent ev;
        next_event(&ev);
        switch (ev.type) {
        case ButtonPress:
            if (ev.xbutton.window == corner_win && ev.xbutton.button == Button1)
                open_mission();
            else if (ev.xbutton.window == start_win && ev.xbutton.button == Button1)
                open_menu();
            else if (ev.xbutton.window == tray_win && !battery_click(&ev.xbutton) && vol_size)
                speaker_click(&ev.xbutton);
            break;
        case MapNotify:
        case ConfigureNotify:
        case ReparentNotify:
            /* IceWM moves or restacks the bar; stay on it. */
            if (tray_win && tray_frame && (ev.xany.window == tray_frame || ev.xany.window == bar_window))
                place_tray();
            break;
        case UnmapNotify:
            if (tray_win && tray_frame && (ev.xany.window == tray_frame || ev.xany.window == bar_window))
                place_tray();       /* the bar hid: so does the tray */
            break;
        case PropertyNotify:
            /* A new scale: every picture and size changes, so start over
               as a fresh copy of this program (IceWM restarts too). The X
               connection is close-on-exec, which takes our windows away. */
            if (ev.xproperty.window == root &&
                (ev.xproperty.atom == a_scale || ev.xproperty.atom == a_scale_auto) &&
                desktop_scale() != scale) {
                execvp(argv[0], argv);
                die("cannot restart");
            }
            if (ev.xproperty.window == root && ev.xproperty.atom == a_rootpm && corner_win) {
                build_corner();
                show_corner(0);
            }
            break;
        case EnterNotify: {
            if (corner_win && ev.xcrossing.window == corner_win) {
                show_corner(1);
                break;
            }
            if (tray_win && (ev.xcrossing.window == tray_win || ev.xcrossing.window == panel_win)) {
                /* Only when the mouse moved here: a pop-up taking the
                   pointer and letting it go is an enter too. */
                if (ev.xcrossing.mode == NotifyNormal)
                    show_panel();
                break;
            }
            show_start(START_HOVER);
            /* IceWM's button saw the pointer come in too and is about to
               show its own tooltip ("Favorite Applications"). Telling it
               the pointer left again cancels that, and only that. */
            Window r, parent, *kids;
            unsigned n;
            if (XQueryTree(dpy, start_win, &r, &parent, &kids, &n)) {
                if (kids)
                    XFree(kids);
                XEvent leave = ev;
                leave.xcrossing.type = LeaveNotify;
                leave.xcrossing.window = parent;
                leave.xcrossing.detail = NotifyInferior;
                XSendEvent(dpy, parent, False, LeaveWindowMask, &leave);
            }
            break;
        }
        case LeaveNotify:
            if (corner_win && ev.xcrossing.window == corner_win)
                show_corner(0);
            else if (tray_win && (ev.xcrossing.window == tray_win || ev.xcrossing.window == panel_win)) {
                if (ev.xcrossing.detail != NotifyInferior)     /* not just onto an icon */
                    hide_panel();
            } else
                show_start(START_NORMAL);
            break;
        case KeyPress:
            if (opens_menu(&ev.xkey))
                open_menu();
            else
                swiped(ev.xkey.keycode, ev.xkey.time);
            break;
        case ClientMessage:
            if (ev.xclient.window == tray_owner && ev.xclient.message_type == a_tray_opcode &&
                ev.xclient.data.l[1] == DOCK_REQUEST)
                dock((Window)ev.xclient.data.l[2]);
            break;
        case SelectionClear:
            if (ev.xselectionclear.selection == a_tray_sel)
                tray_owner = 0;     /* another tray took over */
            break;
        case GenericEvent:
            corner_push(&ev);
            break;
        }
    }
}
