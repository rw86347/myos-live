/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-terminal — Terminal, with the Mac's menu bar.
 *
 * xterm draws the text and runs the shell. This window is the frame around
 * it: the Golden Hour menu bar (the same 22 px bar My Files draws) and the
 * menus Terminal.app has, for the things you can do here:
 *
 *   Shell    New Window, New Window with Profile, Close Window,
 *            Edit Background Color, Reset, Hard Reset
 *   Edit     Copy, Paste, Select All, Clear to Start, Clear Scrollback,
 *            Clear Screen, Show Colors
 *   View     Default Font Size, Bigger, Smaller, Scroll to Top,
 *            Scroll to Bottom, Enter Full Screen
 *   Window   Minimize, Zoom
 *   Help     Terminal Help
 *
 * Show Colors and New Window with Profile list the Mac's profiles (Basic,
 * Homebrew, Pro, and the rest), in the Mac's order, with the Mac's colors.
 * Show Colors repaints this window. New Window with Profile opens another.
 * A few of the Mac's backgrounds are translucent; there is no compositor
 * here, so the color itself is used.
 *
 * Keys follow the Mac, with one change the shell forces. On the Mac, Copy
 * is Command-C and Ctrl-C still stops a program. The Windows key is
 * Command here (Super), and Ctrl+Shift+C is the same shortcut on a keyboard
 * that has no Command key. Ctrl+C, Ctrl+V, Ctrl+A and Ctrl+W are left for
 * the shell (stop, and the line editor). Bigger is Ctrl+Shift+Plus, or
 * Command-Plus.
 *
 * The choice (profile, type size, a background you picked) is kept in
 * ~/.config/myos/terminal.conf. The next Terminal window starts that way.
 * A window opened with a named profile starts as that profile and does not
 * change the saved one until you change something in it.
 *
 *   myos-terminal [--profile NAME] [directory] [-e command...]
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include "scale.h"

#define MENU_H      22
#define ROW_H       24
#define SEP_H       9
#define FACE_MIN    6
#define FACE_MAX    72
#define NPROF       12

#define PANEL_WHITE 0xfbfbf9
#define MENU_BAR    0xefece4
#define BORDER      0xc9c2b2
#define TEXT        0x1d2430
#define TEXT_SOFT   0x5b6573
#define TITLE_EDGE  0x0a4048
#define DISABLED    0x9aa3ad

enum {
    A_END = -1,
    A_SEP = 0,
    A_NEW = 1,
    A_NEW_MENU,
    A_CLOSE,
    A_BG,
    A_RESET,
    A_HARD,
    A_COPY,
    A_PASTE,
    A_ALL,
    A_CLEAR_START,
    A_CLEAR_BACK,
    A_CLEAR,
    A_COLORS,
    A_FONT_DEF,
    A_BIGGER,
    A_SMALLER,
    A_TOP,
    A_BOTTOM,
    A_FULL,
    A_MIN,
    A_ZOOM,
    A_HELP,
    A_PROFILE = 100,            /* 100 + index: this window */
    A_NEWPROF = 200             /* 200 + index: a new window */
};

struct profile {
    const char *name;
    int face;                   /* the Mac's point size for this profile */
    unsigned bg, fg, bold, cursor, sel;
    int blink;
    int underline;              /* Red Sands: an underline, not a block */
    const unsigned *ansi;       /* 16 colors, or NULL for xterm's own */
};

/* Clear Dark and Clear Light name every ANSI color. The older profiles
   leave them alone, and xterm's defaults stand. */
static const unsigned ansi_dark[16] = {
    0x35424C, 0xB45648, 0x6CAA71, 0xC4AC62, 0x6D96B4, 0xBD7BCD, 0x7CCBCD, 0xDEE5EB,
    0x465C6D, 0xDF6C5A, 0x79BE7E, 0xE5C872, 0x67B5ED, 0xD389E5, 0x84DDE0, 0xE5EFF5
};
static const unsigned ansi_light[16] = {
    0x2D3840, 0xB45648, 0x6CAA71, 0xC4AC62, 0x5685A8, 0xAD64BE, 0x69C6C9, 0xC1C8CC,
    0x506573, 0xDF6C5A, 0x79BE7E, 0xE5C872, 0x49A2E1, 0xD389E5, 0x77E1E5, 0xD8E1E7
};

/* Colors copied from this Mac's Terminal.app (its factory profiles). */
static const struct profile profiles[NPROF] = {
    { "Basic",           11, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 0xB3D7FF, 0, 0, NULL },
    { "Clear Dark",      12, 0x212734, 0xE6E6E6, 0xF0F0F0, 0xE6E6E6, 0x334E5E, 0, 0, ansi_dark },
    { "Clear Light",     12, 0xFFFFFF, 0x3A4851, 0x313C44, 0x919191, 0xE5ECF1, 0, 0, ansi_light },
    { "Grass",           12, 0x13773D, 0xFFF0A5, 0xFFB03B, 0x8E2800, 0xB64926, 0, 0, NULL },
    { "Homebrew",        18, 0x000000, 0x28FE14, 0x00F900, 0x38FE27, 0x0C2EEE, 1, 0, NULL },
    { "Man Page",        11, 0xFEF49C, 0x000000, 0x000000, 0x000000, 0xBFB875, 0, 0, NULL },
    { "Novel",           12, 0xDFDBC3, 0x4D2F2D, 0x933A21, 0x3A2322, 0x878563, 0, 0, NULL },
    { "Ocean",           11, 0x2B66C9, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x2986FF, 0, 0, NULL },
    { "Pro",             10, 0x000000, 0xF4F4F4, 0xFFFFFF, 0x606060, 0x525252, 0, 0, NULL },
    { "Red Sands",       11, 0x8E3527, 0xD7C9A7, 0xE6C72B, 0xFFFFFF, 0x3D1916, 0, 1, NULL },
    { "Silver Aerogel",  11, 0x929292, 0x000000, 0xFFFFFF, 0xE0E0E0, 0x787A9C, 0, 0, NULL },
    { "Solid Colors",    11, 0xFFFFFF, 0x000000, 0x000000, 0xCBCBCB, 0xB3D7FF, 0, 0, NULL },
};

struct mitem {
    const char *label;
    const char *keys;
    int act;
};

static const struct mitem M_SHELL[] = {
    { "New Window", "Ctrl+Shift+N", A_NEW },
    { "New Window with Profile", NULL, A_NEW_MENU },
    { "Close Window", "Ctrl+Shift+W", A_CLOSE },
    { NULL, NULL, A_SEP },
    { "Edit Background Color…", NULL, A_BG },
    { NULL, NULL, A_SEP },
    { "Reset", NULL, A_RESET },
    { "Hard Reset", NULL, A_HARD },
    { NULL, NULL, A_END },
};
static const struct mitem M_EDIT[] = {
    { "Copy", "Ctrl+Shift+C", A_COPY },
    { "Paste", "Ctrl+Shift+V", A_PASTE },
    { "Select All", "Ctrl+Shift+A", A_ALL },
    { NULL, NULL, A_SEP },
    { "Clear to Start", NULL, A_CLEAR_START },
    { "Clear Scrollback", NULL, A_CLEAR_BACK },
    { "Clear Screen", NULL, A_CLEAR },
    { NULL, NULL, A_SEP },
    { "Show Colors", NULL, A_COLORS },
    { NULL, NULL, A_END },
};
static const struct mitem M_VIEW[] = {
    { "Default Font Size", "Ctrl+Shift+0", A_FONT_DEF },
    { "Bigger", "Ctrl+Shift++", A_BIGGER },
    { "Smaller", "Ctrl+Shift+-", A_SMALLER },
    { NULL, NULL, A_SEP },
    { "Scroll to Top", NULL, A_TOP },
    { "Scroll to Bottom", NULL, A_BOTTOM },
    { NULL, NULL, A_SEP },
    { "Enter Full Screen", "Ctrl+Shift+F", A_FULL },
    { NULL, NULL, A_END },
};
static const struct mitem M_WINDOW[] = {
    { "Minimize", NULL, A_MIN },
    { "Zoom", NULL, A_ZOOM },
    { NULL, NULL, A_END },
};
static const struct mitem M_HELP[] = {
    { "Terminal Help", NULL, A_HELP },
    { NULL, NULL, A_END },
};
static const struct mitem *const MENUS_ITEMS[] = { M_SHELL, M_EDIT, M_VIEW, M_WINDOW, M_HELP };
static const char *const MENUS[] = { "Shell", "Edit", "View", "Window", "Help" };
#define NMENUS 5

static GtkWidget *win, *menu_w, *term_w;
static Display *dpy;
static double K = 1;
static int desk_scale = 100;
static PangoContext *measure_ctx;
static Atom a_scale, a_scale_auto, a_clip;

static int cur_profile;
static int face, launch_face;
static int use_custom;
static unsigned custom_bg;
static int forced_profile;          /* --profile: don't read the saved choice */
static const char *forced_name;
static const char *start_dir;
static char *cmd_args[64];
static int ncmd;
static int full;

static GPid xterm_pid;
static Window shell_win, vt_win;
static char pts_path[64];
static Time last_time;
static int quitting;

static struct mitem prof_items[NPROF + 1];

struct pop {
    GtkWidget *win, *da;
    const struct mitem *items;
    int n, hover;
    int swatches, ticks;
    double w, h;
};
static struct pop main_pop, sub_pop;
static int menu_open = -1;          /* which menu-bar name is down */
static int sub_for = -1;            /* main_pop row whose submenu is open */

static void save_conf(void);
static void apply_colors(void);
static void font_to(int target);
static void redraw_menu(void) { gtk_widget_queue_draw(menu_w); }

/* --- small drawing ---------------------------------------------------------- */

static void rgb(cairo_t *cr, unsigned c, double a)
{
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
                          (c & 255) / 255.0, a);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void font_options(PangoContext *pc)
{
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pc, fo);
    cairo_font_options_destroy(fo);
}

static PangoLayout *layout(PangoContext *pc, const char *s, double size)
{
    PangoLayout *l = pango_layout_new(pc);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "DejaVu Sans");
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s ? s : "", -1);
    return l;
}

static double text_width(const char *s, double size)
{
    PangoLayout *l = layout(measure_ctx, s, size);
    int w = 0;
    pango_layout_get_pixel_size(l, &w, NULL);
    g_object_unref(l);
    return w;
}

/* y is the baseline, the same way My Files draws its menu. */
static double text_base(cairo_t *cr, double x, double baseline, const char *s, double size, unsigned c)
{
    PangoContext *pc = pango_cairo_create_context(cr);
    font_options(pc);
    PangoLayout *l = layout(pc, s, size);
    int w = 0;
    pango_layout_get_pixel_size(l, &w, NULL);
    rgb(cr, c, 1);
    cairo_move_to(cr, x, baseline - pango_layout_get_baseline(l) / (double)PANGO_SCALE);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    g_object_unref(pc);
    return w;
}

static unsigned contrast(unsigned bg)
{
    int y = (((bg >> 16) & 255) * 3 + ((bg >> 8) & 255) * 6 + (bg & 255)) / 10;
    return y > 140 ? 0x000000 : 0xFFFFFF;
}

static unsigned bg_now(void)
{
    return use_custom ? custom_bg : profiles[cur_profile].bg;
}

/* --- the saved choice ------------------------------------------------------- */

static int find_profile(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < NPROF; i++)
        if (!strcmp(profiles[i].name, name))
            return i;
    return -1;
}

static void conf_path(char *path, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(path, n, "%s/.config/myos/terminal.conf", home ? home : "");
}

static void load_conf(void)
{
    cur_profile = 0;
    face = profiles[0].face;
    use_custom = 0;
    if (forced_profile) {
        int i = find_profile(forced_name);
        if (i >= 0)
            cur_profile = i;
        face = profiles[cur_profile].face;
        return;
    }
    char path[512], line[128];
    conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "profile=", 8)) {
            char *nl = strchr(line + 8, '\n');
            if (nl)
                *nl = 0;
            int i = find_profile(line + 8);
            if (i >= 0)
                cur_profile = i;
        } else if (!strncmp(line, "face=", 5)) {
            int n = atoi(line + 5);
            if (n >= FACE_MIN && n <= FACE_MAX)
                face = n;
        } else if (!strncmp(line, "bg=", 3) && isxdigit((unsigned char)line[3])) {
            custom_bg = (unsigned)strtoul(line + 3, NULL, 16);
            use_custom = 1;
        }
    }
    fclose(f);
}

static void save_conf(void)
{
    char path[512];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config", home ? home : "");
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/.config/myos", home ? home : "");
    mkdir(path, 0755);
    conf_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "profile=%s\nface=%d\n", profiles[cur_profile].name, face);
    if (use_custom)
        fprintf(f, "bg=%06X\n", custom_bg & 0xFFFFFF);
    fclose(f);
}

/* --- talking to xterm ------------------------------------------------------- */

/* An escape written to the slave comes back out the master, which is
   xterm, so xterm acts on it (a color, a font size, a clear). The shell
   never sees it: the shell reads the master, and this is the other
   direction. The fd is closed again at once. Holding it open would keep
   the terminal alive after the shell had gone. */
static int pty_write(const char *s)
{
    if (!pts_path[0])
        return -1;
    int fd = open(pts_path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
    close(fd);
    return off == n ? 0 : -1;
}

static void osc(const char *body)
{
    char buf[160];
    snprintf(buf, sizeof buf, "\033]%s\007", body);
    pty_write(buf);
}

static void color_spec(unsigned c, char *out, size_t n)
{
    snprintf(out, n, "rgb:%02x/%02x/%02x", (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void osc_color(int ps, unsigned c)
{
    char spec[24], body[40];
    color_spec(c, spec, sizeof spec);
    snprintf(body, sizeof body, "%d;%s", ps, spec);
    osc(body);
}

/* The profiles that name their ANSI colors, and a reset for the ones that
   don't, so Clear Dark's blue doesn't linger after a change to Basic. */
static void apply_colors(void)
{
    const struct profile *p = &profiles[cur_profile];
    if (p->ansi) {
        for (int i = 0; i < 16; i++) {
            char spec[24], body[48];
            color_spec(p->ansi[i], spec, sizeof spec);
            snprintf(body, sizeof body, "4;%d;%s", i, spec);
            osc(body);
        }
    } else {
        osc("104");
    }
    osc_color(10, p->fg);
    osc_color(11, bg_now());
    osc_color(12, p->cursor);
    osc_color(17, p->sel);
    osc_color(19, contrast(p->sel));
    gtk_widget_queue_draw(term_w);
}

/* #0 puts the size back to whatever -fs started xterm with. Then one step
   at a time to the size we want. #+1 is one point when a TrueType face is
   in use, which is how xterm was started. */
static void font_to(int target)
{
    if (target < FACE_MIN)
        target = FACE_MIN;
    if (target > FACE_MAX)
        target = FACE_MAX;
    if (!pts_path[0]) {
        face = target;
        return;
    }
    osc("50;#0");
    int at = launch_face;
    while (at < target) {
        osc("50;#+1");
        at++;
    }
    while (at > target) {
        osc("50;#-1");
        at--;
    }
    face = target;
}

/* /proc/<pid>/fd is only /proc/self on this kernel, and that names our
   fds, not xterm's. openpty below tells us the slave's name instead. */

/* xterm's own window, the one -into reparented into ours, and inside it
   the text widget (the wide child of the wide child: the scrollbar is the
   narrow one). Keys and the synthetic Paste go to the text widget. */
static Window widest_child(Window w)
{
    Window root, parent, *kids = NULL, best = 0;
    unsigned n = 0;
    int bestw = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &n))
        return 0;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, kids[i], &a) && a.map_state == IsViewable && a.width > bestw) {
            bestw = a.width;
            best = kids[i];
        }
    }
    if (kids)
        XFree(kids);
    return best;
}

static void find_windows(void)
{
    GdkWindow *gw = gtk_widget_get_window(term_w);
    if (!gw)
        return;
    Window parent = GDK_WINDOW_XID(gw);
    Window top = widest_child(parent);
    if (!top)
        return;
    shell_win = top;
    Window inner = widest_child(top);
    vt_win = inner ? inner : top;
}

static void fit_xterm(void)
{
    if (!shell_win)
        return;
    GdkWindow *gw = gtk_widget_get_window(term_w);
    if (!gw)
        return;
    int w = gdk_window_get_width(gw);
    int h = gdk_window_get_height(gw);
    if (w < 2 || h < 2)
        return;
    GdkDisplay *gd = gdk_display_get_default();
    gdk_x11_display_error_trap_push(gd);
    XResizeWindow(dpy, shell_win, (unsigned)w, (unsigned)h);
    gdk_x11_display_error_trap_pop(gd);
    XFlush(dpy);
}

static void copy_title(void)
{
    if (!shell_win)
        return;
    char *name = NULL;
    if (!XFetchName(dpy, shell_win, &name) || !name)
        return;
    if (name[0] && strcmp(name, "xterm"))
        gtk_window_set_title(GTK_WINDOW(win), name);
    XFree(name);
}

static int fit_idle;
static gboolean fit_idle_cb(gpointer unused)
{
    (void)unused;
    fit_idle = 0;
    fit_xterm();
    return G_SOURCE_REMOVE;
}

static void queue_fit(void)
{
    if (!fit_idle)
        fit_idle = g_idle_add(fit_idle_cb, NULL);
}

static void focus_vt(void)
{
    if (!vt_win || menu_open >= 0)
        return;
    GdkDisplay *gd = gdk_display_get_default();
    gdk_x11_display_error_trap_push(gd);
    XSetInputFocus(dpy, vt_win, RevertToParent, CurrentTime);
    gdk_x11_display_error_trap_pop(gd);
    XFlush(dpy);
}

static int polls;

static gboolean poll_xterm(gpointer unused)
{
    (void)unused;
    if (!shell_win)
        find_windows();
    if (shell_win) {
        queue_fit();
        focus_vt();
        copy_title();
        return G_SOURCE_REMOVE;
    }
    return ++polls > 80 ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

/* After the first find, the same function is the once-a-second title check.
   The pointer distinguishes the two so a missing window doesn't spin. */
static gboolean title_timer(gpointer unused)
{
    (void)unused;
    if (shell_win)
        copy_title();
    return G_SOURCE_CONTINUE;
}

static void hex6(unsigned c, char *out, size_t n)
{
    snprintf(out, n, "#%06X", c & 0xFFFFFF);
}

static int spawn_xterm(void)
{
    gtk_widget_realize(term_w);
    GdkWindow *gw = gtk_widget_get_window(term_w);
    gdk_window_ensure_native(gw);
    char into[32], fs[8], fg[8], bg[8], cr[8], bd[8], sel[8], selfg[8];
    char xrm_bd[48], xrm_hi[48], xrm_ht[56], xrm_blink[32], xrm_ul[40];
    char xrm_ansi[16][40];
    const struct profile *p = &profiles[cur_profile];
    snprintf(into, sizeof into, "%lu", (unsigned long)GDK_WINDOW_XID(gw));
    snprintf(fs, sizeof fs, "%d", face);
    hex6(p->fg, fg, sizeof fg);
    hex6(bg_now(), bg, sizeof bg);
    hex6(p->cursor, cr, sizeof cr);
    hex6(p->bold, bd, sizeof bd);
    hex6(p->sel, sel, sizeof sel);
    hex6(contrast(p->sel), selfg, sizeof selfg);
    snprintf(xrm_bd, sizeof xrm_bd, "*colorBDMode:true");
    char xrm_bdc[40];
    snprintf(xrm_bdc, sizeof xrm_bdc, "*colorBD:%s", bd);
    snprintf(xrm_hi, sizeof xrm_hi, "*highlightColor:%s", sel);
    snprintf(xrm_ht, sizeof xrm_ht, "*highlightTextColor:%s", selfg);
    snprintf(xrm_blink, sizeof xrm_blink, "*cursorBlink:%s", p->blink ? "true" : "false");
    snprintf(xrm_ul, sizeof xrm_ul, "*cursorUnderline:%s", p->underline ? "true" : "false");
    /* Synthetic keys (Paste, Select All, the scroll) are delivered with
       XSendEvent. xterm ignores those unless this is on. The menu is the
       only thing that sends them. */
    static const char xrm_send[] = "*allowSendEvents:true";
    static const char xrm_ops[] = "*allowFontOps:true";
    static const char xrm_trans[] =
        "*VT100.translations:#override\\n"
        "Ctrl Shift <KeyPress> V: insert-selection(CLIPBOARD)\\n"
        "Ctrl Shift <KeyPress> v: insert-selection(CLIPBOARD)\\n"
        "Ctrl Shift <KeyPress> A: select-all()\\n"
        "Ctrl Shift <KeyPress> a: select-all()\\n"
        "Ctrl Shift <KeyPress> Home: scroll-back(100000,line)\\n"
        "Ctrl Shift <KeyPress> End: scroll-forw(100000,line)";

    char *argv[80];
    int n = 0;
    argv[n++] = "xterm";
    argv[n++] = "-into";
    argv[n++] = into;
    argv[n++] = "-title";
    argv[n++] = "Terminal";
    argv[n++] = "-fa";
    argv[n++] = "DejaVu Sans Mono";
    argv[n++] = "-fs";
    argv[n++] = fs;
    argv[n++] = "-fg";
    argv[n++] = fg;
    argv[n++] = "-bg";
    argv[n++] = bg;
    argv[n++] = "-cr";
    argv[n++] = cr;
    argv[n++] = "-xrm";
    argv[n++] = xrm_bdc;
    argv[n++] = "-xrm";
    argv[n++] = xrm_bd;
    argv[n++] = "-xrm";
    argv[n++] = xrm_hi;
    argv[n++] = "-xrm";
    argv[n++] = xrm_ht;
    argv[n++] = "-xrm";
    argv[n++] = xrm_blink;
    argv[n++] = "-xrm";
    argv[n++] = xrm_ul;
    argv[n++] = "-xrm";
    argv[n++] = (char *)xrm_send;
    argv[n++] = "-xrm";
    argv[n++] = (char *)xrm_ops;
    argv[n++] = "-xrm";
    argv[n++] = (char *)xrm_trans;
    if (p->ansi) {
        for (int i = 0; i < 16; i++) {
            char spec[8];
            hex6(p->ansi[i], spec, sizeof spec);
            snprintf(xrm_ansi[i], sizeof xrm_ansi[i], "*color%d:%s", i, spec);
            argv[n++] = "-xrm";
            argv[n++] = xrm_ansi[i];
        }
    }
    if (ncmd > 0) {
        argv[n++] = "-e";
        for (int i = 0; i < ncmd && n < 78; i++)
            argv[n++] = cmd_args[i];
    }
    argv[n] = NULL;

    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &xterm_pid, &err)) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_OK, "Can't start the terminal");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", err->message);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_error_free(err);
        return -1;
    }
    launch_face = face;
    g_child_watch_add(xterm_pid, (GChildWatchFunc)NULL, NULL); /* replaced below */
    return 0;
}

static void send_key(KeySym ks, unsigned state)
{
    if (!vt_win)
        find_windows();
    if (!vt_win)
        return;
    XKeyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.display = dpy;
    ev.window = vt_win;
    ev.root = DefaultRootWindow(dpy);
    ev.time = last_time ? last_time : CurrentTime;
    ev.state = state;
    ev.keycode = XKeysymToKeycode(dpy, ks);
    ev.same_screen = True;
    if (!ev.keycode)
        return;
    GdkDisplay *gd = gdk_display_get_default();
    gdk_x11_display_error_trap_push(gd);
    ev.type = KeyPress;
    XSendEvent(dpy, vt_win, False, KeyPressMask, (XEvent *)&ev);
    ev.type = KeyRelease;
    XSendEvent(dpy, vt_win, False, KeyReleaseMask, (XEvent *)&ev);
    gdk_x11_display_error_trap_pop(gd);
    XFlush(dpy);
}

static int selection_owned(Atom sel)
{
    return XGetSelectionOwner(dpy, sel) != None;
}

static void copy_done(GtkClipboard *cb, const gchar *text, gpointer unused)
{
    (void)cb;
    (void)unused;
    if (!text || !text[0])
        return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
}

static void do_copy(void)
{
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), copy_done, NULL);
}

static void shell_cwd(char *out, size_t n)
{
    snprintf(out, n, "%s", g_get_home_dir() ? g_get_current_dir() : ".");
    char *here = g_get_current_dir();
    if (here) {
        snprintf(out, n, "%s", here);
        g_free(here);
    }
    if (xterm_pid <= 0)
        return;
    /* The shell is xterm's child. Its cwd is the directory a new window
       should open in, which is what the Mac's "same working directory" is. */
    DIR *d = opendir("/proc");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        char path[64], line[256];
        snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int ppid = -1;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PPid:", 5))
                ppid = atoi(line + 5);
        fclose(f);
        if (ppid != (int)xterm_pid)
            continue;
        snprintf(path, sizeof path, "/proc/%s/cwd", e->d_name);
        char link[512];
        ssize_t k = readlink(path, link, sizeof link - 1);
        if (k > 0) {
            link[k] = 0;
            snprintf(out, n, "%s", link);
        }
        break;
    }
    closedir(d);
}

static void new_window(int profile_index)
{
    save_conf();
    char *argv[6];
    int n = 0;
    argv[n++] = "myos-terminal";
    if (profile_index >= 0) {
        argv[n++] = "--profile";
        argv[n++] = (char *)profiles[profile_index].name;
    }
    argv[n] = NULL;
    char dir[512];
    shell_cwd(dir, sizeof dir);
    g_spawn_async(dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void edit_background(void)
{
    GtkWidget *dlg = gtk_color_chooser_dialog_new("Background Color", GTK_WINDOW(win));
    GdkRGBA rgba;
    unsigned c = bg_now();
    rgba.red = ((c >> 16) & 255) / 255.0;
    rgba.green = ((c >> 8) & 255) / 255.0;
    rgba.blue = (c & 255) / 255.0;
    rgba.alpha = 1;
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dlg), &rgba);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dlg), &rgba);
        custom_bg = ((unsigned)(rgba.red * 255 + 0.5) << 16) |
                    ((unsigned)(rgba.green * 255 + 0.5) << 8) |
                    (unsigned)(rgba.blue * 255 + 0.5);
        use_custom = 1;
        osc_color(11, custom_bg);
        gtk_widget_queue_draw(term_w);
        save_conf();
    }
    gtk_widget_destroy(dlg);
}

static void use_profile(int i)
{
    if (i < 0 || i >= NPROF)
        return;
    cur_profile = i;
    use_custom = 0;
    apply_colors();
    font_to(profiles[i].face);
    save_conf();
}

static void help_dialog(void)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "Terminal");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(d),
        "Shell opens windows and sets the colors. Show Colors, in Edit, is the "
        "Mac's list of profiles and repaints this window. Edit Background Color "
        "picks a background of your own.\n\n"
        "Copy is Ctrl+Shift+C and Paste is Ctrl+Shift+V, so Ctrl+C still stops "
        "a program. Command-C and Command-V do the same.\n\n"
        "View makes the type bigger or smaller, and Default Font Size puts it "
        "back to the profile's size.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void quit_app(void)
{
    if (quitting)
        return;
    quitting = 1;
    if (xterm_pid > 0) {
        kill(xterm_pid, SIGTERM);
        xterm_pid = 0;
    }
    gtk_main_quit();
}

static void xterm_gone(GPid pid, gint status, gpointer unused)
{
    (void)status;
    (void)unused;
    g_spawn_close_pid(pid);
    if ((GPid)pid == xterm_pid || xterm_pid == 0) {
        xterm_pid = 0;
        quit_app();
    }
}

static void do_action(int act)
{
    if (!pts_path[0])
        find_pts();
    if (act >= A_NEWPROF && act < A_NEWPROF + NPROF) {
        new_window(act - A_NEWPROF);
        return;
    }
    if (act >= A_PROFILE && act < A_PROFILE + NPROF) {
        use_profile(act - A_PROFILE);
        return;
    }
    switch (act) {
    case A_NEW:
        new_window(-1);
        break;
    case A_CLOSE:
        quit_app();
        break;
    case A_BG:
        edit_background();
        break;
    case A_RESET:
        pty_write("\033[!p");
        break;
    case A_HARD:
        pty_write("\033c");
        apply_colors();
        font_to(face);
        break;
    case A_COPY:
        do_copy();
        break;
    case A_PASTE:
        send_key(XK_v, ControlMask | ShiftMask);
        break;
    case A_ALL:
        send_key(XK_a, ControlMask | ShiftMask);
        break;
    case A_CLEAR_START:
        pty_write("\033[3J\033[1J");
        break;
    case A_CLEAR_BACK:
        pty_write("\033[3J");
        break;
    case A_CLEAR:
        pty_write("\033[2J\033[H");
        break;
    case A_FONT_DEF:
        font_to(profiles[cur_profile].face);
        save_conf();
        break;
    case A_BIGGER:
        font_to(face + 1);
        save_conf();
        break;
    case A_SMALLER:
        font_to(face - 1);
        save_conf();
        break;
    case A_TOP:
        send_key(XK_Home, ControlMask | ShiftMask);
        break;
    case A_BOTTOM:
        send_key(XK_End, ControlMask | ShiftMask);
        break;
    case A_FULL:
        full = !full;
        if (full)
            gtk_window_fullscreen(GTK_WINDOW(win));
        else
            gtk_window_unfullscreen(GTK_WINDOW(win));
        break;
    case A_MIN:
        gtk_window_iconify(GTK_WINDOW(win));
        break;
    case A_ZOOM:
        if (gtk_window_is_maximized(GTK_WINDOW(win)))
            gtk_window_unmaximize(GTK_WINDOW(win));
        else
            gtk_window_maximize(GTK_WINDOW(win));
        break;
    case A_HELP:
        help_dialog();
        break;
    default:
        break;
    }
}

/* --- menus ------------------------------------------------------------------ */

static int is_submenu(int act)
{
    return act == A_NEW_MENU || act == A_COLORS;
}

static const char *item_label(const struct mitem *m)
{
    if (m->act == A_FULL)
        return full ? "Exit Full Screen" : "Enter Full Screen";
    return m->label;
}

static int item_enabled(int act)
{
    if (act == A_COPY)
        return selection_owned(XA_PRIMARY);
    if (act == A_PASTE)
        return selection_owned(a_clip) || selection_owned(XA_PRIMARY);
    return 1;
}

static int item_ticked(const struct pop *p, int act)
{
    return p->ticks && act >= A_PROFILE && act < A_PROFILE + NPROF && act - A_PROFILE == cur_profile;
}

static void fill_profiles(int for_new)
{
    for (int i = 0; i < NPROF; i++) {
        prof_items[i].label = profiles[i].name;
        prof_items[i].keys = NULL;
        prof_items[i].act = (for_new ? A_NEWPROF : A_PROFILE) + i;
    }
    prof_items[NPROF].label = NULL;
    prof_items[NPROF].keys = NULL;
    prof_items[NPROF].act = A_END;
}

static double row_y(const struct pop *p, int i)
{
    double y = 4;
    for (int k = 0; k < i; k++)
        y += p->items[k].label ? ROW_H : SEP_H;
    return y;
}

static int pop_hit(const struct pop *p, double x, double y)
{
    if (x < 0 || x >= p->w || y < 0 || y >= p->h)
        return -1;
    for (int i = 0; i < p->n; i++) {
        double ry = row_y(p, i);
        if (p->items[i].label && y >= ry && y < ry + ROW_H)
            return i;
    }
    return -1;
}

static void draw_tick(cairo_t *cr, double x, double y, unsigned c)
{
    rgb(cr, c, 1);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x, y + 5);
    cairo_line_to(cr, x + 4, y + 9);
    cairo_line_to(cr, x + 11, y);
    cairo_stroke(cr);
}

static void draw_swatch(cairo_t *cr, double x, double y, unsigned bg, unsigned edge)
{
    rrect(cr, x, y, 14, 14, 2);
    rgb(cr, bg, 1);
    cairo_fill_preserve(cr);
    rgb(cr, edge, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static gboolean pop_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w;
    struct pop *p = data;
    cairo_scale(cr, K, K);
    rgb(cr, PANEL_WHITE, 1);
    cairo_paint(cr);
    rgb(cr, TITLE_EDGE, 1);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, p->w - 1, p->h - 1);
    cairo_stroke(cr);
    for (int i = 0; i < p->n; i++) {
        const struct mitem *m = &p->items[i];
        double y = row_y(p, i);
        if (!m->label) {
            rgb(cr, BORDER, 1);
            cairo_move_to(cr, 8, y + 4.5);
            cairo_line_to(cr, p->w - 8, y + 4.5);
            cairo_stroke(cr);
            continue;
        }
        int on = item_enabled(m->act);
        int lit = on && i == p->hover;
        if (lit) {
            cairo_pattern_t *g = cairo_pattern_create_linear(0, y + 1, 0, y + ROW_H - 1);
            cairo_pattern_add_color_stop_rgb(g, 0, 0x3a / 255.0, 0xa7 / 255.0, 0xae / 255.0);
            cairo_pattern_add_color_stop_rgb(g, 1, 0x1f / 255.0, 0x7f / 255.0, 0x86 / 255.0);
            rrect(cr, 3, y + 1, p->w - 6, ROW_H - 2, 3);
            cairo_set_source(cr, g);
            cairo_fill_preserve(cr);
            rgb(cr, 0x17666c, 1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            cairo_pattern_destroy(g);
        }
        unsigned col = lit ? 0xFFFFFF : on ? TEXT : DISABLED;
        const char *lab = item_label(m);
        double tx = 28;
        if (p->ticks || p->swatches) {
            if (item_ticked(p, m->act))
                draw_tick(cr, 8, y + 6, col);
            if (m->act >= A_PROFILE && m->act < A_NEWPROF + NPROF) {
                int idx = m->act >= A_NEWPROF ? m->act - A_NEWPROF : m->act - A_PROFILE;
                if (idx >= 0 && idx < NPROF)
                    draw_swatch(cr, p->ticks ? 26 : 10, y + 5, profiles[idx].bg, profiles[idx].fg);
            }
            tx = p->ticks ? 48 : 32;
        }
        text_base(cr, tx, y + 16, lab, 12, col);
        if (is_submenu(m->act)) {
            rgb(cr, col, 1);
            cairo_set_line_width(cr, 1.4);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            double cx = p->w - 16, cy = y + 8;
            cairo_move_to(cr, cx, cy);
            cairo_line_to(cr, cx + 4, cy + 4);
            cairo_line_to(cr, cx, cy + 8);
            cairo_stroke(cr);
        } else if (m->keys && m->keys[0]) {
            text_base(cr, p->w - 12 - text_width(m->keys, 11), y + 16, m->keys, 11,
                      lit ? 0xFFFFFF : on ? TEXT_SOFT : DISABLED);
        }
    }
    return TRUE;
}

static void close_pop(struct pop *p);

static void open_sub_for(int row);

static void pop_pick(struct pop *p, int i)
{
    if (i < 0 || !p->items[i].label || !item_enabled(p->items[i].act))
        return;
    int act = p->items[i].act;
    if (is_submenu(act)) {
        open_sub_for(i);
        return;
    }
    close_pop(&sub_pop);
    close_pop(&main_pop);
    do_action(act);
}

static gboolean pop_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    struct pop *p = data;
    int h = pop_hit(p, ev->x / K, ev->y / K);
    if (h != p->hover) {
        p->hover = h;
        gtk_widget_queue_draw(p->da);
    }
    if (p == &main_pop) {
        if (h >= 0 && is_submenu(p->items[h].act))
            open_sub_for(h);
        else if (h >= 0)
            close_pop(&sub_pop);
    }
    return TRUE;
}

static gboolean pop_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    struct pop *p = data;
    last_time = ev->time;
    double x = ev->x / K, y = ev->y / K;
    if (pop_hit(p, x, y) < 0) {
        if (ev->type == GDK_BUTTON_PRESS) {
            close_pop(&sub_pop);
            close_pop(&main_pop);
        }
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_RELEASE)
        pop_pick(p, pop_hit(p, x, y));
    return TRUE;
}

static void pop_step(struct pop *p, int dir)
{
    if (p->n <= 0)
        return;
    for (int k = 0; k < p->n; k++) {
        p->hover = (p->hover + dir + p->n) % p->n;
        if (p->items[p->hover].label && item_enabled(p->items[p->hover].act))
            break;
    }
    gtk_widget_queue_draw(p->da);
}

static gboolean pop_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    struct pop *p = data;
    last_time = ev->time;
    switch (ev->keyval) {
    case GDK_KEY_Escape:
        close_pop(&sub_pop);
        close_pop(&main_pop);
        break;
    case GDK_KEY_Down:
        pop_step(p == &sub_pop || sub_pop.win ? &sub_pop : p, 1);
        break;
    case GDK_KEY_Up:
        pop_step(p == &sub_pop || sub_pop.win ? &sub_pop : p, -1);
        break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
        pop_pick(sub_pop.win ? &sub_pop : p, (sub_pop.win ? &sub_pop : p)->hover);
        break;
    case GDK_KEY_Right:
        if (p == &main_pop && p->hover >= 0 && is_submenu(p->items[p->hover].act)) {
            open_sub_for(p->hover);
            pop_step(&sub_pop, 1);
        }
        break;
    case GDK_KEY_Left:
        if (sub_pop.win)
            close_pop(&sub_pop);
        break;
    default:
        break;
    }
    return TRUE;
}

static void close_pop(struct pop *p)
{
    if (!p->win)
        return;
    if (p == &main_pop) {
        gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
        close_pop(&sub_pop);
        menu_open = -1;
        sub_for = -1;
        redraw_menu();
    }
    gtk_widget_destroy(p->win);
    p->win = NULL;
    p->da = NULL;
}

static gboolean pop_mapped(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)ev;
    struct pop *p = data;
    if (p != &main_pop)
        return FALSE;
    gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()), gtk_widget_get_window(w),
                  GDK_SEAT_CAPABILITY_ALL_POINTING | GDK_SEAT_CAPABILITY_KEYBOARD, TRUE, NULL, NULL,
                  NULL, NULL);
    return FALSE;
}

static void open_pop(struct pop *p, const struct mitem *items, int swatches, int ticks,
                     int x_root, int y_root)
{
    close_pop(p);
    p->items = items;
    p->swatches = swatches;
    p->ticks = ticks;
    p->hover = -1;
    double lw = 0, kw = 0;
    for (p->n = 0; items[p->n].act != A_END; p->n++) {
        if (!items[p->n].label)
            continue;
        double a = text_width(item_label(&items[p->n]), 12);
        double b = items[p->n].keys ? text_width(items[p->n].keys, 11) : 0;
        if (a > lw)
            lw = a;
        if (b > kw)
            kw = b;
    }
    double left = (ticks || swatches) ? (ticks ? 48 : 32) : 28;
    p->w = left + lw + (kw ? 24 + kw : 16) + 16;
    if (is_submenu(items[0].act) || (p->n > 1 && items[1].label && is_submenu(&items[1])))
        p->w += 12;
    if (p->w < 180)
        p->w = 180;
    p->h = row_y(p, p->n) + 4;

    p->win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_transient_for(GTK_WINDOW(p->win), GTK_WINDOW(win));
    p->da = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->da, (int)ceil(p->w * K), (int)ceil(p->h * K));
    gtk_container_add(GTK_CONTAINER(p->win), p->da);
    gtk_widget_add_events(p->da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(p->da, "draw", G_CALLBACK(pop_draw), p);
    g_signal_connect(p->da, "motion-notify-event", G_CALLBACK(pop_motion), p);
    g_signal_connect(p->da, "button-press-event", G_CALLBACK(pop_button), p);
    g_signal_connect(p->da, "button-release-event", G_CALLBACK(pop_button), p);
    g_signal_connect(p->win, "key-press-event", G_CALLBACK(pop_key), p);
    if (p == &main_pop)
        g_signal_connect(p->win, "map-event", G_CALLBACK(pop_mapped), p);
    int sw = gdk_screen_get_width(gdk_screen_get_default());
    int sh = gdk_screen_get_height(gdk_screen_get_default());
    if (x_root + (int)(p->w * K) > sw)
        x_root = sw - (int)(p->w * K) - 4;
    if (y_root + (int)(p->h * K) > sh)
        y_root = sh - (int)(p->h * K) - 4;
    if (x_root < 0)
        x_root = 0;
    gtk_window_move(GTK_WINDOW(p->win), x_root, y_root);
    gtk_widget_show_all(p->win);
}

static void open_sub_for(int row)
{
    if (sub_for == row && sub_pop.win)
        return;
    sub_for = row;
    int for_new = main_pop.items[row].act == A_NEW_MENU;
    fill_profiles(for_new);
    int ox, oy;
    gdk_window_get_origin(gtk_widget_get_window(main_pop.da), &ox, &oy);
    int x = ox + (int)(main_pop.w * K) - 4;
    int y = oy + (int)(row_y(&main_pop, row) * K);
    open_pop(&sub_pop, prof_items, 1, !for_new, x, y);
}

static double menu_slot(int i, double *w)
{
    double x = 8;
    for (int k = 0; k < i; k++)
        x += text_width(MENUS[k], 12) + 18;
    if (w)
        *w = text_width(MENUS[i], 12);
    return x;
}

static int menu_at(double x)
{
    for (int i = 0; i < NMENUS; i++) {
        double w, mx = menu_slot(i, &w);
        if (x >= mx - 6 && x < mx + w + 8)
            return i;
    }
    return -1;
}

static void open_menu(int i)
{
    close_pop(&sub_pop);
    close_pop(&main_pop);
    menu_open = i;
    int ox, oy;
    gdk_window_get_origin(gtk_widget_get_window(menu_w), &ox, &oy);
    double w, mx = menu_slot(i, &w);
    open_pop(&main_pop, MENUS_ITEMS[i], 0, 0, ox + (int)((mx - 6) * K), oy + (int)(MENU_H * K));
    redraw_menu();
}

static gboolean menu_draw(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)w;
    (void)unused;
    cairo_scale(cr, K, K);
    rgb(cr, MENU_BAR, 1);
    cairo_paint(cr);
    rgb(cr, BORDER, 1);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, MENU_H - 0.5);
    cairo_line_to(cr, gtk_widget_get_allocated_width(menu_w) / K, MENU_H - 0.5);
    cairo_stroke(cr);
    int hover = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_w), "hover"));
    for (int i = 0; i < NMENUS; i++) {
        double tw, x = menu_slot(i, &tw);
        if (i == menu_open) {
            cairo_pattern_t *g = cairo_pattern_create_linear(0, 2, 0, 21);
            cairo_pattern_add_color_stop_rgb(g, 0, 0x3a / 255.0, 0xa7 / 255.0, 0xae / 255.0);
            cairo_pattern_add_color_stop_rgb(g, 1, 0x1f / 255.0, 0x7f / 255.0, 0x86 / 255.0);
            rrect(cr, x - 6, 2, tw + 14, 19, 3);
            cairo_set_source(cr, g);
            cairo_fill_preserve(cr);
            rgb(cr, 0x17666c, 1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            cairo_pattern_destroy(g);
            text_base(cr, x, 16, MENUS[i], 12, 0xFFFFFF);
        } else {
            if (i == hover) {
                rrect(cr, x - 6, 2, tw + 14, 19, 3);
                rgb(cr, 0xd7ecee, 1);
                cairo_fill_preserve(cr);
                rgb(cr, 0x8fc7cb, 1);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            text_base(cr, x, 16, MENUS[i], 12, TEXT);
        }
    }
    return TRUE;
}

static gboolean menu_motion(GtkWidget *w, GdkEventMotion *ev, gpointer unused)
{
    (void)unused;
    int h = menu_at(ev->x / K);
    int prev = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "hover"));
    if (h != prev) {
        g_object_set_data(G_OBJECT(w), "hover", GINT_TO_POINTER(h));
        gtk_widget_queue_draw(w);
    }
    if (menu_open >= 0 && h >= 0 && h != menu_open)
        open_menu(h);
    return TRUE;
}

static gboolean menu_button(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
    (void)w;
    (void)unused;
    last_time = ev->time;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1)
        return TRUE;
    int h = menu_at(ev->x / K);
    if (h < 0) {
        close_pop(&sub_pop);
        close_pop(&main_pop);
        return TRUE;
    }
    if (h == menu_open)
        close_pop(&main_pop);
    else
        open_menu(h);
    return TRUE;
}

static gboolean menu_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer unused)
{
    (void)ev;
    (void)unused;
    g_object_set_data(G_OBJECT(w), "hover", GINT_TO_POINTER(-1));
    gtk_widget_queue_draw(w);
    return TRUE;
}

/* --- keys ------------------------------------------------------------------- */

static unsigned clean_state(unsigned st)
{
    return st & (unsigned)~(LockMask | Mod2Mask);
}

static int shortcut(KeySym ks, unsigned state)
{
    unsigned st = clean_state(state);
    int ctrl = (st & ControlMask) != 0;
    int shift = (st & ShiftMask) != 0;
    int super = (st & Mod4Mask) != 0;
    int chord = super || (ctrl && shift);          /* Command, or Ctrl+Shift */
    if (!chord)
        return 0;
    if (ks == XK_c) {
        do_copy();
        return 1;
    }
    if (ks == XK_v) {
        do_action(A_PASTE);
        return 1;
    }
    if (ks == XK_a) {
        do_action(A_ALL);
        return 1;
    }
    if (ks == XK_n) {
        do_action(A_NEW);
        return 1;
    }
    if (ks == XK_w) {
        do_action(A_CLOSE);
        return 1;
    }
    if (ks == XK_f) {
        do_action(A_FULL);
        return 1;
    }
    if (ks == XK_KP_Add || ((ks == XK_equal || ks == XK_plus) && shift)) {
        do_action(A_BIGGER);
        return 1;
    }
    if (ks == XK_minus || ks == XK_KP_Subtract) {
        do_action(A_SMALLER);
        return 1;
    }
    if (ks == XK_0 || ks == XK_KP_0) {
        do_action(A_FONT_DEF);
        return 1;
    }
    return 0;
}

static void grab_key(KeySym ks, unsigned mods)
{
    KeyCode c = XKeysymToKeycode(dpy, ks);
    if (!c)
        return;
    Window w = GDK_WINDOW_XID(gtk_widget_get_window(term_w));
    unsigned lock[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (int i = 0; i < 4; i++)
        XGrabKey(dpy, c, mods | lock[i], w, True, GrabModeAsync, GrabModeAsync);
}

static void grab_keys(void)
{
    static const KeySym letters[] = { XK_c, XK_v, XK_a, XK_n, XK_w, XK_f, XK_0, XK_KP_0,
                                      XK_minus, XK_KP_Subtract, XK_KP_Add };
    for (unsigned i = 0; i < sizeof letters / sizeof letters[0]; i++) {
        grab_key(letters[i], ControlMask | ShiftMask);
        grab_key(letters[i], Mod4Mask);
    }
    grab_key(XK_equal, ControlMask | ShiftMask);
    grab_key(XK_equal, Mod4Mask | ShiftMask);
    grab_key(XK_plus, ControlMask | ShiftMask);
    grab_key(XK_plus, Mod4Mask | ShiftMask);
}

static GdkFilterReturn key_filter(GdkXEvent *xev, GdkEvent *ev, gpointer unused)
{
    (void)ev;
    (void)unused;
    XEvent *e = xev;
    if (e->type != KeyPress)
        return GDK_FILTER_CONTINUE;
    last_time = e->xkey.time;
    KeySym ks = XLookupKeysym(&e->xkey, 0);
    if (shortcut(ks, e->xkey.state))
        return GDK_FILTER_REMOVE;
    return GDK_FILTER_CONTINUE;
}

/* --- the window ------------------------------------------------------------- */

static gboolean term_draw(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)w;
    (void)unused;
    rgb(cr, bg_now(), 1);
    cairo_paint(cr);
    return FALSE;
}

static void on_size(GtkWidget *w, GdkRectangle *a, gpointer unused)
{
    (void)w;
    (void)a;
    (void)unused;
    queue_fit();
}

static gboolean on_focus(GtkWidget *w, GdkEventFocus *ev, gpointer unused)
{
    (void)w;
    (void)ev;
    (void)unused;
    focus_vt();
    return FALSE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer unused)
{
    (void)w;
    (void)ev;
    (void)unused;
    quit_app();
    return TRUE;
}

static void update_k(void)
{
    int sf = gtk_widget_get_scale_factor(menu_w ? menu_w : win);
    if (sf < 1)
        sf = 1;
    K = desk_scale / 100.0 / sf;
    if (menu_w)
        gtk_widget_set_size_request(menu_w, -1, (int)ceil(MENU_H * K));
}

static int read_scale(void)
{
    long s = 0;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), a_scale, 0, 1, False, XA_CARDINAL, &type, &fmt,
                           &n, &after, &p) == Success && p && n == 1 && fmt == 32)
        s = *(long *)p;
    if (p)
        XFree(p);
    p = NULL;
    if (!s && XGetWindowProperty(dpy, DefaultRootWindow(dpy), a_scale_auto, 0, 1, False, XA_CARDINAL,
                                 &type, &fmt, &n, &after, &p) == Success && p && n == 1 && fmt == 32)
        s = *(long *)p;
    if (p)
        XFree(p);
    if (!s)
        s = auto_scale(gdk_screen_get_width(gdk_screen_get_default()));
    return clamp_scale((int)s);
}

static GdkFilterReturn root_filter(GdkXEvent *xev, GdkEvent *ev, gpointer unused)
{
    (void)ev;
    (void)unused;
    XEvent *e = xev;
    if (e->type == PropertyNotify && (e->xproperty.atom == a_scale || e->xproperty.atom == a_scale_auto)) {
        int s = read_scale();
        if (s != desk_scale) {
            int w, h;
            gtk_window_get_size(GTK_WINDOW(win), &w, &h);
            double old = K > 0 ? K : 1;
            desk_scale = s;
            update_k();
            gtk_window_resize(GTK_WINDOW(win), (int)(w * K / old), (int)(h * K / old));
            redraw_menu();
        }
    }
    return GDK_FILTER_CONTINUE;
}

static gboolean term_mapped(GtkWidget *w, GdkEvent *ev, gpointer unused)
{
    (void)w;
    (void)ev;
    (void)unused;
    static int started;
    if (started)
        return FALSE;
    started = 1;
    grab_keys();
    gdk_window_add_filter(gtk_widget_get_window(term_w), key_filter, NULL);
    if (spawn_xterm() == 0) {
        g_child_watch_add(xterm_pid, xterm_gone, NULL);
        g_timeout_add(50, poll_xterm, NULL);
        g_timeout_add(1000, title_timer, NULL);
    }
    return FALSE;
}

static void parse_args(int *argc, char ***argv)
{
    char **av = *argv;
    int w = 1;
    for (int i = 1; i < *argc; i++) {
        if (!strcmp(av[i], "--profile") && i + 1 < *argc) {
            forced_name = av[++i];
            forced_profile = 1;
        } else if (!strcmp(av[i], "-e")) {
            while (++i < *argc && ncmd < 63)
                cmd_args[ncmd++] = av[i];
            break;
        } else if (av[i][0] != '-') {
            start_dir = av[i];
        } else {
            av[w++] = av[i];
        }
    }
    av[w] = NULL;
    *argc = w;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    parse_args(&argc, &argv);
    if (start_dir && chdir(start_dir) != 0)
        fprintf(stderr, "myos-terminal: %s: %s\n", start_dir, strerror(errno));
    load_conf();

    gtk_init(&argc, &argv);
    g_set_prgname("myos-terminal");
    g_set_application_name("Terminal");
    dpy = gdk_x11_get_default_xdisplay();
    a_scale = XInternAtom(dpy, "_MYOS_SCALE", False);
    a_scale_auto = XInternAtom(dpy, "_MYOS_SCALE_AUTO", False);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False);

    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *mc = cairo_create(ms);
    measure_ctx = pango_cairo_create_context(mc);
    font_options(measure_ctx);

    desk_scale = read_scale();
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    update_k();
    gtk_window_set_title(GTK_WINDOW(win), "Terminal");
    gtk_window_set_default_size(GTK_WINDOW(win), (int)(680 * desk_scale / 100.0),
                                (int)(440 * desk_scale / 100.0));
    gtk_window_set_icon_from_file(GTK_WINDOW(win), "/usr/share/myos/icons/48/terminal.png", NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    menu_w = gtk_drawing_area_new();
    gtk_widget_set_size_request(menu_w, -1, (int)ceil(MENU_H * K));
    gtk_widget_add_events(menu_w, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_object_set_data(G_OBJECT(menu_w), "hover", GINT_TO_POINTER(-1));
    g_signal_connect(menu_w, "draw", G_CALLBACK(menu_draw), NULL);
    g_signal_connect(menu_w, "motion-notify-event", G_CALLBACK(menu_motion), NULL);
    g_signal_connect(menu_w, "button-press-event", G_CALLBACK(menu_button), NULL);
    g_signal_connect(menu_w, "leave-notify-event", G_CALLBACK(menu_leave), NULL);
    gtk_box_pack_start(GTK_BOX(box), menu_w, FALSE, FALSE, 0);

    term_w = gtk_drawing_area_new();
    gtk_widget_set_app_paintable(term_w, TRUE);
    g_signal_connect(term_w, "draw", G_CALLBACK(term_draw), NULL);
    g_signal_connect(term_w, "size-allocate", G_CALLBACK(on_size), NULL);
    g_signal_connect(term_w, "map-event", G_CALLBACK(term_mapped), NULL);
    gtk_box_pack_start(GTK_BOX(box), term_w, TRUE, TRUE, 0);

    g_signal_connect(win, "focus-in-event", G_CALLBACK(on_focus), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);
    gdk_window_set_events(gdk_get_default_root_window(),
                          gdk_window_get_events(gdk_get_default_root_window()) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(gdk_get_default_root_window(), root_filter, NULL);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
