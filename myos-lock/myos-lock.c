/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-lock — the lock screen (docs/SLEEP-PLAN.md)
 *
 * What a Mac shows when it wakes or the screen comes back on: the time,
 * and your name with a box for the password. In the login screen's look
 * (live/myos-login): its photograph, darker, between the Golden Hour bands.
 * The password goes to PAM as at the login screen, but as the user:
 * pam_unix asks its setgid helper, unix_chkpwd, to read /etc/shadow. An
 * account without a password opens with Enter or a click.
 *
 * It covers the screen with a window the window manager doesn't manage
 * (override-redirect) and takes the keyboard and the pointer (grabs), so
 * no other program sees a key. A program holding a grab of its own (an
 * open menu) makes it try again every 100 ms; anything drawn over it (a
 * tooltip) makes it raise itself again.
 *
 * myos-idle starts it: when the screen goes off, and when myos-powerd is
 * about to sleep. Then it has --ack and writes "locked" to /run/myos/power
 * as soon as the screen is covered: the photograph is drawn after that,
 * so sleep never waits for a picture to be scaled. One lock at a time: it
 * owns the X selection _MYOS_LOCK, and a second one sees that, answers
 * for it, and exits.
 */
#include <Imlib2.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "powerconf.h"
#include "xpic.h"

#define ART      "/usr/share/myos/login"
#define SERVICE  "myos-lock"
#define DESIGN_H 768                /* the height the login theme was drawn for */
#define PASS_MAX 128

/* The login screen's palette. */
#define BAND_TOP    0x0e2c52
#define BAND_BOTTOM 0x133f63
#define GOLD        0xffc56b
#define TEAL_LIGHT  0xd6f6f7
#define INK         0x1d2430
#define TEAL_RIM    0x2b8a92

static struct {
    Display *dpy;
    int scr;
    Window win;
    Visual *vis;
    Colormap cmap;
    GC gc;
    XftDraw *draw;
    int w, h;
    double s;                       /* screen height / 768 */
    Pixmap bg;                      /* photograph, bands and panel; None until drawn */
    XftFont *f_time, *f_date, *f_name, *f_hint, *f_pass;
    char user[64], name[64];
    char pass[PASS_MAX];
    int passlen;
    int no_password;                /* 1 or 0; -1 until PAM was asked */
    int grabbed;
    const char *note;
} ui;

static int sc(double v)
{
    return (int)(v * ui.s + 0.5);
}

/* Where things go: the clock in the upper third, the panel below it. */
static int panel_w(void) { return sc(420); }
static int panel_h(void) { return sc(130); }
static int panel_x(void) { return (ui.w - panel_w()) / 2; }
static int panel_y(void) { return ui.h * 11 / 20; }

/* --- the picture ------------------------------------------------------------------- */

static void blend(uint32_t *px, int x, int y, uint32_t rgb, int alpha)
{
    if (x < 0 || y < 0 || x >= ui.w || y >= ui.h)
        return;
    uint32_t *d = &px[(size_t)y * ui.w + x], o = *d, out = 0xff000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int src = (rgb >> shift) & 0xff, dst = (o >> shift) & 0xff;
        out |= (uint32_t)(dst + (src - dst) * alpha / 255) << shift;
    }
    *d = out;
}

static void vgrad(uint32_t *px, int y, int rh, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < rh; j++) {
        uint32_t c = 0xff000000u;
        for (int shift = 0; shift <= 16; shift += 8) {
            int a = (top >> shift) & 0xff, b = (bottom >> shift) & 0xff;
            c |= (uint32_t)(a + (b - a) * j / (rh > 1 ? rh - 1 : 1)) << shift;
        }
        for (int i = 0; i < ui.w && y + j < ui.h; i++)
            px[(size_t)(y + j) * ui.w + i] = c;
    }
}

/* The gold line between a band and the photograph, fading at both ends. */
static void gold_line(uint32_t *px, int y)
{
    for (int i = 0; i < ui.w; i++)
        for (int j = 0; j < (sc(2) > 0 ? sc(2) : 1); j++)
            blend(px, i, y + j, GOLD, 255 * (i < ui.w / 2 ? i : ui.w - 1 - i) / (ui.w / 2));
}

/* A rounded rectangle: fill at strength fa, a border `thick` wide. Squared
   distances from each corner's centre, so no square roots. */
static void round_rect(uint32_t *px, int x, int y, int rw, int rh, int r, int thick,
                       uint32_t fill, int fa, uint32_t edge)
{
    int in = r - thick;
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) {
            int cx = i < r ? r - i : i >= rw - r ? i - (rw - r - 1) : 0;
            int cy = j < r ? r - j : j >= rh - r ? j - (rh - r - 1) : 0;
            int d2 = cx * cx + cy * cy;
            if (d2 > r * r)
                continue;
            if (d2 > in * in)
                blend(px, x + i, y + j, edge, 255);
            else
                blend(px, x + i, y + j, fill, fa);
        }
}

static void put_icon(uint32_t *px, const char *name, int x, int y, int size)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s.png", ART, name);
    Imlib_Image img = imlib_load_image(path);
    if (!img)
        return;
    imlib_context_set_image(img);
    Imlib_Image scaled = imlib_create_cropped_scaled_image(
        0, 0, imlib_image_get_width(), imlib_image_get_height(), size, size);
    imlib_free_image();
    if (!scaled)
        return;
    imlib_context_set_image(scaled);
    const uint32_t *src = imlib_image_get_data_for_reading_only();
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++)
            blend(px, x + i, y + j, src[j * size + i] & 0xffffff, (int)(src[j * size + i] >> 24));
    imlib_free_image();
}

/* Everything that doesn't change, composed once in our memory and sent to
   the server as one Pixmap, as the login screen does. */
static void build_background(void)
{
    size_t n = (size_t)ui.w * ui.h;
    uint32_t *px = malloc(n * sizeof *px);
    if (!px)
        return;
    Imlib_Image photo = imlib_load_image(ART "/background.png");
    Imlib_Image img = NULL;
    if (photo) {
        imlib_context_set_image(photo);
        img = imlib_create_cropped_scaled_image(0, 0, imlib_image_get_width(),
                                                imlib_image_get_height(), ui.w, ui.h);
        imlib_free_image();
    }
    if (img) {
        imlib_context_set_image(img);
        memcpy(px, imlib_image_get_data_for_reading_only(), n * sizeof *px);
        imlib_free_image();
        for (size_t k = 0; k < n; k++)  /* 35% darker: the white letters must read */
            px[k] = 0xff000000u | ((px[k] >> 16 & 0xff) * 166 / 255) << 16 |
                    ((px[k] >> 8 & 0xff) * 166 / 255) << 8 | (px[k] & 0xff) * 166 / 255;
    } else
        vgrad(px, 0, ui.h, BAND_TOP, BAND_BOTTOM);
    int top = sc(64), bottom = sc(80);
    vgrad(px, 0, top, BAND_TOP, BAND_BOTTOM);
    vgrad(px, ui.h - bottom, bottom, BAND_BOTTOM, BAND_TOP);
    gold_line(px, top - sc(2));
    gold_line(px, ui.h - bottom);
    put_icon(px, "logo", sc(16), sc(8), sc(48));

    int x = panel_x(), y = panel_y();
    round_rect(px, x, y, panel_w(), panel_h(), sc(10), sc(1.5) > 0 ? sc(1.5) : 1, 0xffffff, 46, GOLD);
    put_icon(px, "user", x + sc(18), y + sc(18), sc(72));
    round_rect(px, x + sc(108), y + sc(78), sc(290), sc(30), sc(4), sc(1.5) > 0 ? sc(1.5) : 1,
               0xffffff, 255, TEAL_RIM);
    ui.bg = xpic_pixmap(ui.dpy, ui.vis, DefaultDepth(ui.dpy, ui.scr), ui.win, px, ui.w, ui.h);
    free(px);
}

/* --- text ---------------------------------------------------------------------------- */

static XftFont *font(const char *spec, double size)
{
    char name[128];
    snprintf(name, sizeof name, "%s:pixelsize=%g", spec, size * ui.s);
    XftFont *f = XftFontOpenName(ui.dpy, ui.scr, name);
    return f ? f : XftFontOpenName(ui.dpy, ui.scr, "DejaVu Sans:pixelsize=16");
}

/* align: 0 left, 1 centred on x. A drop shadow when shadow is set. */
static void text(XftFont *f, uint32_t rgb, int x, int y, const char *s, int align, int shadow)
{
    XGlyphInfo ext;
    XftTextExtentsUtf8(ui.dpy, f, (const FcChar8 *)s, strlen(s), &ext);
    if (align)
        x -= ext.xOff / 2;
    for (int k = shadow ? 0 : 1; k < 2; k++) {
        uint32_t c = k ? rgb : 0;
        XRenderColor rc = { (c >> 16 & 0xff) * 257, (c >> 8 & 0xff) * 257, (c & 0xff) * 257,
                            k ? 0xffff : 204 * 257 };
        XftColor col;
        XftColorAllocValue(ui.dpy, ui.vis, ui.cmap, &rc, &col);
        XftDrawStringUtf8(ui.draw, &col, f, x + !k, y + !k, (const FcChar8 *)s, strlen(s));
        XftColorFree(ui.dpy, ui.vis, ui.cmap, &col);
    }
}

static void redraw(void)
{
    if (ui.bg)
        XCopyArea(ui.dpy, ui.bg, ui.win, ui.gc, 0, 0, ui.w, ui.h, 0, 0);
    else
        XClearWindow(ui.dpy, ui.win);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[16], day[16], month[16], date[48];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    strftime(day, sizeof day, "%A", &tm);
    strftime(month, sizeof month, "%B", &tm);
    snprintf(date, sizeof date, "%s %d %s", day, tm.tm_mday, month);
    text(ui.f_time, 0xffffff, ui.w / 2, ui.h * 3 / 10, hm, 1, 1);
    text(ui.f_date, 0xffffff, ui.w / 2, ui.h * 3 / 10 + sc(44), date, 1, 1);

    int x = panel_x() + sc(108), y = panel_y();
    text(ui.f_name, 0xffffff, x, y + sc(42), ui.name, 0, 1);
    const char *hint = ui.note ? ui.note : ui.no_password > 0 ? "Press Enter or click to unlock"
                     : ui.no_password == 0 ? "Type your password" : NULL;
    if (hint)
        text(ui.f_hint, TEAL_LIGHT, x, y + sc(66), hint, 0, 0);
    char bullets[PASS_MAX * 3 + 1] = "";
    for (int k = 0; k < ui.passlen; k++)
        strcat(bullets, "\u2022");
    if (ui.bg) {
        text(ui.f_pass, INK, x + sc(10), y + sc(99), bullets, 0, 0);
        XGlyphInfo ext;
        XftTextExtentsUtf8(ui.dpy, ui.f_pass, (const FcChar8 *)bullets, strlen(bullets), &ext);
        XSetForeground(ui.dpy, ui.gc, xpic_channel(0x1d, ui.vis->red_mask) |
                       xpic_channel(0x24, ui.vis->green_mask) | xpic_channel(0x30, ui.vis->blue_mask));
        XFillRectangle(ui.dpy, ui.win, ui.gc, x + sc(10) + ext.xOff + 1, y + sc(85),
                       sc(1.5) > 0 ? sc(1.5) : 1, sc(16));
    }
    XFlush(ui.dpy);
}

/* --- the password ------------------------------------------------------------------------ */

static int conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    (void)data;
    struct pam_response *r = calloc(n, sizeof *r);
    if (!r)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup(ui.pass);
    *resp = r;
    return PAM_SUCCESS;
}

static void no_delay(int retval, unsigned usec, void *data)
{
    (void)retval, (void)usec, (void)data;
}

/* Does ui.pass open it? quick: without the delay pam_unix adds after a
   wrong one (for the "is there a password at all" look at the start). */
static int check(int quick)
{
    struct pam_conv conv = { conversation, NULL };
    pam_handle_t *pamh = NULL;
    if (pam_start(SERVICE, ui.user, &conv, &pamh) != PAM_SUCCESS)
        return 0;
    if (quick)
        pam_set_item(pamh, PAM_FAIL_DELAY, (const void *)no_delay);
    int err = pam_authenticate(pamh, 0);
    if (err == PAM_SUCCESS)
        pam_setcred(pamh, PAM_REFRESH_CRED);
    pam_end(pamh, err);
    return err == PAM_SUCCESS;
}

static void forget(void)
{
    memset(ui.pass, 0, sizeof ui.pass);
    ui.passlen = 0;
}

static void try_unlock(void)
{
    ui.note = "Checking\u2026";
    redraw();
    if (check(0)) {
        forget();
        exit(0);                        /* the window goes with us */
    }
    forget();
    ui.note = "Wrong password \u2014 try again";
    redraw();
}

/* --- the screen ----------------------------------------------------------------------------- */

static void grab(void)
{
    if (ui.grabbed)
        return;
    if (XGrabKeyboard(ui.dpy, ui.win, True, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
        return;
    if (XGrabPointer(ui.dpy, ui.win, False, ButtonPressMask, GrabModeAsync, GrabModeAsync,
                     ui.win, None, CurrentTime) != GrabSuccess) {
        XUngrabKeyboard(ui.dpy, CurrentTime);
        return;
    }
    ui.grabbed = 1;
}

static void ack(void)
{
    int fd = open(POWER_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, "locked\n", 7) != 7)
            fprintf(stderr, "myos-lock: myos-powerd did not hear us\n");
        close(fd);
    }
}

static void key(XKeyEvent *k)
{
    char buf[32];
    KeySym ks;
    int n = XLookupString(k, buf, sizeof buf, &ks, NULL);
    if (ks == XK_Return || ks == XK_KP_Enter) {
        try_unlock();
        return;
    }
    if (ks == XK_BackSpace) {
        while (ui.passlen > 0 && (ui.pass[--ui.passlen] & 0xc0) == 0x80)
            ui.pass[ui.passlen] = 0;    /* a whole UTF-8 character */
        ui.pass[ui.passlen] = 0;
    } else if (ks == XK_Escape)
        forget();
    else if (n > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f && ui.passlen + n < PASS_MAX) {
        memcpy(ui.pass + ui.passlen, buf, (size_t)n);
        ui.passlen += n;
    }
    ui.note = NULL;
    redraw();
}

int main(int argc, char **argv)
{
    int want_ack = argc > 1 && !strcmp(argv[1], "--ack");
    signal(SIGCHLD, SIG_DFL);           /* myos-idle ignores it; PAM's helper is waited for */
    if (!(ui.dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "myos-lock: no X display\n");
        return 1;
    }
    Atom sel = XInternAtom(ui.dpy, "_MYOS_LOCK", False);
    if (XGetSelectionOwner(ui.dpy, sel) != None) {     /* locked already */
        if (want_ack)
            ack();
        return 0;
    }
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        fprintf(stderr, "myos-lock: who am I?\n");
        return 1;
    }
    snprintf(ui.user, sizeof ui.user, "%s", pw->pw_name);
    snprintf(ui.name, sizeof ui.name, "%.*s", (int)strcspn(pw->pw_gecos ? pw->pw_gecos : "", ","),
             pw->pw_gecos ? pw->pw_gecos : "");
    if (!ui.name[0])
        snprintf(ui.name, sizeof ui.name, "%s", ui.user);

    ui.scr = DefaultScreen(ui.dpy);
    ui.vis = DefaultVisual(ui.dpy, ui.scr);
    ui.cmap = DefaultColormap(ui.dpy, ui.scr);
    ui.gc = DefaultGC(ui.dpy, ui.scr);
    ui.w = DisplayWidth(ui.dpy, ui.scr);
    ui.h = DisplayHeight(ui.dpy, ui.scr);
    ui.s = (double)ui.h / DESIGN_H;
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = xpic_channel(0x0e, ui.vis->red_mask) | xpic_channel(0x2c, ui.vis->green_mask) |
                            xpic_channel(0x52, ui.vis->blue_mask),
        .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | VisibilityChangeMask |
                      StructureNotifyMask,
    };
    ui.win = XCreateWindow(ui.dpy, RootWindow(ui.dpy, ui.scr), 0, 0, (unsigned)ui.w, (unsigned)ui.h, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XStoreName(ui.dpy, ui.win, "myos-lock");
    XSetSelectionOwner(ui.dpy, sel, ui.win, CurrentTime);
    XMapRaised(ui.dpy, ui.win);
    XEvent e;
    XWindowEvent(ui.dpy, ui.win, StructureNotifyMask, &e);
    while (e.type != MapNotify)
        XWindowEvent(ui.dpy, ui.win, StructureNotifyMask, &e);

    ui.no_password = -1;
    ui.draw = XftDrawCreate(ui.dpy, ui.win, ui.vis, ui.cmap);
    ui.f_time = font("DejaVu Sans", 96);
    ui.f_date = font("DejaVu Sans", 22);
    ui.f_name = font("DejaVu Sans:bold", 22);
    ui.f_hint = font("DejaVu Sans", 13);
    ui.f_pass = font("DejaVu Sans", 15);
    for (int tries = 0; !ui.grabbed && tries < 10; tries++) {
        grab();
        if (!ui.grabbed)
            usleep(50000);
    }
    redraw();
    if (want_ack) {
        XSync(ui.dpy, False);
        usleep(50000);                  /* a compositor's next frame */
        ack();
    }
    build_background();
    ui.no_password = check(1);          /* ui.pass is empty */
    redraw();

    for (;;) {
        while (XPending(ui.dpy)) {
            XNextEvent(ui.dpy, &e);
            if (e.type == Expose && e.xexpose.count == 0)
                redraw();
            else if (e.type == KeyPress)
                key(&e.xkey);
            else if (e.type == ButtonPress && ui.no_password > 0)
                try_unlock();
            else if (e.type == VisibilityNotify && e.xvisibility.state != VisibilityUnobscured)
                XRaiseWindow(ui.dpy, ui.win);
            else if (e.type == ClientMessage)   /* myos-idle: awake again, the clock jumped */
                redraw();
        }
        /* Not holding the keyboard yet: try again soon. Else wake for the
           next minute, to move the clock on. */
        time_t now = time(NULL);
        int ms = ui.grabbed ? (int)(60 - now % 60) * 1000 : 100;
        struct pollfd p = { ConnectionNumber(ui.dpy), POLLIN, 0 };
        if (poll(&p, 1, ms) == 0) {
            grab();
            if (ui.grabbed)
                redraw();
        }
    }
}
