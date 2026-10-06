/*
 * myos-menu — the Start menu of the MyOS desktop (the Golden Hour theme).
 *
 * IceWM draws the taskbar and the window buttons, but its own Start menu
 * is a plain list and its clock is a flat rectangle the width of the time.
 * The theme's menu has two columns under a header with your name, and Log
 * off / Turn off along the bottom, so this program draws it, and the Start
 * button too. It also draws the tray at the right of the bar: the mockup's
 * color, the divider, the network and volume icons, and the time. How it
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
 * desktop (see "the hot corner" below), and is the tray's notification
 * area: the Wi-Fi icon (myos-wifi) docks there, where the mockup has its
 * network icon (see "the notification area" below).
 *
 * Everything you see is drawn at build time by live/theme/make-live-theme.py
 * from the theme's design, into /usr/share/myos/menu/<scale>: the whole
 * menu, each entry as it looks under the mouse, the Start button in its
 * three states, the Run box, and the tray. menu.conf there says where each
 * entry is and what it runs, and where the clock sits in the tray. This
 * program adds only your name, the entry under the mouse, what you type,
 * and the time. The scale is the desktop's (100 to 200%, see
 * myos-xsettings); when it changes, the program starts itself over.
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
static Atom a_tray_sel, a_tray_opcode, a_xembed;   /* (the notification area) */
static Window tray_owner;           /* holds the tray selection; dock requests come to it */
static Window icon_win;             /* the docked icon (it stays ours if the bar restarts) */
static void redraw_icon(void);
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
static int grab_input(Window w)
{
    struct timespec t = { 0, 10 * 1000 * 1000 };
    for (int i = 0; i < 50; i++) {
        if (XGrabPointer(dpy, w, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) {
            if (XGrabKeyboard(dpy, w, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
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
    if (ev->xdestroywindow.window == icon_win)
        icon_win = 0;               /* its program quit */
    if (ev->xdestroywindow.window == tray_win || ev->xdestroywindow.window == tray_frame) {
        tray_win = 0;
        tray_frame = 0;
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

/* Run...: type a command, Enter (or OK) starts it. */
static void run_dialog(void)
{
    int w = run_pic.w, h = run_pic.h, x = (sw - w) / 2, y = (sh - h) / 3;
    uint32_t *px = grab(root, x, y, w, h);
    over(px, w, h, &run_pic, 0, 0);
    Pixmap bg = pixmap_from(px, w, h);
    free(px);
    Window win = popup(x, y, w, h, bg);
    if (!grab_input(win)) {
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
    Pixmap pm = pixmap_from(tray_pic.px, tray_pic.w, tray_pic.h);
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
        redraw_icon();
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
   (XEmbed) — ours goes where the mockup draws its network icon. One slot:
   the Wi-Fi icon is the one program that docks. The icon's background is
   ParentRelative (GTK's choice when the tray has no alpha visual), so it
   shows our picture through, and is told to redraw when the picture does.
   IceWM's own tray is off (TaskBarEnableSystemTray=0), or it would hold
   the selection first. */
#define DOCK_REQUEST 0              /* SYSTEM_TRAY_REQUEST_DOCK */
#define XEMBED_EMBEDDED_NOTIFY 0

static void redraw_icon(void)
{
    if (icon_win)
        XClearArea(dpy, icon_win, 0, 0, 0, 0, True);
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

/* Put the icon in its slot, if the tray is up (else when it comes). */
static void embed_icon(void)
{
    if (!icon_win || !tray_win)
        return;
    XReparentWindow(dpy, icon_win, tray_win, slot_x, slot_y);
    XResizeWindow(dpy, icon_win, (unsigned)slot_size, (unsigned)slot_size);
    XClientMessageEvent m = { .type = ClientMessage, .window = icon_win, .format = 32,
                              .message_type = a_xembed };
    m.data.l[0] = CurrentTime;
    m.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
    m.data.l[3] = (long)tray_win;
    XSendEvent(dpy, icon_win, False, NoEventMask, (XEvent *)&m);
    XMapRaised(dpy, icon_win);
}

static void dock(Window w)
{
    if (icon_win && icon_win != w)
        return;                     /* the slot is taken */
    icon_win = w;
    XSelectInput(dpy, w, StructureNotifyMask);
    XAddToSaveSet(dpy, w);          /* if we go, it goes back to the root, not away */
    embed_icon();
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

/* Put the tray in the bottom-right of the taskbar, above the clock.
   Moving it only when it isn't already there: a move sends another
   ConfigureNotify, and following that blindly would chase itself.
   Raising only when something has covered it, for the same reason —
   IceWM lifts its clock back to the top when it restacks the bar. */
static void place_tray(void)
{
    XWindowAttributes frame, self;
    if (!tray_win || !tray_frame)
        return;
    if (!XGetWindowAttributes(dpy, tray_frame, &frame) || !XGetWindowAttributes(dpy, tray_win, &self))
        return;
    int x = frame.width - tray_pic.w, y = frame.height - tray_pic.h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (self.x != x || self.y != y)
        XMoveWindow(dpy, tray_win, x, y);

    Window r, parent, *kids = NULL;
    unsigned n = 0;
    int covered = 1;
    if (XQueryTree(dpy, tray_frame, &r, &parent, &kids, &n))
        covered = n == 0 || kids[n - 1] != tray_win;
    if (kids)
        XFree(kids);
    if (covered)
        XRaiseWindow(dpy, tray_win);
}

/* Once IceWM has made the taskbar. No Enter/Leave: the main loop treats
   any enter that isn't the hot corner as the Start button. Clicks are
   taken and ignored, so they don't fall through onto a task button. */
static void attach_tray(void)
{
    if (tray_win || !tray_pic.px)
        return;
    tray_frame = taskbar_window();
    if (!tray_frame)
        return;
    paint_tray();
    if (!tray_bg) {
        tray_frame = 0;
        return;
    }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, tray_frame, &wa)) {
        tray_frame = 0;
        return;
    }
    int x = wa.width - tray_pic.w, y = wa.height - tray_pic.h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    /* Override-redirect: the taskbar passes its children's map requests to
       IceWM (substructure redirect), and IceWM ignores one for a window it
       didn't make, so ours would never show. */
    XSetWindowAttributes a = { .background_pixmap = tray_bg, .override_redirect = True,
                               .event_mask = StructureNotifyMask | ButtonPressMask };
    tray_win = XCreateWindow(dpy, tray_frame, x, y, tray_pic.w, tray_pic.h, 0, depth, InputOutput, vis,
                             CWBackPixmap | CWOverrideRedirect | CWEventMask, &a);
    if (!tray_win) {
        tray_frame = 0;
        return;
    }
    XSelectInput(dpy, tray_frame, SubstructureNotifyMask | StructureNotifyMask);
    XMapRaised(dpy, tray_win);
    /* The create can fail after the id comes back (the bar went away).
       Don't keep the id, or we would never try again. */
    XSync(dpy, False);
    if (!XGetWindowAttributes(dpy, tray_win, &wa)) {
        tray_win = 0;
        tray_frame = 0;
        return;
    }
    embed_icon();                   /* one that docked before the bar was there */
}

/* Redraw when the minute changes. */
static void refresh_tray(void)
{
    if (!tray_win || !f_clock)
        return;
    if (time(NULL) / 60 == tray_minute)
        return;
    paint_tray();
}

/* How long to sleep. Until the taskbar appears, look often. Once the tray
   is up, wake at least once a second: IceWM restacks its clock on the
   second, and the tray has to stay above it. Also wake for the new minute,
   which is when the time is drawn again. No tray picture means there is
   nothing to wait for on that side. */
static int nap_ms(void)
{
    if (!start_win || (tray_pic.px && !tray_win))
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
   theme says. (The window overview it is meant to open is not written yet;
   see docs/ROADMAP.md, step 11b.) */

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
    XSetWindowAttributes a = { .event_mask = EnterWindowMask | LeaveWindowMask };
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
}

/* --- the menu, again ------------------------------------------------------------- */

/* Show the menu until something is chosen; returns the entry, or -1. It
   stands on the taskbar, at the left, with Start held down below it. */
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
    if (!grab_input(win)) {
        close_popup(win, bg);
        return -1;
    }
    show_start(START_PRESSED);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    int cur = -1, chosen = -2;      /* -2 still open */
    while (chosen == -2) {
        XEvent ev;
        next_event(&ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                light(win, gc, -1, cur);
            break;
        case MotionNotify: {
            int i = item_at(ev.xmotion.x, ev.xmotion.y);
            if (i != cur)
                light(win, gc, cur, i), cur = i;
            break;
        }
        case ButtonPress:
            /* A click outside the menu (Start included) closes it. A click
               on an entry runs it when the button comes up, so you can
               also press Start, slide up to an entry and let go. */
            if (!inside(&(struct rect){ 0, 0, w, h }, ev.xbutton.x, ev.xbutton.y))
                chosen = -1;
            break;
        case ButtonRelease:
            if (ev.xbutton.button == Button1 && item_at(ev.xbutton.x, ev.xbutton.y) >= 0)
                chosen = item_at(ev.xbutton.x, ev.xbutton.y);
            break;
        case KeyPress: {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            int next = cur;
            if (k == XK_Escape || opens_menu(&ev.xkey))
                chosen = -1;
            else if ((k == XK_Return || k == XK_KP_Enter) && cur >= 0)
                chosen = cur;
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
    XFreeGC(dpy, gc);
    close_popup(win, bg);
    show_start(START_NORMAL);
    return chosen;
}

static void open_menu(void)
{
    int i = menu();
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

/* The desktop's scale, as myos-xsettings decided it: _MYOS_SCALE, or 0
   for "automatic", which is _MYOS_SCALE_AUTO. The theme has pictures for
   100, 125 ... 200%, so it's rounded to the nearest of those. */
static int desktop_scale(void)
{
    long s = root_cardinal(a_scale, 0);
    if (s <= 0)
        s = root_cardinal(a_scale_auto, 100);
    s = (s + 12) / 25 * 25;
    return s < 100 ? 100 : s > 200 ? 200 : (int)s;
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
    XSelectInput(dpy, root, PropertyChangeMask);    /* the scale, the wallpaper */
    scale = desktop_scale();
    snprintf(dir, sizeof dir, "/usr/share/myos/menu/%d/", scale);

    read_conf();
    read_user();
    char spec[96];
    snprintf(spec, sizeof spec, "Trebuchet MS,DejaVu Sans:bold:pixelsize=%d", name_size);
    f_name = XftFontOpenName(dpy, screen, spec);
    snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", type_size);
    f_type = XftFontOpenName(dpy, screen, spec);
    if (clock_size) {
        snprintf(spec, sizeof spec, "Verdana,DejaVu Sans:pixelsize=%d", clock_size);
        f_clock = XftFontOpenName(dpy, screen, spec);
    }
    if (!f_name || !f_type)
        die("no fonts");

    grab_key(XK_Escape, ControlMask);
    grab_key(XK_Super_L, 0);
    grab_key(XK_Super_R, 0);
    make_corner();
    own_tray();

    int fd = ConnectionNumber(dpy);
    for (;;) {
        /* Until IceWM's taskbar exists (it may still be starting, or
           restarting), look for it four times a second. Once the tray is
           up, keep it at the right end and above the clock. */
        if (!XPending(dpy)) {
            if (!start_win)
                attach_start();
            if (!tray_win)
                attach_tray();
            else {
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
            if (ev.xbutton.window == start_win && ev.xbutton.button == Button1)
                open_menu();
            break;
        case MapNotify:
        case ConfigureNotify:
        case ReparentNotify:
            /* IceWM restacks the bar; stay above its flat clock. */
            if (tray_win && tray_frame && ev.xany.window == tray_frame)
                place_tray();
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
            else
                show_start(START_NORMAL);
            break;
        case KeyPress:
            if (opens_menu(&ev.xkey))
                open_menu();
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
        }
    }
}
