/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-login — the graphical login screen
 *
 * What xdm, lightdm or gdm do on a Linux desktop, in one small program:
 *   1. start the X server on this machine's screen,
 *   2. draw the login screen and let someone pick a user and type a password,
 *   3. check it with PAM (the same check /bin/login makes on the console),
 *   4. become that user and start their desktop session.
 * When the session ends this program exits, and runit starts it again —
 * which gives the next person a fresh X server, as xdm does.
 *
 * The look is the "Golden Hour" theme (assets/themes/golden-hour): a
 * blurred photograph with a dark band at the top and bottom, the MyOS logo
 * on the left, and the machine's users on the right. The drawing is built
 * from one background picture plus shapes and text at run time, so it is
 * sharp on any screen: the picture is the only thing that gets scaled, and
 * it is blurred anyway. Sizes follow the screen's height (the theme was
 * drawn for 768), so the layout is the same shape on a 1024x768 Pentium 4
 * and on a 1920x1200 one.
 *
 * Speed: the whole still part of the screen — picture, bands, logo, the
 * text that never changes — is composed once in our own memory and sent to
 * the X server as one Pixmap. A keystroke copies that back and redraws only
 * the user tiles, so typing never touches the photograph again. The tile of
 * the selected user (a translucent panel over the photograph) is composed
 * once per user too. Imlib2 only ever decodes the pictures here: the pixels
 * go to the server through XPutImage, because MyOS has no shared memory
 * (see live/overlay/etc/X11/xorg.conf.d/10-myos.conf).
 */
#include <Imlib2.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "xpic.h"

#define ART     "/usr/share/myos/login"
#define SESSION "/etc/myos/xsession"
#define XSERVER "/usr/bin/X"
#define LOGIN_SERVICE "myos-login"

#define DESIGN_H  768               /* the height the theme was drawn for */
#define MAX_USERS 8
#define PASS_MAX  128

static void say(const char *what);      /* a line on the console */

/* The palette, from make-theme.py. */
#define BAND_TOP    0x0e2c52
#define BAND_BOTTOM 0x133f63
#define GOLD        0xffc56b
#define TEAL_LIGHT  0xd6f6f7
#define INK         0x1d2430
#define TEAL_RIM    0x2b8a92

struct user {
    char name[64];
    char gecos[64];
    int has_password;
};

static struct {
    Display *dpy;
    int screen;
    Window win;
    Visual *visual;
    Colormap cmap;
    int w, h;
    double s;                       /* screen height / 768 */
    uint32_t *px;                   /* the whole screen while it is drawn */
    Pixmap still;                   /* the part that never changes */
    Pixmap tile[MAX_USERS];         /* each user's panel, when selected */
    int tile_w, tile_h;
    XftDraw *draw;
    XftFont *f_logo, *f_welcome, *f_name, *f_name2, *f_small, *f_sub, *f_pass;
    struct user users[MAX_USERS];
    int nusers, selected;
    char pass[PASS_MAX];
    int passlen;
    int failed;
    const char *note;               /* something to say instead of the hint */
} ui;

static pid_t xpid;

static void die(const char *what)
{
    fprintf(stderr, "myos-login: %s: %s\n", what, strerror(errno));
    if (xpid > 0)
        kill(xpid, SIGTERM);
    exit(1);
}

static int sc(double v)             /* a design length on this screen */
{
    return (int)(v * ui.s + 0.5);
}

/* --- the users ------------------------------------------------------------------ */

/* Everyone who can log in: a real person (uid 1000 and up, as every
   distribution's useradd hands them out) with a shell that is not a
   refusal. Root is left out on purpose — you reach root with doas. */
static int shell_ok(const char *sh)
{
    if (!sh || !*sh)
        return 0;
    const char *base = strrchr(sh, '/');
    base = base ? base + 1 : sh;
    return strcmp(base, "nologin") && strcmp(base, "false");
}

static void read_users(void)
{
    struct passwd *p;
    setpwent();
    while (ui.nusers < MAX_USERS && (p = getpwent())) {
        if (p->pw_uid < 1000 || p->pw_uid >= 65534 || !shell_ok(p->pw_shell))
            continue;
        struct user *u = &ui.users[ui.nusers++];
        snprintf(u->name, sizeof u->name, "%s", p->pw_name);
        snprintf(u->gecos, sizeof u->gecos, "%s", p->pw_gecos ? p->pw_gecos : "");
        char *comma = strchr(u->gecos, ',');
        if (comma)
            *comma = 0;
        u->has_password = 1;        /* unknown: /etc/shadow is root's to read */
    }
    endpwent();
}

/* Does this user have an empty password? Only root can look, and this
   program runs as root until it hands the session over. */
static void read_passwords(void)
{
    FILE *f = fopen("/etc/shadow", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        for (int i = 0; i < ui.nusers; i++)
            if (!strcmp(ui.users[i].name, line))
                ui.users[i].has_password = colon[1] != ':' && colon[1] != '\n' && colon[1] != 0;
    }
    fclose(f);
}

/* --- drawing into a picture ------------------------------------------------------ */
/* A picture is 0xAARRGGBB, the way Imlib2 hands it over. The background is
   opaque, so a blend is one multiply-add per channel; everything below
   writes into it before it is handed to the X server once. */

static void blend(uint32_t *px, int w, int h, int x, int y, uint32_t rgb, int alpha)
{
    if (x < 0 || y < 0 || x >= w || y >= h || alpha <= 0)
        return;
    uint32_t *d = &px[y * w + x];
    uint32_t o = *d;
    if (alpha >= 255) {
        *d = 0xff000000u | rgb;
        return;
    }
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int src = (rgb >> shift) & 0xff, dst = (o >> shift) & 0xff;
        out |= (uint32_t)(dst + (src - dst) * alpha / 255) << shift;
    }
    *d = out;
}

static void fill_rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh,
                      uint32_t rgb, int alpha)
{
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++)
            blend(px, w, h, x + i, y + j, rgb, alpha);
}

/* One colour fading into another, down the rectangle. */
static void fill_vgrad(uint32_t *px, int w, int h, int x, int y, int rw, int rh,
                       uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < rh; j++) {
        uint32_t c = 0;
        for (int shift = 0; shift <= 16; shift += 8) {
            int a = (top >> shift) & 0xff, b = (bottom >> shift) & 0xff;
            c |= (uint32_t)(a + (b - a) * j / (rh > 1 ? rh - 1 : 1)) << shift;
        }
        for (int i = 0; i < rw; i++)
            blend(px, w, h, x + i, y + j, c, 255);
    }
}

/* One colour at a fading strength, across the rectangle: the panel behind
   the selected user (white, 28% on the left to 6% on the right). */
static void fill_hfade(uint32_t *px, int w, int h, int x, int y, int rw, int rh,
                       uint32_t rgb, int a_left, int a_right)
{
    for (int i = 0; i < rw; i++) {
        int a = a_left + (a_right - a_left) * i / (rw > 1 ? rw - 1 : 1);
        for (int j = 0; j < rh; j++)
            blend(px, w, h, x + i, y + j, rgb, a);
    }
}

/* A line that fades out at both ends: where a band meets the photograph. */
static void gold_line(uint32_t *px, int w, int h, int y, int thick)
{
    for (int i = 0; i < w; i++) {
        int d = i < w / 2 ? i : w - 1 - i;
        int a = 255 * d / (w / 2);
        for (int j = 0; j < thick; j++)
            blend(px, w, h, i, y + j, GOLD, a);
    }
}

/* Is (x,y) inside a rectangle with rounded corners? Used for both the
   panel's edge and its border, so the two always agree. */
static double corner_dist(int x, int y, int rw, int rh, double r)
{
    double cx = x < r ? r : (x > rw - 1 - r ? rw - 1 - r : x);
    double cy = y < r ? r : (y > rh - 1 - r ? rh - 1 - r : y);
    double dx = x - cx, dy = y - cy;
    double d = dx * dx + dy * dy;
    return d > 0 ? __builtin_sqrt(d) : 0;
}

static void rounded_border(uint32_t *px, int w, int h, int x, int y, int rw, int rh,
                           double r, double thick, uint32_t rgb)
{
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) {
            double d = corner_dist(i, j, rw, rh, r);
            int edge = i < thick || j < thick || i >= rw - thick || j >= rh - thick;
            if (d > r)
                continue;                   /* outside the rounded corner */
            if (d > r - thick || (d < 0.5 && edge))
                blend(px, w, h, x + i, y + j, rgb, 255);
        }
}

/* Clear the pixels outside the rounded rectangle back to what `under`
   holds there: the panel must not have square corners. */
static void round_corners(uint32_t *px, const uint32_t *under, int w,
                          int x, int y, int rw, int rh, double r)
{
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++)
            if (corner_dist(i, j, rw, rh, r) > r)
                px[(y + j) * w + x + i] = under[(y + j) * w + x + i];
}

/* An icon from the theme, blended in at the size asked for. */
static void put_icon(uint32_t *px, int w, int h, const char *name, int x, int y, int size)
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
        for (int i = 0; i < size; i++) {
            uint32_t c = src[j * size + i];
            blend(px, w, h, x + i, y + j, c & 0xffffff, (int)(c >> 24));
        }
    imlib_free_image();
}

/* --- handing a picture to the X server -------------------------------------------- */

static Pixmap pixmap_from(const uint32_t *px, int w, int h)
{
    Pixmap p = xpic_pixmap(ui.dpy, ui.visual, DefaultDepth(ui.dpy, ui.screen), ui.win, px, w, h);
    if (!p)
        die("out of memory");
    return p;
}

/* --- text ------------------------------------------------------------------------ */

/* `size` is the height in the drawing, in pixels — pixelsize, not size,
   so the letters come out the size the theme drew them whatever the
   screen says its dots per inch are. */
static XftFont *font(const char *spec, double size)
{
    char name[128];
    snprintf(name, sizeof name, "%s:pixelsize=%g", spec, size * ui.s);
    XftFont *f = XftFontOpenName(ui.dpy, ui.screen, name);
    return f ? f : XftFontOpenName(ui.dpy, ui.screen, "DejaVu Sans:pixelsize=16");
}

static void text(Drawable d, XftFont *f, uint32_t rgb, int alpha, int x, int y,
                 const char *s, int right_align)
{
    XftDraw *draw = XftDrawCreate(ui.dpy, d, ui.visual, ui.cmap);
    XRenderColor rc = { .red = ((rgb >> 16) & 0xff) * 257, .green = ((rgb >> 8) & 0xff) * 257,
                        .blue = (rgb & 0xff) * 257, .alpha = alpha * 257 };
    XftColor col;
    XftColorAllocValue(ui.dpy, ui.visual, ui.cmap, &rc, &col);
    if (right_align) {
        XGlyphInfo ext;
        XftTextExtentsUtf8(ui.dpy, f, (const FcChar8 *)s, strlen(s), &ext);
        x -= ext.xOff;
    }
    XftDrawStringUtf8(draw, &col, f, x, y, (const FcChar8 *)s, strlen(s));
    XftColorFree(ui.dpy, ui.visual, ui.cmap, &col);
    XftDrawDestroy(draw);
}

/* Text with the theme's drop shadow (1 px, black at 80%). */
static void text_shadow(Drawable d, XftFont *f, uint32_t rgb, int x, int y,
                        const char *s, int right_align)
{
    text(d, f, 0x000000, 204, x + 1, y + 1, s, right_align);
    text(d, f, rgb, 255, x, y, s, right_align);
}

/* --- the layout ------------------------------------------------------------------ */
/* Everything is placed against the middle of the screen, where the theme
   puts the divider: the logo to its left, the users to its right. */

static int tile_x(void)   { return ui.w / 2 + sc(37); }
static int tile_y(int i)  { return sc(262) + i * sc(148); }

/* --- the still part of the screen ------------------------------------------------- */
/* Three steps, in this order: the photograph and everything painted on it;
   then each user's tile, cut from that clean picture; then the small
   pictures of the users who are not selected, and the words. */

static void build_background(void)
{
    char path[256];
    snprintf(path, sizeof path, "%s/background.png", ART);
    Imlib_Image photo = imlib_load_image(path);
    if (!photo)
        die("the login background is missing");
    imlib_context_set_image(photo);
    Imlib_Image img = imlib_create_cropped_scaled_image(
        0, 0, imlib_image_get_width(), imlib_image_get_height(), ui.w, ui.h);
    imlib_free_image();
    if (!img)
        die("scaling the background");
    imlib_context_set_image(img);
    int w = ui.w, h = ui.h, top = sc(64), bottom = sc(80);
    ui.px = malloc((size_t)w * h * sizeof *ui.px);
    if (!ui.px)
        die("out of memory");
    memcpy(ui.px, imlib_image_get_data_for_reading_only(), (size_t)w * h * sizeof *ui.px);
    imlib_free_image();
    uint32_t *px = ui.px;

    fill_vgrad(px, w, h, 0, 0, w, top, BAND_TOP, BAND_BOTTOM);
    fill_vgrad(px, w, h, 0, h - bottom, w, bottom, BAND_BOTTOM, BAND_TOP);
    gold_line(px, w, h, top - sc(2), sc(2) > 0 ? sc(2) : 1);
    gold_line(px, w, h, h - bottom, sc(2) > 0 ? sc(2) : 1);

    /* The divider: white, strongest in the middle, gone at both ends. */
    int dx = ui.w / 2, dy0 = sc(170), dy1 = sc(600);
    for (int y = dy0; y < dy1; y++) {
        int mid = (dy0 + dy1) / 2;
        int d = y < mid ? y - dy0 : dy1 - y;
        blend(px, w, h, dx, y, 0xffffff, 150 * d / (mid - dy0));
    }

    /* Left: the logo, the name and the hint. */
    put_icon(px, w, h, "logo", ui.w / 2 - sc(149), sc(262), sc(108));
    /* Bottom band: the power icon. */
    put_icon(px, w, h, "turn-off", sc(30), h - sc(58), sc(36));
}

static void build_still(void)
{
    /* A smaller picture for each user who is not the selected one; the
       selected one's tile is drawn over the top of it. */
    for (int i = 0; i < ui.nusers; i++)
        put_icon(ui.px, ui.w, ui.h, "user", tile_x() + sc(14), tile_y(i), sc(56));

    ui.still = pixmap_from(ui.px, ui.w, ui.h);
    free(ui.px);                    /* the server has it all now */
    ui.px = NULL;

    /* The words, drawn by the font engine straight onto the pixmap. */
    text(ui.still, ui.f_welcome, 0xffffff, 230, ui.w - sc(32), sc(40), "Welcome", 1);
    text_shadow(ui.still, ui.f_logo, 0xffffff, ui.w / 2 - sc(41), sc(410), "MyOS", 1);
    text(ui.still, ui.f_small, TEAL_LIGHT, 255, ui.w / 2 - sc(41), sc(446),
         "To begin, click your user name", 1);
    text_shadow(ui.still, ui.f_small, 0xffffff, sc(76), ui.h - sc(34), "Turn off MyOS", 0);
    text(ui.still, ui.f_sub, TEAL_LIGHT, 255, ui.w - sc(32), ui.h - sc(44),
         "After you log on, you can add or change users.", 1);
    text(ui.still, ui.f_sub, TEAL_LIGHT, 255, ui.w - sc(32), ui.h - sc(26),
         "Go to Settings and click Users.", 1);
}

/* Each user's tile as it looks when chosen: the photograph underneath, a
   translucent panel over it with a golden edge, the user's picture and the
   empty password box. Composed once; a keystroke only copies it back. */
static void build_tiles(void)
{
    ui.tile_w = sc(400);
    ui.tile_h = sc(120);
    double r = sc(10);
    size_t bytes = (size_t)ui.tile_w * ui.tile_h * sizeof *ui.px;
    uint32_t *px = malloc(bytes), *under = malloc(bytes);
    if (!px || !under)
        die("out of memory");
    for (int i = 0; i < ui.nusers; i++) {
        int x = tile_x(), y = tile_y(i);
        if (y + ui.tile_h > ui.h - sc(80))
            break;
        for (int j = 0; j < ui.tile_h; j++)   /* the photograph under this tile */
            memcpy(px + (size_t)j * ui.tile_w, ui.px + (size_t)(y + j) * ui.w + x,
                   (size_t)ui.tile_w * sizeof *px);
        memcpy(under, px, bytes);

        fill_hfade(px, ui.tile_w, ui.tile_h, 0, 0, ui.tile_w, ui.tile_h, 0xffffff, 71, 15);
        rounded_border(px, ui.tile_w, ui.tile_h, 0, 0, ui.tile_w, ui.tile_h, r, sc(1.5), GOLD);
        round_corners(px, under, ui.tile_w, 0, 0, ui.tile_w, ui.tile_h, r);

        put_icon(px, ui.tile_w, ui.tile_h, "user", sc(14), sc(14), sc(72));
        /* The password box and the green arrow that sends it. */
        fill_rect(px, ui.tile_w, ui.tile_h, sc(102), sc(78), sc(220), sc(26), 0xffffff, 255);
        rounded_border(px, ui.tile_w, ui.tile_h, sc(102), sc(78), sc(220), sc(26), sc(3),
                       sc(1.5), TEAL_RIM);
        put_icon(px, ui.tile_w, ui.tile_h, "all-programs", sc(332), sc(76), sc(30));

        ui.tile[i] = pixmap_from(px, ui.tile_w, ui.tile_h);
    }
    free(px);
    free(under);
}

/* --- what changes: the tiles and the typing --------------------------------------- */

static void draw_users(void)
{
    char bullets[PASS_MAX * 3 + 1];
    for (int i = 0; i < ui.nusers; i++) {
        int x = tile_x(), y = tile_y(i);
        struct user *u = &ui.users[i];
        if (i == ui.selected) {
            XCopyArea(ui.dpy, ui.tile[i], ui.win, DefaultGC(ui.dpy, ui.screen),
                      0, 0, ui.tile_w, ui.tile_h, x, y);
            text_shadow(ui.win, ui.f_name, 0xffffff, x + sc(102), y + sc(44), u->name, 0);
            text(ui.win, ui.f_sub, TEAL_LIGHT, 255, x + sc(102), y + sc(66),
                 ui.note      ? ui.note
                 : ui.failed  ? "Wrong password — try again"
                 : u->has_password ? "Type your password"
                                   : "Press Enter to log in", 0);
            int n = ui.passlen < PASS_MAX ? ui.passlen : PASS_MAX;
            bullets[0] = 0;
            for (int k = 0; k < n; k++)
                strcat(bullets, "\u2022");
            text(ui.win, ui.f_pass, INK, 255, x + sc(112), y + sc(97), bullets, 0);
            XGlyphInfo ext;
            XftTextExtentsUtf8(ui.dpy, ui.f_pass, (const FcChar8 *)bullets, strlen(bullets), &ext);
            XSetForeground(ui.dpy, DefaultGC(ui.dpy, ui.screen), 0x1d2430);
            XFillRectangle(ui.dpy, ui.win, DefaultGC(ui.dpy, ui.screen),
                           x + sc(112) + ext.xOff + 1, y + sc(83), sc(1.5) > 0 ? sc(1.5) : 1, sc(16));
        } else {
            XCopyArea(ui.dpy, ui.still, ui.win, DefaultGC(ui.dpy, ui.screen),
                      x, y, ui.tile_w, ui.tile_h, x, y);
            text(ui.win, ui.f_name2, 0xffffff, 217, x + sc(86), y + sc(28), u->name, 0);
            text(ui.win, ui.f_sub, TEAL_LIGHT, 217, x + sc(86), y + sc(46),
                 u->has_password ? "Password required" : "No password", 0);
        }
    }
}

/* Forget what was typed, and any message about the last attempt. */
static void reset_entry(void)
{
    memset(ui.pass, 0, sizeof ui.pass);
    ui.passlen = ui.failed = 0;
    ui.note = NULL;
}

static void redraw(void)
{
    XCopyArea(ui.dpy, ui.still, ui.win, DefaultGC(ui.dpy, ui.screen), 0, 0, ui.w, ui.h, 0, 0);
    draw_users();
    XFlush(ui.dpy);
}

/* --- PAM -------------------------------------------------------------------------- */

static int conversation(int n, const struct pam_message **msg,
                        struct pam_response **resp, void *data)
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

/* Hand the screen to the user: their groups, their uid, their session.
   Returns how many seconds it lasted, when it has finished. */
static long run_session(pam_handle_t *pamh, const struct passwd *pw, char **pamenv)
{
    /* Out of the way: our window is override-redirect, so nothing the
       window manager does would ever cover it. */
    XUnmapWindow(ui.dpy, ui.win);
    XFlush(ui.dpy);
    time_t started = time(NULL);

    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 ||
            setuid(pw->pw_uid) < 0)
            _exit(1);
        if (chdir(pw->pw_dir) < 0 && chdir("/") < 0)
            _exit(1);
        for (char **e = pamenv; e && *e; e++)
            putenv(*e);
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("SHELL", pw->pw_shell, 1);
        /* Alpine's /etc/profile PATH: the sbin folders too, so ifconfig,
           ip and the like are found without typing /sbin/. */
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("DISPLAY", ":0", 1);
        execl("/bin/sh", "sh", SESSION, (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    pam_close_session(pamh, 0);
    (void)status;
    XMapRaised(ui.dpy, ui.win);
    return (long)(time(NULL) - started);
}

/* Returns 1 if the session ran (and has now ended). */
static int try_login(void)
{
    struct user *u = &ui.users[ui.selected];
    struct pam_conv conv = { conversation, NULL };
    pam_handle_t *pamh = NULL;
    ui.note = NULL;
    if (pam_start(LOGIN_SERVICE, u->name, &conv, &pamh) != PAM_SUCCESS)
        return 0;
    pam_set_item(pamh, PAM_TTY, ":0");
    int err = pam_authenticate(pamh, 0);
    if (err == PAM_SUCCESS)
        err = pam_acct_mgmt(pamh, 0);
    if (err != PAM_SUCCESS) {
        pam_end(pamh, err);
        return 0;
    }
    struct passwd *pw = getpwnam(u->name);
    if (!pw) {
        pam_end(pamh, PAM_USER_UNKNOWN);
        return 0;
    }
    pam_setcred(pamh, PAM_ESTABLISH_CRED);
    pam_open_session(pamh, 0);
    say("logged in");
    char **pamenv = pam_getenvlist(pamh);
    long secs = run_session(pamh, pw, pamenv);
    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_end(pamh, PAM_SUCCESS);
    /* A session that ends at once is a broken session, not a finished one:
       stay here and say so, rather than have runit restart us for ever. */
    if (secs < 5) {
        ui.note = "That desktop would not start";
        return 0;
    }
    return 1;
}

/* --- the X server ----------------------------------------------------------------- */

/* Start it and wait for it to answer. X tells its parent it is ready by
   sending SIGUSR1, but only if SIGUSR1 was ignored when it started; we
   simply knock on the door instead, which also covers a slow machine. */
static void start_x(void)
{
    xpid = fork();
    if (xpid < 0)
        die("fork");
    if (xpid == 0) {
        execl(XSERVER, "X", ":0", "vt7", "-nolisten", "tcp", "-nolock",
              "-logfile", "/var/log/Xorg.0.log", (char *)NULL);
        _exit(127);
    }
    setenv("DISPLAY", ":0", 1);
    for (int i = 0; i < 300; i++) {         /* up to 30 seconds */
        int status;
        if (waitpid(xpid, &status, WNOHANG) == xpid) {
            fprintf(stderr, "myos-login: the X server stopped before it was ready\n");
            exit(1);
        }
        ui.dpy = XOpenDisplay(NULL);
        if (ui.dpy)
            return;
        usleep(100000);
    }
    fprintf(stderr, "myos-login: the X server never answered\n");
    kill(xpid, SIGTERM);
    exit(1);
}

static void power_off(void)
{
    if (fork() == 0) {
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        execl("/bin/poweroff", "poweroff", (char *)NULL);
        _exit(127);
    }
}

/* --- putting it together ----------------------------------------------------------- */

static void open_window(void)
{
    ui.screen = DefaultScreen(ui.dpy);
    ui.visual = DefaultVisual(ui.dpy, ui.screen);
    ui.cmap = DefaultColormap(ui.dpy, ui.screen);
    ui.w = DisplayWidth(ui.dpy, ui.screen);
    ui.h = DisplayHeight(ui.dpy, ui.screen);
    ui.s = (double)ui.h / DESIGN_H;

    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixel = BlackPixel(ui.dpy, ui.screen);
    at.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;
    ui.win = XCreateWindow(ui.dpy, RootWindow(ui.dpy, ui.screen), 0, 0, ui.w, ui.h, 0,
                           CopyFromParent, InputOutput, ui.visual,
                           CWOverrideRedirect | CWBackPixel | CWEventMask, &at);
    XDefineCursor(ui.dpy, ui.win, XCreateFontCursor(ui.dpy, XC_left_ptr));
    XStoreName(ui.dpy, ui.win, "MyOS login");
    XMapRaised(ui.dpy, ui.win);

    /* Imlib2 is used to decode and scale pictures, never to draw: it is
       not told about the display at all. */
    imlib_context_set_anti_alias(1);
}

/* Which user tile is at this point, or -1; -2 for "Turn off MyOS". */
static int hit(int x, int y)
{
    if (y > ui.h - sc(80) && x < sc(240))
        return -2;
    for (int i = 0; i < ui.nusers; i++)
        if (x >= tile_x() && x < tile_x() + ui.tile_w &&
            y >= tile_y(i) && y < tile_y(i) + ui.tile_h)
            return i;
    return -1;
}

/* One line on the console (a machine with no serial port shows it last;
   the desktop test waits for these). */
static void say(const char *what)
{
    FILE *con = fopen("/dev/console", "w");
    if (con) {
        fprintf(con, "myos-login: %s\n", what);
        fclose(con);
    }
}

static void event_loop(void)
{
    int shown = 0;
    for (;;) {
        XEvent ev;
        XNextEvent(ui.dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            redraw();
            /* Say so once it is on the screen and takes keys: on a machine
               with no serial port it is the last line before the picture,
               and the desktop test waits for it before pressing Enter. */
            if (!shown++)
                say("the login screen is up");
        } else if (ev.type == ButtonPress) {
            int n = hit(ev.xbutton.x, ev.xbutton.y);
            if (n == -2) {
                power_off();
            } else if (n >= 0 && n != ui.selected) {
                ui.selected = n;
                reset_entry();
                draw_users();
            }
            XSetInputFocus(ui.dpy, ui.win, RevertToPointerRoot, CurrentTime);
        } else if (ev.type == KeyPress) {
            char buf[16];
            KeySym ks;
            int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
            if (ks == XK_Return || ks == XK_KP_Enter) {
                ui.pass[ui.passlen] = 0;
                int ok = try_login();
                memset(ui.pass, 0, sizeof ui.pass);
                ui.passlen = 0;
                if (ok)
                    return;                 /* the session ended: start over */
                ui.failed = ui.note == NULL;
                redraw();                   /* the session drew over us */
            } else if (ks == XK_BackSpace) {
                if (ui.passlen)
                    ui.pass[--ui.passlen] = 0;
                ui.failed = 0;
                ui.note = NULL;
                draw_users();
            } else if (ks == XK_Escape) {
                reset_entry();
                draw_users();
            } else if (ks == XK_Tab || ks == XK_Down || ks == XK_Up) {
                int step = ks == XK_Up ? ui.nusers - 1 : 1;
                ui.selected = (ui.selected + step) % (ui.nusers ? ui.nusers : 1);
                reset_entry();
                draw_users();
            } else if (n == 1 && (unsigned char)buf[0] >= 32 && ui.passlen < PASS_MAX - 1) {
                ui.pass[ui.passlen++] = buf[0];
                ui.failed = 0;
                ui.note = NULL;
                draw_users();
            }
        }
    }
}

int main(void)
{
    read_users();
    read_passwords();
    if (!ui.nusers) {
        fprintf(stderr, "myos-login: no user to log in as (none with uid 1000 or more)\n");
        return 1;
    }
    signal(SIGCHLD, SIG_DFL);
    start_x();
    open_window();

    /* The sizes the theme drew with (assets/themes/golden-hour). */
    ui.f_logo = font("DejaVu Sans:bold:italic", 54);
    ui.f_welcome = font("DejaVu Sans:bold:italic", 22);
    ui.f_name = font("DejaVu Sans:bold", 22);
    ui.f_name2 = font("DejaVu Sans", 20);
    ui.f_small = font("DejaVu Sans", 15);
    ui.f_sub = font("DejaVu Sans", 12);
    ui.f_pass = font("DejaVu Sans", 14);

    build_background();
    build_tiles();
    build_still();
    XSetInputFocus(ui.dpy, ui.win, RevertToPointerRoot, CurrentTime);
    redraw();
    event_loop();

    /* The session has ended. runit starts us again with a fresh server. */
    XCloseDisplay(ui.dpy);
    kill(xpid, SIGTERM);
    waitpid(xpid, NULL, 0);
    return 0;
}
