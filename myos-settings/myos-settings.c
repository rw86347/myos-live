/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-settings — Settings, laid out like the Mac's System Settings.
 *
 * A sidebar on the left (search field, the user's card, then the panes in
 * groups, each with a coloured square icon) and the chosen pane on the
 * right: a title with back/forward arrows, then white rounded boxes of rows,
 * a name on the left and its value or control on the right. Rows with a ">"
 * open a sub-page (General > About), and Back returns.
 *
 * The panes, grouped and ordered as on the Mac: Wi-Fi, Network; General
 * (About, Storage, Date & Time), Displays, Wallpaper; Users & Groups (Add
 * User…, with an administrator's password: myos-adduser.c); Mouse & Keyboard.
 * Kernel: the mode slider, the time slots as a wheel with their tasks'
 * programs, and the power savings as cards (docs/KERNEL-CONFIG.md; saved by
 * myos-kconfig.c into the myos-config partition).
 * Wi-Fi is listed although MyOS can't drive Wi-Fi yet, and says so.
 * Wallpaper is the Mac's page: the picture as it looks now, Fill Screen and
 * the other fits, Add Photo…, then the pictures and a row of colours.
 * Choosing one writes ~/.config/myos/desktop.conf and tells myos-desktop,
 * which paints it (the pixmap has to outlive this window).
 * Displays is where the desktop's size is chosen: five pictures from
 * "Larger Text" to "More Space", as on the Mac; Mouse & Keyboard is where
 * the pointer's size is. Both go to myos-xsettings (see myos-xsettings.c),
 * which makes every program follow.
 *
 * GTK3, because it is already on the disk (AbiWord uses it). All sizes
 * here are in "points at 100%" and multiplied by k, the current scale, so
 * this window grows with the rest of the desktop; fonts grow by themselves
 * (GTK takes their DPI from myos-xsettings).
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <arpa/inet.h>
#include <cpuid.h>
#include <fcntl.h>
#include <gdk/gdkx.h>
#include <grp.h>
#include <gtk/gtk.h>
#include <math.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "kconfig.h"
#include "scale.h"
#include "wallpaper.h"

/* --- the scale ---------------------------------------------------------------- */

static int chosen;          /* the user's choice: a percentage, or 0 = automatic */
static int auto_pct;        /* what automatic means on this screen */
static int screen_w, screen_h;
static double k = 1.0;      /* points at 100% -> pixels GTK draws in */
static Atom a_scale, a_auto;

static int effective(void)
{
    return chosen ? chosen : auto_pct;
}

/* GTK draws whole windows 2x itself at 200% (myos-xsettings says so);
   below that, we scale our own sizes. */
static double k_for(int pct)
{
    return pct >= 200 ? pct / 200.0 : pct / 100.0;
}

#define S(x) ((int)((x) * k + 0.5))

static long root_cardinal(Display *d, Atom a, long dflt)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long v = dflt;
    if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &data) == Success && data && n == 1 && fmt == 32)
        v = *(long *)data;
    if (data)
        XFree(data);
    return v;
}

static void read_scale(void)
{
    Display *d = gdk_x11_get_default_xdisplay();
    a_scale = XInternAtom(d, "_MYOS_SCALE", False);
    a_auto = XInternAtom(d, "_MYOS_SCALE_AUTO", False);
    screen_w = DisplayWidth(d, DefaultScreen(d));
    screen_h = DisplayHeight(d, DefaultScreen(d));
    /* myos-xsettings publishes both; without it (another session), the file. */
    auto_pct = root_cardinal(d, a_auto, auto_scale(screen_w));
    long v = root_cardinal(d, a_scale, -1);
    chosen = v < 0 ? scale_read_conf() : v ? clamp_scale(v) : 0;
    k = k_for(effective());
}

/* --- drawing helpers ------------------------------------------------------------ */

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void rgb(cairo_t *cr, uint32_t c)
{
    cairo_set_source_rgb(cr, (c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0);
}

/* The Mac's settings icons: a white symbol on a coloured rounded square,
   lighter at the top. Drawn, not loaded, so they're sharp at every scale. */
enum icon { IC_NETWORK, IC_GENERAL, IC_DISPLAYS, IC_USERS, IC_ABOUT, IC_STORAGE, IC_CLOCK, IC_ETHERNET,
            IC_MOUSE, IC_WIFI, IC_WALLPAPER, IC_KERNEL };

static const uint32_t icon_colour[] = {
    [IC_NETWORK] = 0x2d7cf6, [IC_GENERAL] = 0x8e8e93, [IC_DISPLAYS] = 0x1c9ef0, [IC_USERS] = 0x5e5ce6,
    [IC_ABOUT] = 0x8e8e93,   [IC_STORAGE] = 0x8e8e93, [IC_CLOCK] = 0x2d7cf6,    [IC_ETHERNET] = 0x2d7cf6,
    [IC_MOUSE] = 0x8e8e93,   [IC_WIFI] = 0x2d7cf6,    [IC_WALLPAPER] = 0x5ac8fa, [IC_KERNEL] = 0x3a5068,
};

static void draw_glyph(cairo_t *cr, enum icon ic, double s)
{
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, s * 0.065);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    switch (ic) {
    case IC_NETWORK:                    /* a globe */
        cairo_arc(cr, s * .5, s * .5, s * .29, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, s * .5, s * .5);
        cairo_scale(cr, .45, 1);
        cairo_arc(cr, 0, 0, s * .29, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .22, s * .5);
        cairo_line_to(cr, s * .78, s * .5);
        cairo_stroke(cr);
        break;
    case IC_GENERAL:                    /* a gear */
        cairo_save(cr);
        cairo_translate(cr, s * .5, s * .5);
        for (int i = 0; i < 8; i++) {
            cairo_rotate(cr, M_PI / 4);
            cairo_rectangle(cr, -s * .065, -s * .33, s * .13, s * .14);
        }
        cairo_fill(cr);
        cairo_arc(cr, 0, 0, s * .23, 0, 2 * M_PI);
        cairo_fill(cr);
        rgb(cr, icon_colour[ic]);
        cairo_arc(cr, 0, 0, s * .09, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        break;
    case IC_DISPLAYS:                   /* a screen on a stand */
        rounded(cr, s * .18, s * .24, s * .64, s * .42, s * .05);
        cairo_fill(cr);
        cairo_rectangle(cr, s * .45, s * .66, s * .10, s * .08);
        cairo_rectangle(cr, s * .34, s * .73, s * .32, s * .05);
        cairo_fill(cr);
        break;
    case IC_USERS:                      /* two people */
        cairo_set_source_rgba(cr, 1, 1, 1, .75);
        cairo_arc(cr, s * .64, s * .36, s * .10, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, s * .64, s * .78, s * .20, M_PI, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_arc(cr, s * .42, s * .38, s * .12, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, s * .42, s * .82, s * .24, M_PI, 2 * M_PI);
        cairo_fill(cr);
        break;
    case IC_ABOUT:                      /* "i" */
        cairo_arc(cr, s * .5, s * .29, s * .065, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_rectangle(cr, s * .445, s * .41, s * .11, s * .34);
        cairo_fill(cr);
        break;
    case IC_STORAGE:                    /* a drive */
        rounded(cr, s * .18, s * .34, s * .64, s * .32, s * .06);
        cairo_fill(cr);
        rgb(cr, icon_colour[ic]);
        cairo_arc(cr, s * .68, s * .5, s * .045, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    case IC_CLOCK:                      /* a clock */
        cairo_arc(cr, s * .5, s * .5, s * .29, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .5, s * .5);
        cairo_line_to(cr, s * .5, s * .32);
        cairo_move_to(cr, s * .5, s * .5);
        cairo_line_to(cr, s * .63, s * .58);
        cairo_stroke(cr);
        break;
    case IC_ETHERNET:                   /* three linked boxes */
        cairo_rectangle(cr, s * .40, s * .20, s * .20, s * .16);
        cairo_rectangle(cr, s * .18, s * .62, s * .20, s * .16);
        cairo_rectangle(cr, s * .62, s * .62, s * .20, s * .16);
        cairo_fill(cr);
        cairo_move_to(cr, s * .5, s * .36);
        cairo_line_to(cr, s * .5, s * .5);
        cairo_move_to(cr, s * .28, s * .62);
        cairo_line_to(cr, s * .28, s * .5);
        cairo_line_to(cr, s * .72, s * .5);
        cairo_line_to(cr, s * .72, s * .62);
        cairo_stroke(cr);
        break;
    case IC_WIFI:                       /* a dot and three arcs fanning up from it */
        cairo_set_line_width(cr, s * .075);
        for (int i = 1; i <= 3; i++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, s * .5, s * .74, s * (.07 + .135 * i), M_PI * 1.25, M_PI * 1.75);
        }
        cairo_stroke(cr);
        cairo_arc(cr, s * .5, s * .72, s * .065, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    case IC_MOUSE:                      /* a mouse, its two buttons parted */
        rounded(cr, s * .32, s * .18, s * .36, s * .64, s * .18);
        cairo_fill(cr);
        rgb(cr, icon_colour[ic]);
        cairo_set_line_width(cr, s * .04);
        cairo_move_to(cr, s * .5, s * .18);
        cairo_line_to(cr, s * .5, s * .42);
        cairo_move_to(cr, s * .32, s * .42);
        cairo_line_to(cr, s * .68, s * .42);
        cairo_stroke(cr);
        break;
    case IC_KERNEL:                     /* a chip: a square with pins on every side */
        rounded(cr, s * .3, s * .3, s * .4, s * .4, s * .05);
        cairo_stroke(cr);
        cairo_rectangle(cr, s * .41, s * .41, s * .18, s * .18);
        cairo_fill(cr);
        for (int i = 0; i < 3; i++) {
            double p = s * (.38 + .12 * i);
            cairo_move_to(cr, p, s * .3), cairo_line_to(cr, p, s * .19);
            cairo_move_to(cr, p, s * .7), cairo_line_to(cr, p, s * .81);
            cairo_move_to(cr, s * .3, p), cairo_line_to(cr, s * .19, p);
            cairo_move_to(cr, s * .7, p), cairo_line_to(cr, s * .81, p);
        }
        cairo_stroke(cr);
        break;
    case IC_WALLPAPER:                  /* a sun and two hills, the Mac's picture */
        cairo_arc(cr, s * .68, s * .32, s * .08, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_move_to(cr, s * .16, s * .76);
        cairo_line_to(cr, s * .40, s * .46);
        cairo_line_to(cr, s * .54, s * .60);
        cairo_line_to(cr, s * .68, s * .42);
        cairo_line_to(cr, s * .86, s * .76);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    }
}

static gboolean draw_icon(GtkWidget *w, cairo_t *cr, gpointer data)
{
    enum icon ic = GPOINTER_TO_INT(data);
    double s = gtk_widget_get_allocated_height(w);
    uint32_t c = icon_colour[ic];
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, s);
    cairo_pattern_add_color_stop_rgb(p, 0, fmin(1, (c >> 16 & 255) / 255.0 + .12),
                                     fmin(1, (c >> 8 & 255) / 255.0 + .12), fmin(1, (c & 255) / 255.0 + .12));
    cairo_pattern_add_color_stop_rgb(p, 1, (c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0);
    rounded(cr, 0, 0, s, s, s * .23);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    draw_glyph(cr, ic, s);
    return TRUE;
}

static GtkWidget *icon_new(enum icon ic, int pts)
{
    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_size_request(w, S(pts), S(pts));
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    g_signal_connect(w, "draw", G_CALLBACK(draw_icon), GINT_TO_POINTER(ic));
    return w;
}

/* A round picture with the person's initial, like the Mac's account icon. */
static gboolean draw_avatar(GtkWidget *w, cairo_t *cr, gpointer data)
{
    const char *name = data;
    double s = gtk_widget_get_allocated_height(w);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, s);
    cairo_pattern_add_color_stop_rgb(p, 0, .66, .69, .74);
    cairo_pattern_add_color_stop_rgb(p, 1, .52, .55, .60);
    cairo_arc(cr, s / 2, s / 2, s / 2, 0, 2 * M_PI);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    char initial[2] = {(char)g_ascii_toupper(name[0]), 0};
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, s * .48);
    cairo_text_extents(cr, initial, &ext);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (s - ext.width) / 2 - ext.x_bearing, (s - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(cr, initial);
    return TRUE;
}

static void free_data(gpointer data, GClosure *unused)
{
    (void)unused;
    g_free(data);
}

static GtkWidget *avatar_new(const char *name, int pts)
{
    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_size_request(w, S(pts), S(pts));
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    g_signal_connect_data(w, "draw", G_CALLBACK(draw_avatar), g_strdup(name), free_data, 0);
    return w;
}

/* --- the look ----------------------------------------------------------------------
   The Mac's light appearance: a grey sidebar, a lighter pane, white boxes
   with hairline borders, the system blue for the selection. Font sizes are
   points (GTK scales them by the DPI); everything else is pixels times k. */
static GtkCssProvider *css;

static void apply_css(void)
{
    char *s = g_strdup_printf(
        "* { font-family: 'DejaVu Sans'; }\n"
        "window.settings, .pane { background-color: #f5f5f7; }\n"
        ".sidebar { background-color: #e8e7e9; border-right: 1px solid #d5d4d7; }\n"
        ".sidebar list { background-color: transparent; }\n"
        ".sidebar row { padding: %dpx %dpx; margin: 0 %dpx; border-radius: %dpx; color: #1d1d1f; }\n"
        ".sidebar row:selected { background-color: #0a64d6; color: white; }\n"
        ".sidebar row.gap { min-height: %dpx; padding: 0; }\n"
        ".sidebar row.gap:hover { background-color: transparent; }\n"
        ".sidebar .subtitle { font-size: 8pt; color: #6e6e73; }\n"
        ".sidebar row:selected .subtitle { color: rgba(255,255,255,0.8); }\n"
        ".sidebar entry { margin: %dpx %dpx %dpx %dpx; min-height: %dpx; border-radius: %dpx;"
        "  background-color: rgba(0,0,0,0.06); border: none; box-shadow: none; }\n"
        ".header { padding: %dpx %dpx; background-color: #f5f5f7; border-bottom: 1px solid #e3e3e6; }\n"
        ".header .title { font-weight: bold; font-size: 11pt; }\n"
        ".header button { padding: 0 %dpx; min-width: %dpx; min-height: %dpx; border: none;"
        "  background: none; box-shadow: none; font-size: 14pt; color: #3c3c43; }\n"
        ".header button:disabled { color: #c7c7cc; }\n"
        ".header button:hover:not(:disabled) { background-color: rgba(0,0,0,0.06); border-radius: %dpx; }\n"
        "list.group { background-color: white; border: 1px solid #e3e3e6; border-radius: %dpx; }\n"
        "list.group row { padding: %dpx %dpx; min-height: %dpx; border-radius: %dpx; }\n"
        "list.group row:hover { background-color: transparent; }\n"
        "list.group row.link:hover { background-color: rgba(0,0,0,0.035); }\n"
        "list.group separator { background-color: #ececee; margin: 0 %dpx; }\n"
        ".value { color: #6e6e73; }\n"
        ".chevron { color: #aeaeb2; font-size: 12pt; }\n"
        ".heading { font-weight: bold; font-size: 15pt; }\n"
        ".blurb { color: #6e6e73; }\n"
        ".section { font-weight: bold; color: #1d1d1f; }\n"
        ".footnote { color: #6e6e73; font-size: 8pt; }\n"
        ".connected { color: #28a745; } .disconnected { color: #d70015; } .error { color: #d70015; }\n"
        "button.thumb { padding: %dpx; border: none; background: none; box-shadow: none; }\n"
        ".thumb-label { font-size: 8pt; color: #6e6e73; }\n"
        ".thumb-label.selected { color: #1d1d1f; font-weight: bold; }\n"
        ".bar-used { background-color: #0a84ff; border-radius: %dpx; min-height: %dpx; }\n"
        ".bar-free { background-color: #e5e5ea; border-radius: %dpx; min-height: %dpx; }\n",
        S(5), S(8), S(10), S(6), S(12), S(10), S(10), S(4), S(10), S(26), S(7), S(10), S(16), S(6),
        S(26), S(26), S(6), S(10), S(9), S(12), S(30), S(10), S(12), S(2), S(3), S(10), S(3), S(10));
    /* Settings › Kernel (mockup B, docs/KERNEL-CONFIG.md). */
    char *kcss = g_strdup_printf(
        ".kbox { background-color: white; border: 1px solid #e3e3e6; border-radius: %dpx; padding: %dpx %dpx; }\n"
        ".kcard { border: 1px solid #dcdad4; border-radius: %dpx; padding: %dpx; background-color: white; }\n"
        ".kcard.on { border-color: #9ad3ab; background-color: #f6fcf7; }\n"
        ".kcard.off { background-color: #f6f5f2; }\n"
        ".kcard.off .kcard-title { color: #8a93a0; }\n"
        ".kcard-title { font-weight: bold; font-size: 9pt; }\n"
        ".kwhy { color: #c77700; font-size: 8pt; }\n"
        ".kmeta { color: #6e6e73; font-size: 7.5pt; }\n"
        ".knum { font-weight: bold; color: #6e6e73; }\n"
        ".kmode { color: #6e6e73; } .kmode-on { color: #1d1d1f; font-weight: bold; }\n"
        "entry.kos { color: #1b6b78; font-weight: bold; background-color: #eef4f5; }\n"
        "entry.kbad { background-color: #fff3f3; border-color: #d70015; }\n"
        "entry.kmono { font-family: 'DejaVu Sans Mono'; font-size: 9pt; }\n"
        ".ktask { border-top: 1px solid #ececee; padding-top: %dpx; }\n",
        S(10), S(10), S(14), S(8), S(8), S(7));
    char *all = g_strconcat(s, kcss, NULL);
    gtk_css_provider_load_from_data(css, all, -1, NULL);
    g_free(all);
    g_free(kcss);
    g_free(s);
}

/* --- page building blocks ------------------------------------------------------------ */

static GtkWidget *label_new(const char *text, const char *cls, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    if (cls)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void separators(GtkListBoxRow *row, GtkListBoxRow *before, gpointer unused)
{
    (void)unused;
    if (before && !gtk_list_box_row_get_header(row))
        gtk_list_box_row_set_header(row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

static void go(const char *id, gboolean record);

static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer unused)
{
    (void)box, (void)unused;
    const char *target = g_object_get_data(G_OBJECT(row), "target");
    if (target)
        go(target, TRUE);
}

/* A white rounded box of rows, with an optional bold title above it. */
static GtkWidget *group_new(GtkWidget *page, const char *title)
{
    if (title) {
        GtkWidget *t = label_new(title, "section", 0);
        gtk_widget_set_margin_start(t, S(4));
        gtk_box_pack_start(GTK_BOX(page), t, FALSE, FALSE, 0);
    }
    GtkWidget *lb = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(lb), separators, NULL, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(lb), "group");
    g_signal_connect(lb, "row-activated", G_CALLBACK(row_activated), NULL);
    gtk_box_pack_start(GTK_BOX(page), lb, FALSE, FALSE, 0);
    return lb;
}

static GtkWidget *row_add(GtkWidget *group, GtkWidget *content)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), content);
    gtk_container_add(GTK_CONTAINER(group), row);
    return row;
}

/* "Name ............ value" */
static GtkWidget *row_value(GtkWidget *group, const char *name, const char *value)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(12));
    gtk_box_pack_start(GTK_BOX(h), label_new(name, NULL, 0), FALSE, FALSE, 0);
    GtkWidget *v = label_new(value, "value", 1);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_box_pack_end(GTK_BOX(h), v, TRUE, TRUE, 0);
    row_add(group, h);
    return v;
}

/* An icon, a name (and a second line), and a ">" that opens another page. */
static GtkWidget *row_link(GtkWidget *group, int ic, const char *name, const char *detail,
                           GtkWidget *right, const char *target)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
    if (ic >= 0)
        gtk_box_pack_start(GTK_BOX(h), icon_new(ic, 22), FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), label_new(name, NULL, 0), FALSE, FALSE, 0);
    if (detail)
        gtk_box_pack_start(GTK_BOX(v), label_new(detail, "footnote", 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (target)
        gtk_box_pack_end(GTK_BOX(h), label_new("\u203a", "chevron", 1), FALSE, FALSE, 0);
    if (right)
        gtk_box_pack_end(GTK_BOX(h), right, FALSE, FALSE, 0);
    GtkWidget *row = row_add(group, h);
    if (target) {
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "target", g_strdup(target), g_free);
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "link");
    }
    return row;
}

/* The big icon, name and sentence at the top of a pane, as on the Mac. */
static void banner(GtkWidget *page, enum icon ic, const char *title, const char *text)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(6));
    GtkWidget *i = icon_new(ic, 52);
    gtk_widget_set_halign(i, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), i, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), label_new(title, "heading", .5), FALSE, FALSE, 0);
    GtkWidget *b = label_new(text, "blurb", .5);
    gtk_label_set_justify(GTK_LABEL(b), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(b), 52);
    gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), v, FALSE, FALSE, S(4));
}

/* --- Network ---------------------------------------------------------------------------
   MyOS has one network card and no interface ioctls yet, so this asks the
   question that matters: which address does the system use to reach the
   world? (A UDP "connect" sends nothing; it only picks the route.) */
static int our_address(char *out, size_t size)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53)};
    socklen_t len = sizeof a;
    int ok = 0;
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(s, (struct sockaddr *)&a, &len) == 0 && a.sin_addr.s_addr) {
        inet_ntop(AF_INET, &a.sin_addr, out, size);
        ok = 1;
    }
    if (s >= 0)
        close(s);
    return ok;
}

static void dns_servers(char *out, size_t size)
{
    char line[256];
    out[0] = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f)
        f = fopen("/proc/net/dns", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if (!strncmp(p, "nameserver", 10))
            p += 10;
        p += strspn(p, " \t");
        p[strcspn(p, " \t\n#")] = 0;
        if (*p && g_ascii_isdigit(*p) && strlen(out) + strlen(p) + 3 < size) {
            if (out[0])
                strcat(out, ", ");
            strcat(out, p);
        }
    }
    fclose(f);
}

static GtkWidget *status_dot(int up)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(5));
    gtk_box_pack_start(GTK_BOX(h), label_new("\u25cf", up ? "connected" : "disconnected", 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), label_new(up ? "Connected" : "Not connected", "value", 0), FALSE, FALSE, 0);
    return h;
}

/* --- Wi-Fi ------------------------------------------------------------------------------
   The kernel lists its Wi-Fi cards in /proc/net/wireless, as Linux does: two
   header lines, then one per card, "wlan0: 0000   60.  -50.  -256 ...". While
   the card is connected, the link quality (0-70) and the signal in dBm come
   marked with a '.'. Choosing a network is the Wi-Fi icon's job, in the
   panel (myos-wifi, through wpa_supplicant); this pane says what there is. */
struct wifi_state {
    char card[32];
    int connected, quality, dbm;
};

static int wifi_card(struct wifi_state *w)
{
    char line[256];
    int n = 0, found = 0;
    memset(w, 0, sizeof *w);
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f))
        if (++n > 2 && strchr(line, ':')) {
            char *p = line + strspn(line, " "), *colon = strchr(p, ':');
            unsigned status;
            char qmark = ' ', lmark = ' ';
            *colon = 0;
            snprintf(w->card, sizeof w->card, "%s", p);
            w->connected = sscanf(colon + 1, " %x %d%c %d%c", &status, &w->quality, &qmark,
                                  &w->dbm, &lmark) == 5 && qmark == '.';
            found = 1;
        }
    fclose(f);
    return found;
}

static void page_wifi(GtkWidget *page)
{
    struct wifi_state w;
    int have = wifi_card(&w);
    char *s = !have ? g_strdup("No Wi-Fi hardware found")
            : w.connected ? g_strdup_printf("%s: signal %d%% (%d dBm)", w.card,
                                            w.quality * 100 / 70, w.dbm)
            : g_strdup_printf("%s", w.card);
    GtkWidget *g = group_new(page, NULL);
    row_link(g, IC_WIFI, "Wi-Fi", s,
             have ? status_dot(w.connected) : label_new("Not available", "value", 1), NULL);
    g_free(s);
    if (have)
        gtk_box_pack_start(GTK_BOX(page),
                           label_new("To choose a network, click the Wi-Fi icon in the panel.",
                                     "footnote", 0),
                           FALSE, FALSE, 0);
}

static void page_network(GtkWidget *page)
{
    char ip[64];
    int up = our_address(ip, sizeof ip);
    GtkWidget *g = group_new(page, NULL);
    row_link(g, IC_ETHERNET, "Ethernet", NULL, status_dot(up), "ethernet");
    struct wifi_state w;
    row_link(g, IC_WIFI, "Wi-Fi", NULL,
             wifi_card(&w) ? status_dot(w.connected) : label_new("Not available", "value", 1),
             "wifi");
}

static void page_ethernet(GtkWidget *page)
{
    char ip[64], dns[256];
    int up = our_address(ip, sizeof ip);
    dns_servers(dns, sizeof dns);
    GtkWidget *g = group_new(page, NULL);
    row_link(g, IC_ETHERNET, "Ethernet", NULL, status_dot(up), NULL);
    g = group_new(page, "TCP/IP");
    row_value(g, "Configure IPv4", "Using DHCP");
    row_value(g, "IP address", up ? ip : "\u2014");
    g = group_new(page, "DNS");
    row_value(g, "DNS servers", dns[0] ? dns : "\u2014");
}

/* --- General -------------------------------------------------------------------------- */

static void page_general(GtkWidget *page)
{
    banner(page, IC_GENERAL, "General", "Information about this computer, its storage, and the date and time.");
    GtkWidget *g = group_new(page, NULL);
    row_link(g, IC_ABOUT, "About", NULL, NULL, "about");
    row_link(g, IC_STORAGE, "Storage", NULL, NULL, "storage");
    g = group_new(page, NULL);
    row_link(g, IC_CLOCK, "Date & Time", NULL, NULL, "datetime");
}

/* No /proc/cpuinfo yet: ask the processor itself. */
static void cpu_name(char *out, size_t size)
{
    unsigned r[12] = {0}, max = __get_cpuid_max(0x80000000, NULL);
    if (max >= 0x80000004) {
        __get_cpuid(0x80000002, &r[0], &r[1], &r[2], &r[3]);
        __get_cpuid(0x80000003, &r[4], &r[5], &r[6], &r[7]);
        __get_cpuid(0x80000004, &r[8], &r[9], &r[10], &r[11]);
        char *s = (char *)r;
        s[47] = 0;
        snprintf(out, size, "%s", g_strstrip(s));
        return;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    char vendor[13] = {0};
    __get_cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4), memcpy(vendor + 4, &d, 4), memcpy(vendor + 8, &c, 4);
    __get_cpuid(1, &a, &b, &c, &d);
    int family = a >> 8 & 15, model = a >> 4 & 15;
    const char *name = !strcmp(vendor, "GenuineIntel") && family == 6 && model >= 7 && model <= 11
                           ? "Intel Pentium III"
                       : !strcmp(vendor, "GenuineIntel") && family == 15 ? "Intel Pentium 4"
                                                                          : NULL;
    if (name)
        snprintf(out, size, "%s", name);
    else
        snprintf(out, size, "%s, family %d, model %d", vendor, family, model);
}

static void page_about(GtkWidget *page)
{
    struct utsname u;
    struct sysinfo si;
    char cpu[64], mem[32], res[32], kern[160];
    uname(&u);
    sysinfo(&si);
    cpu_name(cpu, sizeof cpu);
    snprintf(mem, sizeof mem, "%llu MB", (unsigned long long)si.totalram * si.mem_unit >> 20);
    snprintf(res, sizeof res, "%d \u00d7 %d", screen_w, screen_h);
    snprintf(kern, sizeof kern, "%s %s", u.sysname, u.release);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
    GtkWidget *i = icon_new(IC_DISPLAYS, 84);
    gtk_widget_set_halign(i, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), i, FALSE, FALSE, S(6));
    gtk_box_pack_start(GTK_BOX(v), label_new("MyOS", "heading", .5), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), label_new(u.version, "blurb", .5), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), v, FALSE, FALSE, S(6));

    GtkWidget *g = group_new(page, NULL);
    row_value(g, "Name", u.nodename);
    row_value(g, "Processor", cpu);
    row_value(g, "Memory", mem);
    row_value(g, "Screen", res);
    row_value(g, "Kernel", kern);
    row_value(g, "Architecture", u.machine);
}

static void storage_row(GtkWidget *g, const char *path, const char *name)
{
    struct statvfs st;
    if (statvfs(path, &st) || !st.f_blocks)
        return;
    double total = (double)st.f_blocks * st.f_frsize, avail = (double)st.f_bavail * st.f_frsize;
    double used = total - (double)st.f_bfree * st.f_frsize;
    char *top = g_strdup_printf("%.1f GB available of %.1f GB", avail / 1e9, total / 1e9);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
    gtk_box_pack_start(GTK_BOX(h), label_new(name, "section", 0), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), label_new(top, "value", 1), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    /* The bar: a blue part for what's used, grey for the rest. */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
    gtk_box_set_homogeneous(GTK_BOX(bar), FALSE);
    GtkWidget *u = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(u), "bar-used");
    gtk_style_context_add_class(gtk_widget_get_style_context(f), "bar-free");
    int width = S(380), uw = (int)(width * used / total);
    gtk_widget_set_size_request(u, uw < S(4) ? S(4) : uw, -1);
    gtk_box_pack_start(GTK_BOX(bar), u, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), f, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), bar, FALSE, FALSE, 0);
    char *bottom = g_strdup_printf("%.1f GB used", used / 1e9);
    gtk_box_pack_start(GTK_BOX(v), label_new(bottom, "footnote", 0), FALSE, FALSE, 0);
    row_add(g, v);
    g_free(top);
    g_free(bottom);
}

static void page_storage(GtkWidget *page)
{
    GtkWidget *g = group_new(page, NULL);
    storage_row(g, "/", "MyOS (system)");
    struct stat a, b;
    if (!stat("/data", &a) && !stat("/", &b) && a.st_dev != b.st_dev)
        storage_row(g, "/data", "Data");
}

static GtkWidget *clock_label;

static void show_time(void)
{
    char s[64];
    time_t t = time(NULL);
    strftime(s, sizeof s, "%A, %e %B %Y, %H:%M:%S", localtime(&t));
    gtk_label_set_text(GTK_LABEL(clock_label), s);
}

static gboolean tick(gpointer unused)
{
    (void)unused;
    if (clock_label)
        show_time();
    return G_SOURCE_CONTINUE;
}

static void page_datetime(GtkWidget *page)
{
    char zone[128] = "UTC";
    ssize_t n = readlink("/etc/localtime", zone, sizeof zone - 1);
    if (n > 0) {
        zone[n] = 0;
        char *z = strstr(zone, "zoneinfo/");
        if (z)
            memmove(zone, z + 9, strlen(z + 9) + 1);
    } else if (getenv("TZ")) {
        snprintf(zone, sizeof zone, "%s", getenv("TZ"));
    }
    GtkWidget *g = group_new(page, NULL);
    clock_label = row_value(g, "Date and time", "");
    show_time();
    row_value(g, "Time zone", zone);
}

/* --- Displays -----------------------------------------------------------------------------
   Five pictures of the same desktop, each drawn the way it would look:
   bigger text on the left, more room on the right. */
static const int sizes[] = {200, 175, 150, 125, 100};
static GtkWidget *looks_like;

static void set_scale(int pct);

static gboolean draw_thumb(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int pct = GPOINTER_TO_INT(data);
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double pad = 3 * k, x = pad, y = pad, tw = W - 2 * pad, th = H - 2 * pad;
    double f = pct / 100.0;
    int selected = pct == effective();

    if (selected) {                         /* the system-blue ring */
        rounded(cr, 0, 0, W, H, 9 * k);
        rgb(cr, 0x0a64d6);
        cairo_fill(cr);
        rounded(cr, pad * .6, pad * .6, W - pad * 1.2, H - pad * 1.2, 7.5 * k);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
    }
    /* The desktop: Golden Hour's sky to hills. */
    rounded(cr, x, y, tw, th, 6 * k);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + th);
    cairo_pattern_add_color_stop_rgb(p, 0, .96, .80, .55);
    cairo_pattern_add_color_stop_rgb(p, .55, .93, .70, .45);
    cairo_pattern_add_color_stop_rgb(p, 1, .42, .58, .52);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    /* The taskbar, as tall as it would be at this size. */
    double bar = th * .065 * f;
    rgb(cr, 0x1f6f78);
    cairo_rectangle(cr, x, y + th - bar, tw, bar);
    cairo_fill(cr);
    /* A window: a title bar and lines of text, all f times as big. */
    double wx = x + tw * .14, wy = y + th * .14, ww = tw * .62, wh = th * .62;
    rounded(cr, wx, wy, ww, wh, 2.5 * k);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    double tb = th * .06 * f;
    rgb(cr, 0x2a8a93);
    cairo_rectangle(cr, wx, wy, ww, tb);
    cairo_fill(cr);
    double line = th * .025 * f, gap = line * 1.9;
    rgb(cr, 0xb8b8bd);
    for (double ly = wy + tb + gap; ly + line < wy + wh - gap * .5; ly += gap) {
        double len = ww * (fmod(ly * 7.3, 1.0) * .3 + .55);
        cairo_rectangle(cr, wx + ww * .08, ly, fmin(len, ww * .84), line);
    }
    cairo_fill(cr);
    cairo_restore(cr);
    return TRUE;
}

static void thumb_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    set_scale(GPOINTER_TO_INT(data));
}

static void page_displays(GtkWidget *page)
{
    char *s;
    /* The screen, as the Mac draws it at the top. */
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
    GtkWidget *i = icon_new(IC_DISPLAYS, 64);
    gtk_widget_set_halign(i, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), i, FALSE, FALSE, 0);
    /* The card's name: the first 16 bytes of Linux's fb_fix_screeninfo
       (FBIOGET_FSCREENINFO, 0x4602; 68 bytes in all on i386). */
    const char *name = "Display";
    char fix[68];
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && ioctl(fd, 0x4602, fix) == 0 && !strncmp(fix, "bochsdrmfb", 10))
        name = "Standard VGA (QEMU, UTM)";
    if (fd >= 0)
        close(fd);
    gtk_box_pack_start(GTK_BOX(v), label_new(name, "section", .5), FALSE, FALSE, 0);
    s = g_strdup_printf("%d \u00d7 %d", screen_w, screen_h);
    gtk_box_pack_start(GTK_BOX(v), label_new(s, "footnote", .5), FALSE, FALSE, 0);
    g_free(s);
    gtk_box_pack_start(GTK_BOX(page), v, FALSE, FALSE, S(6));

    /* The sizes. Only those that leave at least 800 x 600 to work in. */
    GtkWidget *g = group_new(page, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(10));
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
    gtk_box_pack_start(GTK_BOX(top), label_new("Text and window size", NULL, 0), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(12));
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    int shown = 0, n = sizeof sizes / sizeof sizes[0];
    for (int j = 0; j < n; j++) {
        int pct = sizes[j];
        if (pct != 100 && (screen_w * 100 / pct < 800 || screen_h * 100 / pct < 600))
            continue;
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "thumb");
        GtkWidget *d = gtk_drawing_area_new();
        gtk_widget_set_size_request(d, S(88), S(56));
        g_signal_connect(d, "draw", G_CALLBACK(draw_thumb), GINT_TO_POINTER(pct));
        gtk_container_add(GTK_CONTAINER(b), d);
        g_signal_connect(b, "clicked", G_CALLBACK(thumb_clicked), GINT_TO_POINTER(pct));
        char *tip = g_strdup_printf("%d%%: looks like %d \u00d7 %d", pct, screen_w * 100 / pct, screen_h * 100 / pct);
        gtk_widget_set_tooltip_text(b, tip);
        g_free(tip);
        gtk_box_pack_start(GTK_BOX(col), b, FALSE, FALSE, 0);
        const char *what = pct == auto_pct ? "Default" : shown == 0 ? "Larger Text" : pct == 100 ? "More Space" : "";
        GtkWidget *l = label_new(what, "thumb-label", .5);
        if (pct == effective())
            gtk_style_context_add_class(gtk_widget_get_style_context(l), "selected");
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), col, FALSE, FALSE, 0);
        shown++;
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    s = g_strdup_printf("%d%% \u2014 looks like %d \u00d7 %d", effective(), screen_w * 100 / effective(),
                        screen_h * 100 / effective());
    looks_like = label_new(s, "value", .5);
    g_free(s);
    gtk_box_pack_start(GTK_BOX(box), looks_like, FALSE, FALSE, 0);
    row_add(g, box);

    g = group_new(page, NULL);
    s = g_strdup_printf("%d \u00d7 %d", screen_w, screen_h);
    row_value(g, "Resolution", s);
    g_free(s);
    s = g_strdup_printf("%d%%%s", effective(), chosen ? "" : " (automatic)");
    row_value(g, "Scale", s);
    g_free(s);

    gtk_box_pack_start(GTK_BOX(page),
                       label_new("GTK programs (Settings, AbiWord, Geany) change size at once. "
                                 "xterm windows that are already open keep their size until they "
                                 "are opened again. Each user has their own size "
                                 "(~/.config/myos/display.conf).",
                                 "footnote", 0),
                       FALSE, FALSE, 0);
}

/* --- Mouse & Keyboard ----------------------------------------------------------------------
   The pointer's size, as four pictures of the arrow, and the keys that
   work everywhere. The size goes to myos-xsettings like the scale does. */
static const int pointer_sizes[] = {100, 150, 200, 300};
static const char *const pointer_names[] = {"Normal", "Large", "Larger", "Largest"};
static int pointer = POINTER_MIN;
static Atom a_pointer;

static void read_pointer(void)
{
    Display *d = gdk_x11_get_default_xdisplay();
    a_pointer = XInternAtom(d, "_MYOS_POINTER", False);
    long v = root_cardinal(d, a_pointer, -1);
    pointer = v < 0 ? pointer_read_conf() : clamp_pointer(v);
}

/* The arrow, white with a dark edge like Adwaita's, its tip at (x, y),
   h tall. */
static void draw_arrow(cairo_t *cr, double x, double y, double h)
{
    static const double pts[][2] = {{0, 0}, {0, 17}, {4, 13.2}, {6.8, 19.6}, {9.6, 18.4},
                                    {6.9, 12.2}, {12, 12.2}};
    double f = h / 19.6;
    cairo_move_to(cr, x, y);
    for (size_t i = 1; i < sizeof pts / sizeof pts[0]; i++)
        cairo_line_to(cr, x + pts[i][0] * f, y + pts[i][1] * f);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, .1, .1, .1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, fmax(1, f * 1.2));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
}

static gboolean draw_pointer_thumb(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int pct = GPOINTER_TO_INT(data);
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double pad = 3 * k;
    if (pct == pointer) {                   /* the system-blue ring, as on Displays */
        rounded(cr, 0, 0, W, H, 9 * k);
        rgb(cr, 0x0a64d6);
        cairo_fill(cr);
    }
    rounded(cr, pad, pad, W - 2 * pad, H - 2 * pad, 6 * k);
    rgb(cr, 0xd9a35f);                      /* Golden Hour's sand, so the white edge shows */
    cairo_fill(cr);
    double h = (H - 4 * pad) * pct / POINTER_MAX;
    draw_arrow(cr, (W - h * 12 / 19.6) / 2, (H - h) / 2, h);
    return TRUE;
}

static void set_pointer(int pct);

static void pointer_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    set_pointer(GPOINTER_TO_INT(data));
}

static void page_mouse(GtkWidget *page)
{
    GtkWidget *g = group_new(page, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(10));
    gtk_box_pack_start(GTK_BOX(box), label_new("Pointer size", NULL, 0), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(12));
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    for (size_t j = 0; j < sizeof pointer_sizes / sizeof pointer_sizes[0]; j++) {
        int pct = pointer_sizes[j];
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "thumb");
        GtkWidget *d = gtk_drawing_area_new();
        gtk_widget_set_size_request(d, S(72), S(72));
        g_signal_connect(d, "draw", G_CALLBACK(draw_pointer_thumb), GINT_TO_POINTER(pct));
        gtk_container_add(GTK_CONTAINER(b), d);
        g_signal_connect(b, "clicked", G_CALLBACK(pointer_clicked), GINT_TO_POINTER(pct));
        /* What X will show: the theme's nearest size. */
        static const int have[] = {24, 32, 48, 64, 96};
        int want = pointer_pixels(effective(), pct), best = have[0];
        for (size_t h = 1; h < sizeof have / sizeof have[0]; h++)
            if (abs(have[h] - want) < abs(best - want))
                best = have[h];
        char *tip = g_strdup_printf("%d pixels", best);
        gtk_widget_set_tooltip_text(b, tip);
        g_free(tip);
        gtk_box_pack_start(GTK_BOX(col), b, FALSE, FALSE, 0);
        GtkWidget *l = label_new(pointer_names[j], "thumb-label", .5);
        if (pct == pointer)
            gtk_style_context_add_class(gtk_widget_get_style_context(l), "selected");
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), col, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    row_add(g, box);

    g = group_new(page, "Keyboard shortcuts");
    row_value(g, "Start menu", "Ctrl+Esc or the Windows key");
    row_value(g, "Switch windows", "Alt+Tab");
    row_value(g, "Terminal", "Ctrl+Alt+T");
    row_value(g, "Settings", "Ctrl+Alt+S");

    gtk_box_pack_start(GTK_BOX(page),
                       label_new("The pointer grows with the desktop's size (Displays) too. Programs "
                                 "that are already open keep their pointer until they are opened "
                                 "again; Settings and the desktop change at once. Each user has their "
                                 "own size (~/.config/myos/mouse.conf).",
                                 "footnote", 0),
                       FALSE, FALSE, 0);
}

/* --- Users & Groups ------------------------------------------------------------------------ */

static int in_group(const char *user, const char *group)
{
    struct group *g = getgrnam(group);
    if (!g)
        return 0;
    for (char **m = g->gr_mem; *m; m++)
        if (!strcmp(*m, user))
            return 1;
    return 0;
}

/* Adding a user. Settings runs as you, so the work is done by
   /usr/libexec/myos-adduser (setuid root; see myos-adduser.c): it checks an
   administrator's password with PAM and writes passwd, group and shadow.
   Requests go to its stdin a field per line, one reply line comes back.
   It runs while our main loop keeps going, so the window still draws during
   the password check (a wrong one takes a second, on purpose). */
#define ADDUSER "/usr/libexec/myos-adduser"
#define FIELD_MAX 200               /* the helper takes lines of up to 255 bytes */

static GtkWidget *window;
static const char *current;
static void show(const char *id);

struct helper_wait {
    GMainLoop *loop;
    int status;
};

static void helper_done(GPid pid, gint status, gpointer data)
{
    struct helper_wait *w = data;
    w->status = status;
    g_spawn_close_pid(pid);
    g_main_loop_quit(w->loop);
}

static int run_helper(const char *request, char *reply, size_t size)
{
    char *argv[] = {ADDUSER, NULL};
    GPid pid;
    int in, out;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &in, &out,
                                  NULL, NULL)) {
        snprintf(reply, size, "Can't start %s.", ADDUSER);
        return -1;
    }
    /* A request is at most 7 x 201 bytes: it fits in the pipe at once. */
    size_t n = strlen(request), done = 0;
    while (done < n) {
        ssize_t w = write(in, request + done, n - done);
        if (w <= 0)
            break;
        done += w;
    }
    close(in);
    struct helper_wait w = {g_main_loop_new(NULL, FALSE), -1};
    g_child_watch_add(pid, helper_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    ssize_t r = read(out, reply, size - 1);
    close(out);
    reply[r > 0 ? r : 0] = 0;
    reply[strcspn(reply, "\n")] = 0;
    if (!reply[0])
        snprintf(reply, size, "%s stopped without an answer.", ADDUSER);
    return w.status == 0 && !strcmp(reply, "ok") ? 0 : -1;
}

static GtkWidget *form_entry(GtkWidget *grid, int row, const char *name, gboolean secret)
{
    GtkWidget *l = label_new(name, NULL, 1);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e), FIELD_MAX);
    gtk_entry_set_visibility(GTK_ENTRY(e), !secret);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
    return e;
}

/* A dialog with a sentence, a grid for fields, a red line for errors,
   Cancel and one more button (the default). */
static GtkWidget *form_dialog(const char *title, const char *text, const char *ok, GtkWidget **grid,
                              GtkWidget **error)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "Cancel", GTK_RESPONSE_CANCEL, ok, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_box_set_spacing(GTK_BOX(box), S(10));
    gtk_container_set_border_width(GTK_CONTAINER(box), S(16));
    GtkWidget *t = label_new(text, NULL, 0);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 46);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(*grid), S(8));
    gtk_grid_set_column_spacing(GTK_GRID(*grid), S(10));
    gtk_box_pack_start(GTK_BOX(box), *grid, FALSE, FALSE, 0);
    *error = label_new("", "error", 0);
    gtk_box_pack_start(GTK_BOX(box), *error, FALSE, FALSE, 0);
    return d;
}

static void wipe_entry(GtkWidget *e)
{
    gtk_entry_set_text(GTK_ENTRY(e), "");
}

/* "Settings wants to make changes": an administrator's name and password,
   checked by the helper. On success both are left in name/pass. */
static int ask_admin(const char *why, char *name, char *pass)
{
    GtkWidget *grid, *error;
    GtkWidget *d = form_dialog("Settings", why, "Unlock", &grid, &error);
    GtkWidget *who = form_entry(grid, 0, "User name", FALSE), *pw = form_entry(grid, 1, "Password", TRUE);
    struct passwd *me = getpwuid(getuid());
    if (me && in_group(me->pw_name, "wheel")) {
        gtk_entry_set_text(GTK_ENTRY(who), me->pw_name);
        gtk_widget_grab_focus(pw);
    }
    gtk_widget_show_all(d);
    int ok = 0;
    while (!ok && gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        char request[2 * FIELD_MAX + 16], reply[256];
        snprintf(name, FIELD_MAX + 1, "%s", gtk_entry_get_text(GTK_ENTRY(who)));
        snprintf(pass, FIELD_MAX + 1, "%s", gtk_entry_get_text(GTK_ENTRY(pw)));
        snprintf(request, sizeof request, "check\n%s\n%s\n", name, pass);
        gtk_widget_set_sensitive(d, FALSE);
        if (strpbrk(name, "\r\n") || strpbrk(pass, "\r\n"))
            snprintf(reply, sizeof reply, "Names and passwords can't contain line breaks.");
        else
            ok = run_helper(request, reply, sizeof reply) == 0;
        explicit_bzero(request, sizeof request);
        gtk_widget_set_sensitive(d, TRUE);
        if (!ok) {
            gtk_label_set_text(GTK_LABEL(error), reply);
            wipe_entry(pw);
            gtk_widget_grab_focus(pw);
        }
    }
    wipe_entry(pw);
    gtk_widget_destroy(d);
    if (!ok)
        explicit_bzero(pass, FIELD_MAX + 1);
    return ok;
}

/* The account name follows the full name ("Ann Lee" -> "annlee") until
   you type in it yourself. */
static void full_name_changed(GtkEditable *e, gpointer data)
{
    GtkWidget *account = data;
    if (g_object_get_data(G_OBJECT(account), "typed"))
        return;
    char out[33];
    size_t n = 0;
    for (const char *s = gtk_entry_get_text(GTK_ENTRY(e)); *s && n < 31; s++)
        if (g_ascii_isalnum(*s) && (n || g_ascii_isalpha(*s)))
            out[n++] = g_ascii_tolower(*s);
    out[n] = 0;
    g_object_set_data(G_OBJECT(account), "setting", GINT_TO_POINTER(1));
    gtk_entry_set_text(GTK_ENTRY(account), out);
    g_object_set_data(G_OBJECT(account), "setting", NULL);
}

static void account_changed(GtkEditable *e, gpointer unused)
{
    (void)unused;
    if (!g_object_get_data(G_OBJECT(e), "setting"))
        g_object_set_data(G_OBJECT(e), "typed", GINT_TO_POINTER(1));
}

static const char *check_new_user(const char *full, const char *account, const char *p1, const char *p2)
{
    if (strpbrk(full, "\r\n") || strpbrk(account, "\r\n") || strpbrk(p1, "\r\n"))
        return "Names and passwords can't contain line breaks.";
    if (!*full || strpbrk(full, ":,"))
        return "Type the person's full name (without : or ,).";
    if (!*account || !(g_ascii_islower(account[0]) || account[0] == '_'))
        return "The account name must start with a small letter.";
    for (const char *s = account; *s; s++)
        if (!(g_ascii_islower(*s) || g_ascii_isdigit(*s) || *s == '_' || *s == '-'))
            return "The account name can only have small letters, digits, - and _.";
    if (strlen(account) > 31)
        return "The account name can be at most 31 characters.";
    if (getpwnam(account))
        return "There is already an account with that name.";
    if (!*p1)
        return "Type a password for the new account.";
    if (strcmp(p1, p2))
        return "The two passwords don't match.";
    return NULL;
}

static void add_user_clicked(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    char admin[FIELD_MAX + 1], apass[FIELD_MAX + 1];
    if (!ask_admin("Settings wants to add a user. Enter an administrator's name and password to allow this.",
                   admin, apass))
        return;

    GtkWidget *grid, *error;
    GtkWidget *d = form_dialog("New User", "A new account, with its own home folder and settings.", "Create User",
                               &grid, &error);
    GtkWidget *full = form_entry(grid, 0, "Full name", FALSE), *account = form_entry(grid, 1, "Account name", FALSE);
    GtkWidget *p1 = form_entry(grid, 2, "Password", TRUE), *p2 = form_entry(grid, 3, "Verify", TRUE);
    GtkWidget *admin_sw = gtk_switch_new();
    gtk_widget_set_halign(admin_sw, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label_new("Administrator", NULL, 1), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), admin_sw, 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label_new("Lets this user add users and run commands as root.", "footnote", 0),
                    1, 5, 1, 1);
    g_signal_connect(full, "changed", G_CALLBACK(full_name_changed), account);
    g_signal_connect(account, "changed", G_CALLBACK(account_changed), NULL);
    gtk_widget_show_all(d);

    int done = 0;
    while (!done && gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        const char *f = gtk_entry_get_text(GTK_ENTRY(full)), *a = gtk_entry_get_text(GTK_ENTRY(account));
        const char *why = check_new_user(f, a, gtk_entry_get_text(GTK_ENTRY(p1)), gtk_entry_get_text(GTK_ENTRY(p2)));
        if (why) {
            gtk_label_set_text(GTK_LABEL(error), why);
            continue;
        }
        char request[7 * (FIELD_MAX + 1) + 16], reply[256];
        snprintf(request, sizeof request, "add\n%s\n%s\n%s\n%s\n%s\n%s\n", admin, apass, f, a,
                 gtk_entry_get_text(GTK_ENTRY(p1)), gtk_switch_get_active(GTK_SWITCH(admin_sw)) ? "admin" : "standard");
        gtk_widget_set_sensitive(d, FALSE);
        done = run_helper(request, reply, sizeof reply) == 0;
        explicit_bzero(request, sizeof request);
        gtk_widget_set_sensitive(d, TRUE);
        if (!done)
            gtk_label_set_text(GTK_LABEL(error), reply);
    }
    wipe_entry(p1);
    wipe_entry(p2);
    gtk_widget_destroy(d);
    explicit_bzero(apass, sizeof apass);
    if (done)
        show(current);              /* the list, with the new user in it */
}

static void page_users(GtkWidget *page)
{
    GtkWidget *g = group_new(page, NULL);
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000)
            continue;
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
        gtk_box_pack_start(GTK_BOX(h), avatar_new(pw->pw_name, 34), FALSE, FALSE, 0);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
        char *full = g_strdup(pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : pw->pw_name);
        full[strcspn(full, ",")] = 0;
        gtk_box_pack_start(GTK_BOX(v), label_new(full, NULL, 0), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), label_new(in_group(pw->pw_name, "wheel") ? "Admin" : "Standard", "footnote", 0),
                           FALSE, FALSE, 0);
        g_free(full);
        gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
        if (pw->pw_uid == getuid())
            gtk_box_pack_end(GTK_BOX(h), label_new("You", "value", 1), FALSE, FALSE, 0);
        row_add(g, h);
    }
    endpwent();
    GtkWidget *add = gtk_button_new_with_label("Add User\u2026");
    gtk_widget_set_halign(add, GTK_ALIGN_END);
    g_signal_connect(add, "clicked", G_CALLBACK(add_user_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(page), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       label_new("Adding a user needs an administrator's name and password, as on the Mac. "
                                 "Administrators can also run commands as root with doas.", "footnote", 0),
                       FALSE, FALSE, 0);
}

/* --- Wallpaper -----------------------------------------------------------------------------
   The Mac's Wallpaper page: a large preview with the fit menu on it
   (Fill Screen, Fit to Screen, Stretch, Center, Tile), Add Photo…, then
   the pictures and a row of colours. myos-desktop paints the result. */

static struct wall shown_wall;
static int wall_gen;

static void show(const char *id);

/* myos-desktop is waiting on this. It paints the new picture and keeps
   the pixmap, which would vanish if this program owned it and then quit. */
static gboolean wall_refresh(gpointer unused)
{
    (void)unused;
    show("wallpaper");
    return G_SOURCE_REMOVE;
}

static void wall_publish(const struct wall *c)
{
    shown_wall = *c;
    wall_write(c);
    Display *d = gdk_x11_get_default_xdisplay();
    long v = ++wall_gen;
    Atom a = XInternAtom(d, "_MYOS_WALLPAPER", False);
    XChangeProperty(d, DefaultRootWindow(d), a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(d);
    /* Rebuilding the page now would destroy the control whose signal is
       still running. Do it once that returns. */
    g_idle_add(wall_refresh, NULL);
}

static gboolean draw_wall_preview(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double aspect = screen_h > 0 ? (double)screen_w / screen_h : 1.6;
    double aw = W - S(8), ah = aw / aspect;
    if (ah > H - S(8)) {
        ah = H - S(8);
        aw = ah * aspect;
    }
    double x = (W - aw) / 2, y = (H - ah) / 2;
    cairo_save(cr);
    rounded(cr, x, y, aw, ah, S(10));
    cairo_clip(cr);
    wall_paint(cr, x, y, aw, ah, &shown_wall);
    cairo_restore(cr);
    rounded(cr, x, y, aw, ah, S(10));
    cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return TRUE;
}

static void wall_mode_changed(GtkComboBox *box, gpointer unused)
{
    (void)unused;
    int m = gtk_combo_box_get_active(box);
    if (m < 0 || m == shown_wall.mode)
        return;
    shown_wall.mode = m;
    wall_publish(&shown_wall);
}

static void wall_pick_image(const char *path)
{
    struct wall c = shown_wall;
    snprintf(c.image, sizeof c.image, "%s", path);
    c.color[0] = 0;
    wall_publish(&c);
}

static void wall_pick_color(const char *hex)
{
    struct wall c = shown_wall;
    c.image[0] = 0;
    snprintf(c.color, sizeof c.color, "%s", hex);
    wall_publish(&c);
}

static void wall_add_photo(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Add Photo", GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
        GTK_RESPONSE_CANCEL, "Add", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, "Pictures");
    gtk_file_filter_add_pixbuf_formats(filt);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filt);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg);
        return;
    }
    char *src = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    if (!src)
        return;
    /* The theme's own picture is used where it stands. Anything else is
       copied into the account, so deleting the original doesn't blank
       the desktop. */
    if (!strcmp(src, WALL_THEME) || !strncmp(src, WALL_SHARE "/", sizeof WALL_SHARE)) {
        wall_pick_image(src);
        g_free(src);
        return;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.local/share/myos/wallpapers", wall_home());
    wall_mkdir(dir);
    if (!strncmp(src, dir, strlen(dir))) {
        wall_pick_image(src);
        g_free(src);
        return;
    }
    char *base = g_path_get_basename(src);
    char dest[PATH_MAX];
    snprintf(dest, sizeof dest, "%s/%s", dir, base);
    for (int n = 2; g_file_test(dest, G_FILE_TEST_EXISTS) && n < 100; n++) {
        const char *dot = strrchr(base, '.');
        if (dot)
            snprintf(dest, sizeof dest, "%s/%.*s %d%s", dir, (int)(dot - base), base, n, dot);
        else
            snprintf(dest, sizeof dest, "%s/%s %d", dir, base, n);
    }
    GFile *from = g_file_new_for_path(src), *to = g_file_new_for_path(dest);
    gboolean ok = g_file_copy(from, to, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    g_object_unref(from);
    g_object_unref(to);
    g_free(base);
    g_free(src);
    if (ok)
        wall_pick_image(dest);
}

static gboolean draw_wall_thumb(GtkWidget *w, cairo_t *cr, gpointer data)
{
    const char *path = data;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    int on = shown_wall.color[0] == 0 && !strcmp(path, shown_wall.image);
    if (on) {
        rounded(cr, 0, 0, W, H, S(8));
        rgb(cr, 0x0a64d6);
        cairo_fill(cr);
    }
    double pad = on ? S(3) : 0;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, (int)(W - 2 * pad), (int)(H - 2 * pad), NULL);
    if (pb) {
        double tw = gdk_pixbuf_get_width(pb), th = gdk_pixbuf_get_height(pb);
        cairo_save(cr);
        rounded(cr, pad + (W - 2 * pad - tw) / 2, pad, tw, th, S(6));
        cairo_clip(cr);
        gdk_cairo_set_source_pixbuf(cr, pb, pad + (W - 2 * pad - tw) / 2, pad + (H - 2 * pad - th) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(pb);
    }
    return TRUE;
}

static void wall_thumb_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    wall_pick_image(data);
}

static void wall_add_thumb(GtkWidget *flow, const char *path);

static int wall_name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every picture in dir, by name. Hidden files are skipped. */
static void wall_add_dir(GtkWidget *flow, const char *dir)
{
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd)
        return;
    char **names = NULL;
    int n = 0;
    const char *name;
    while ((name = g_dir_read_name(gd))) {
        if (name[0] == '.')
            continue;
        names = g_renew(char *, names, n + 1);
        names[n++] = g_strdup(name);
    }
    g_dir_close(gd);
    if (n)
        qsort(names, (size_t)n, sizeof *names, wall_name_cmp);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        wall_add_thumb(flow, path);
        g_free(names[i]);
    }
    g_free(names);
}

static void wall_add_thumb(GtkWidget *flow, const char *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return;
    GtkWidget *b = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "thumb");
    GtkWidget *d = gtk_drawing_area_new();
    gtk_widget_set_size_request(d, S(112), S(70));
    g_signal_connect_data(d, "draw", G_CALLBACK(draw_wall_thumb), g_strdup(path), free_data, 0);
    gtk_container_add(GTK_CONTAINER(b), d);
    g_signal_connect_data(b, "clicked", G_CALLBACK(wall_thumb_clicked), g_strdup(path), free_data, 0);
    gtk_container_add(GTK_CONTAINER(flow), b);
}

static gboolean draw_color_dot(GtkWidget *w, cairo_t *cr, gpointer data)
{
    const char *hex = data;
    double s = gtk_widget_get_allocated_height(w);
    int on = shown_wall.image[0] == 0 && !strcmp(hex, shown_wall.color);
    if (on) {
        cairo_arc(cr, s / 2, s / 2, s / 2 - 1, 0, 2 * G_PI);
        rgb(cr, 0x0a64d6);
        cairo_fill(cr);
    }
    unsigned rgbv = (unsigned)strtoul(hex + 1, NULL, 16);
    cairo_arc(cr, s / 2, s / 2, s / 2 - (on ? S(3) : 1), 0, 2 * G_PI);
    cairo_set_source_rgb(cr, (rgbv >> 16 & 255) / 255.0, (rgbv >> 8 & 255) / 255.0, (rgbv & 255) / 255.0);
    cairo_fill(cr);
    return TRUE;
}

static void wall_color_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    wall_pick_color(data);
}

static void page_wallpaper(GtkWidget *page)
{
    wall_read(&shown_wall);
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *preview = gtk_drawing_area_new();
    gtk_widget_set_size_request(preview, -1, S(210));
    gtk_widget_set_hexpand(preview, TRUE);
    g_signal_connect(preview, "draw", G_CALLBACK(draw_wall_preview), NULL);
    gtk_container_add(GTK_CONTAINER(overlay), preview);
    GtkWidget *mode = gtk_combo_box_text_new();
    for (int i = 0; i < WALL_NMODE; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), wall_labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), shown_wall.mode);
    g_signal_connect(mode, "changed", G_CALLBACK(wall_mode_changed), NULL);
    gtk_widget_set_halign(mode, GTK_ALIGN_END);
    gtk_widget_set_valign(mode, GTK_ALIGN_END);
    gtk_widget_set_margin_end(mode, S(16));
    gtk_widget_set_margin_bottom(mode, S(12));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), mode);
    gtk_box_pack_start(GTK_BOX(page), overlay, FALSE, FALSE, 0);

    GtkWidget *add = gtk_button_new_with_label("Add Photo\u2026");
    gtk_widget_set_halign(add, GTK_ALIGN_START);
    g_signal_connect(add, "clicked", G_CALLBACK(wall_add_photo), NULL);
    gtk_box_pack_start(GTK_BOX(page), add, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), label_new("Desktop Pictures", "section", 0), FALSE, FALSE, 0);
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 3);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), (guint)S(8));
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), (guint)S(8));
    wall_add_thumb(flow, WALL_THEME);
    wall_add_dir(flow, WALL_SHARE);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.local/share/myos/wallpapers", wall_home());
    wall_add_dir(flow, dir);
    /* A picture added from somewhere else (an older choice) still shows. */
    if (shown_wall.image[0] && strcmp(shown_wall.image, WALL_THEME) && !strstr(shown_wall.image, "/myos/wallpapers/"))
        wall_add_thumb(flow, shown_wall.image);
    gtk_box_pack_start(GTK_BOX(page), flow, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), label_new("Colors", "section", 0), FALSE, FALSE, 0);
    static const char *const colors[] = {"#0a84ff", "#5e5ce6", "#bf5af2", "#ff375f", "#ff9f0a",
                                         "#ffd60a", "#30d158", "#8e8e93", "#1d1d1f"};
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        GtkWidget *b = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "thumb");
        GtkWidget *d = gtk_drawing_area_new();
        gtk_widget_set_size_request(d, S(28), S(28));
        g_signal_connect_data(d, "draw", G_CALLBACK(draw_color_dot), g_strdup(colors[i]), free_data, 0);
        gtk_container_add(GTK_CONTAINER(b), d);
        g_signal_connect_data(b, "clicked", G_CALLBACK(wall_color_clicked), g_strdup(colors[i]), free_data, 0);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
}

/* --- the window ------------------------------------------------------------------------------- */

static void destroy_child(GtkWidget *w, gpointer unused);

/* --- Kernel ----------------------------------------------------------------------------
   docs/KERNEL-CONFIG.md, built as mockup B: the mode slider (realtime, soft
   realtime, max power savings); in the two real-time modes the 20 ms frame
   as a wheel with a name field beside each slot, and under it the task list
   (which program runs in a task's slots, as whom); in power savings a grid
   of cards, one per saving this computer has. Save checks everything
   (kconfig.c), then /usr/libexec/myos-kconfig (setuid root) checks the
   administrator's password and the config again and writes it to the
   myos-config partition. The kernel reads it at the next start and gives
   the slots to the tasks named here; the modes, starting the programs and
   the power savings come later: the page says so. */
#define KCONFIG_HELPER "/usr/libexec/myos-kconfig"

static struct kc_config kc;             /* what the page shows */
static struct kc_machine kcm;
static char kc_dev[64], kc_badge_text[256];
static int kc_cleared;                  /* savings switched off on load: this computer lacks them */
static GtkWidget *k_badge, *k_rail, *k_mode_label[3], *k_mode_desc, *k_body, *k_wheel, *k_entries[KC_SLOTS],
    *k_chips[KC_SLOTS], *k_tasks, *k_cards, *k_status, *k_save;
static const uint32_t k_colours[] = {0x0a84ff, 0xbf5af2, 0xff9f0a, 0x30d158, 0xff375f, 0x5e5ce6, 0x64b9d6, 0xac8e68,
                                     0xe0b000};
static const char *const k_mode_name[] = {"Realtime", "Soft realtime", "Max power savings"};
static const char *const k_mode_text[] = {
    "Named tasks get their slots exactly, every time: they start on the slot boundary and are stopped at its "
    "end. No power management: the processor runs at its full rated speed (no boost) and only halts when there "
    "is nothing to do; disks and devices never sleep.",
    "Named tasks run first in their slots, but without a promise: one may start a little late or run a little "
    "over. The processor may rest in idle states that wake within the latency budget. Its speed stays fixed, and "
    "disks and devices stay on.",
    "No time slots: every program takes its turn. The processor slows down and sleeps, and disks and devices "
    "power down when idle. Choose which savings this computer uses.",
};

static void k_rebuild_body(void);
static void k_rebuild_tasks(void);
static void k_update_status(void);

/* A task's colour: by its place in the task list. */
static int k_task_index(const char *name)
{
    char names[KC_MAX_TASKS][KC_NAME];
    int n = kc_tasks(&kc, names);
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return i;
    return -1;
}

/* Fill the current path with a slot's colour: the OS striped teal, a
   task its colour, a free slot light grey. */
static void k_fill_slot(cairo_t *cr, const char *name, double w, double h)
{
    if (!strcmp(name, "os")) {
        cairo_save(cr);
        cairo_clip_preserve(cr);
        rgb(cr, 0x1b6b78);
        cairo_fill(cr);
        rgb(cr, 0x2b8796);
        cairo_set_line_width(cr, S(4));
        for (double x = -h; x < w + h; x += S(8)) {
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x + h, h);
        }
        cairo_stroke(cr);
        cairo_restore(cr);
        cairo_new_path(cr);
        return;
    }
    int i = name[0] ? k_task_index(name) : -1;
    rgb(cr, i >= 0 ? k_colours[i % (int)(sizeof k_colours / sizeof k_colours[0])] : 0xefede7);
    cairo_fill(cr);
}

static gboolean k_draw_chip(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int s = GPOINTER_TO_INT(data);
    double cw = gtk_widget_get_allocated_width(w), ch = gtk_widget_get_allocated_height(w);
    rounded(cr, 0, 0, cw, ch, S(2));
    k_fill_slot(cr, kc.slots[s], cw, ch);
    return TRUE;
}

/* The 20 ms frame as a wheel: slot 0 at 12 o'clock, clockwise; the OS
   wedges (0 and 10) face each other. */
static gboolean k_draw_wheel(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double size = gtk_widget_get_allocated_width(w), cx = size / 2, cy = size / 2;
    double R = size * .46, r = size * .28, seg = 2 * M_PI / KC_SLOTS;
    for (int s = 0; s < KC_SLOTS; s++) {
        double a0 = -M_PI / 2 + s * seg + .012, a1 = a0 + seg - .024;
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, R, a0, a1);
        cairo_arc_negative(cr, cx, cy, r, a1, a0);
        cairo_close_path(cr);
        k_fill_slot(cr, kc.slots[s], size, size);
        char num[4];
        snprintf(num, sizeof num, "%d", s);
        double am = a0 + seg / 2, tx = cx + (R + r) / 2 * cos(am), ty = cy + (R + r) / 2 * sin(am);
        cairo_text_extents_t te;
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, S(10.5));
        cairo_text_extents(cr, num, &te);
        if (kc.slots[s][0])
            cairo_set_source_rgb(cr, 1, 1, 1);
        else
            rgb(cr, 0x8a93a0);
        cairo_move_to(cr, tx - te.width / 2 - te.x_bearing, ty - te.height / 2 - te.y_bearing);
        cairo_show_text(cr, num);
    }
    const char *lines[] = {"20 ms", "then it repeats", "OS at 0 and 10"};
    const double sizes[] = {S(20), S(10), S(9.5)}, dy[] = {-S(6), S(12), S(28)};
    const uint32_t cols[] = {0x1d1d1f, 0x6e6e73, 0x1b6b78};
    for (int i = 0; i < 3; i++) {
        cairo_text_extents_t te;
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                               i ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, sizes[i]);
        cairo_text_extents(cr, lines[i], &te);
        rgb(cr, cols[i]);
        cairo_move_to(cr, cx - te.width / 2 - te.x_bearing, cy + dy[i]);
        cairo_show_text(cr, lines[i]);
    }
    return TRUE;
}

/* A click on a wedge goes to that slot's name field. */
static gboolean k_wheel_click(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
    (void)unused;
    double size = gtk_widget_get_allocated_width(w), dx = ev->x - size / 2, dy = ev->y - size / 2;
    double a = atan2(dy, dx) + M_PI / 2;
    if (a < 0)
        a += 2 * M_PI;
    int s = (int)(a / (2 * M_PI / KC_SLOTS)) % KC_SLOTS;
    if (s != 0 && s != 10)
        gtk_widget_grab_focus(k_entries[s]);
    return TRUE;
}

/* The slider: three stops with pictures on a rail from blue to green. */
static void k_glyph(cairo_t *cr, int mode, double x, double y, double s)
{
    cairo_set_line_width(cr, s * .09);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (mode == 0) {                    /* a stopwatch */
        rgb(cr, 0x0a64d6);
        cairo_arc(cr, x, y + s * .06, s * .32, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_rectangle(cr, x - s * .1, y - s * .42, s * .2, s * .1);
        cairo_fill(cr);
        cairo_move_to(cr, x, y + s * .06);
        cairo_line_to(cr, x, y - s * .14);
        cairo_stroke(cr);
    } else if (mode == 1) {             /* a wave */
        rgb(cr, 0x6d5fe0);
        cairo_move_to(cr, x - s * .4, y + s * .05);
        cairo_curve_to(cr, x - s * .25, y - s * .4, x - s * .1, y - s * .4, x, y);
        cairo_curve_to(cr, x + s * .1, y + s * .4, x + s * .25, y + s * .4, x + s * .4, y - s * .05);
        cairo_stroke(cr);
    } else {                            /* a leaf */
        rgb(cr, 0x2fae5a);
        cairo_move_to(cr, x - s * .32, y + s * .32);
        cairo_curve_to(cr, x - s * .32, y - s * .14, x - s * .04, y - s * .32, x + s * .36, y - s * .36);
        cairo_curve_to(cr, x + s * .36, y + s * .06, x + s * .1, y + s * .32, x - s * .32, y + s * .32);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, s * .05);
        cairo_move_to(cr, x - s * .28, y + s * .28);
        cairo_line_to(cr, x + s * .12, y - s * .1);
        cairo_stroke(cr);
    }
}

static double k_stop_x(double w, int i)
{
    double r = S(26);
    return i == 0 ? r + 2 : i == 1 ? w / 2 : w - r - 2;
}

static gboolean k_draw_rail(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double wd = gtk_widget_get_allocated_width(w), h = gtk_widget_get_allocated_height(w), cy = h / 2;
    double r = S(26), x0 = k_stop_x(wd, 0), x2 = k_stop_x(wd, 2);
    cairo_pattern_t *p = cairo_pattern_create_linear(x0, 0, x2, 0);
    cairo_pattern_add_color_stop_rgb(p, 0, 0x2f / 255.0, 0x6f / 255.0, 0xd6 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, .5, 0x6d / 255.0, 0x5f / 255.0, 0xe0 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, 0x2f / 255.0, 0xae / 255.0, 0x5a / 255.0);
    rounded(cr, x0, cy - S(5), x2 - x0, S(10), S(5));
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    for (int i = 0; i < 3; i++) {
        double x = k_stop_x(wd, i);
        if (i == kc.mode) {
            cairo_arc(cr, x, cy, r + S(4), 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0x0a / 255.0, 0x64 / 255.0, 0xd6 / 255.0, .22);
            cairo_fill(cr);
        }
        cairo_arc(cr, x, cy, r, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        rgb(cr, i == kc.mode ? 0x0a64d6 : 0xd4d2cb);
        cairo_set_line_width(cr, S(2));
        cairo_stroke(cr);
        k_glyph(cr, i, x, cy, S(30));
    }
    return TRUE;
}

static void k_set_mode(int m)
{
    if (m == kc.mode)
        return;
    kc.mode = m;
    gtk_widget_queue_draw(k_rail);
    for (int i = 0; i < 3; i++) {
        GtkStyleContext *sc = gtk_widget_get_style_context(k_mode_label[i]);
        if (i == m)
            gtk_style_context_add_class(sc, "kmode-on");
        else
            gtk_style_context_remove_class(sc, "kmode-on");
    }
    gtk_label_set_text(GTK_LABEL(k_mode_desc), k_mode_text[m]);
    k_rebuild_body();
    k_update_status();
}

static gboolean k_rail_click(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
    (void)unused;
    double wd = gtk_widget_get_allocated_width(w);
    int best = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(ev->x - k_stop_x(wd, i)) < fabs(ev->x - k_stop_x(wd, best)))
            best = i;
    k_set_mode(best);
    return TRUE;
}

/* A white rounded box, like the other panes' groups, for free-form content. */
static GtkWidget *k_box(GtkWidget *parent, const char *title, const char *sub)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "kbox");
    if (title) {
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_box_pack_start(GTK_BOX(h), label_new(title, "section", 0), FALSE, FALSE, 0);
        if (sub) {
            GtkWidget *s = label_new(sub, "footnote", 0);
            gtk_widget_set_valign(s, GTK_ALIGN_BASELINE);
            gtk_box_pack_start(GTK_BOX(h), s, TRUE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(parent), v, FALSE, FALSE, 0);
    return v;
}

static void k_mark(GtkWidget *w, const char *cls, int on)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    if (on)
        gtk_style_context_add_class(sc, cls);
    else
        gtk_style_context_remove_class(sc, cls);
}

/* --- the slots and the tasks --- */

static void k_slot_changed(GtkEditable *e, gpointer data)
{
    int s = GPOINTER_TO_INT(data);
    snprintf(kc.slots[s], KC_NAME, "%s", gtk_entry_get_text(GTK_ENTRY(e)));
    k_mark(GTK_WIDGET(e), "kbad", kc.slots[s][0] && !kc_good_name(kc.slots[s]));
    if (kc.slots[s][0] && kc_good_name(kc.slots[s]))
        kc_prog(&kc, kc.slots[s], 1);   /* a new name runs /bin/<name> until told otherwise */
    gtk_widget_queue_draw(k_wheel);
    for (int i = 0; i < KC_SLOTS; i++)
        gtk_widget_queue_draw(k_chips[i]);
    k_rebuild_tasks();
    k_update_status();
}

static GtkWidget *k_slot_row(int s, int left)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
    char num[4];
    snprintf(num, sizeof num, "%d", s);
    GtkWidget *n = label_new(num, "knum", left ? 0 : 1);
    gtk_widget_set_size_request(n, S(18), -1);
    GtkWidget *chip = k_chips[s] = gtk_drawing_area_new();
    gtk_widget_set_size_request(chip, S(9), S(18));
    gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
    g_signal_connect(chip, "draw", G_CALLBACK(k_draw_chip), GINT_TO_POINTER(s));
    GtkWidget *e = k_entries[s] = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(e), 12);
    if (s == 0 || s == 10) {
        gtk_entry_set_text(GTK_ENTRY(e), "OS (system)");
        gtk_widget_set_sensitive(e, FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(e), "kos");
        gtk_widget_set_tooltip_text(e, "The kernel and the desktop's own programs: every 10 ms, always.");
    } else {
        gtk_entry_set_max_length(GTK_ENTRY(e), KC_NAME - 1);
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), "free");
        gtk_entry_set_text(GTK_ENTRY(e), kc.slots[s]);
        k_mark(e, "kbad", kc.slots[s][0] && !kc_good_name(kc.slots[s]));
        g_signal_connect(e, "changed", G_CALLBACK(k_slot_changed), GINT_TO_POINTER(s));
    }
    if (left) {
        gtk_box_pack_start(GTK_BOX(h), e, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(h), chip, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), n, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(h), n, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), chip, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), e, TRUE, TRUE, 0);
    }
    return h;
}

/* Who a task may run as: nobody, the people with accounts, root. */
struct k_user {
    char name[33];
    unsigned uid, gid;
};
static struct k_user k_users[24];
static int k_nusers;

static void k_load_users(void)
{
    struct passwd *pw;
    k_nusers = 0;
    snprintf(k_users[k_nusers].name, sizeof k_users[0].name, "nobody");
    k_users[k_nusers].uid = k_users[k_nusers].gid = 65534;
    k_nusers++;
    setpwent();
    while ((pw = getpwent()) && k_nusers < 23)
        if (pw->pw_uid >= 1000 && pw->pw_uid < 60000) {
            snprintf(k_users[k_nusers].name, sizeof k_users[0].name, "%s", pw->pw_name);
            k_users[k_nusers].uid = pw->pw_uid;
            k_users[k_nusers].gid = pw->pw_gid;
            k_nusers++;
        }
    endpwent();
    snprintf(k_users[k_nusers].name, sizeof k_users[0].name, "root");
    k_users[k_nusers].uid = k_users[k_nusers].gid = 0;
    k_nusers++;
}

static void k_program_changed(GtkEditable *e, gpointer data)
{
    struct kc_task *t = kc_prog(&kc, data, 0);
    if (!t)
        return;
    snprintf(t->program, sizeof t->program, "%s", gtk_entry_get_text(GTK_ENTRY(e)));
    struct stat st;
    k_mark(GTK_WIDGET(e), "kbad", t->program[0] != '/' || stat(t->program, &st) || !(st.st_mode & 0111));
    k_update_status();
}

static void k_args_changed(GtkEditable *e, gpointer data)
{
    struct kc_task *t = kc_prog(&kc, data, 0);
    if (t)
        snprintf(t->args, sizeof t->args, "%s", gtk_entry_get_text(GTK_ENTRY(e)));
    k_update_status();
}

static void k_user_changed(GtkComboBox *c, gpointer data)
{
    struct kc_task *t = kc_prog(&kc, data, 0);
    int i = gtk_combo_box_get_active(c);
    if (t && i >= 0 && i < k_nusers) {
        t->uid = k_users[i].uid;
        t->gid = k_users[i].gid;
    }
    k_update_status();
}

static void k_boot_toggled(GtkToggleButton *b, gpointer data)
{
    struct kc_task *t = kc_prog(&kc, data, 0);
    if (t)
        t->start_at_boot = gtk_toggle_button_get_active(b);
    k_update_status();
}

static void k_restart_toggled(GtkToggleButton *b, gpointer data)
{
    struct kc_task *t = kc_prog(&kc, data, 0);
    if (t)
        t->restart = gtk_toggle_button_get_active(b);
    k_update_status();
}

static void k_choose_program(GtkButton *b, gpointer entry)
{
    (void)b;
    GtkWidget *d = gtk_file_chooser_dialog_new("Choose the program", GTK_WINDOW(window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "Cancel", GTK_RESPONSE_CANCEL, "Choose", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), "/usr/bin");
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (f)
            gtk_entry_set_text(GTK_ENTRY(entry), f);
        g_free(f);
    }
    gtk_widget_destroy(d);
}

static void k_rebuild_tasks(void)
{
    if (!k_tasks)
        return;
    gtk_container_foreach(GTK_CONTAINER(k_tasks), destroy_child, NULL);
    char names[KC_MAX_TASKS][KC_NAME];
    int n = kc_tasks(&kc, names);
    if (!n)
        gtk_box_pack_start(GTK_BOX(k_tasks), label_new("Name a slot on the wheel to add a task.", "blurb", 0), FALSE,
                           FALSE, 0);
    for (int i = 0; i < n; i++) {
        struct kc_task *t = kc_prog(&kc, names[i], 1);
        if (!t || !kc_good_name(names[i]))
            continue;
        char *key = g_strdup(t->name);  /* the handlers' data, freed with the row */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(5));
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "ktask");
        g_object_set_data_full(G_OBJECT(row), "task", key, g_free);

        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        int sl[KC_SLOTS], k = kc_task_slots(&kc, t->name, sl);
        GtkWidget *chip = gtk_drawing_area_new();
        gtk_widget_set_size_request(chip, S(11), S(11));
        gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
        g_signal_connect(chip, "draw", G_CALLBACK(k_draw_chip), GINT_TO_POINTER(sl[0]));
        gtk_box_pack_start(GTK_BOX(top), chip, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top), label_new(t->name, "section", 0), FALSE, FALSE, 0);
        char cad[64], info[160];
        kc_cadence(sl, k, cad, sizeof cad);
        size_t at = (size_t)snprintf(info, sizeof info, "%s · slot%s ", cad, k > 1 ? "s" : "");
        for (int j = 0; j < k && at < sizeof info; j++)
            at += (size_t)snprintf(info + at, sizeof info - at, "%s%d", j ? ", " : "", sl[j]);
        gtk_box_pack_start(GTK_BOX(top), label_new(info, "footnote", 0), FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(top), label_new("not started: myos-taskd comes in a later step", "footnote", 1), FALSE,
                         FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), top, FALSE, FALSE, 0);

        GtkWidget *l2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_widget_set_margin_start(l2, S(19));
        gtk_box_pack_start(GTK_BOX(l2), label_new("Program", "value", 0), FALSE, FALSE, 0);
        GtkWidget *prog = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(prog), KC_PROGRAM - 1);
        gtk_entry_set_text(GTK_ENTRY(prog), t->program);
        gtk_style_context_add_class(gtk_widget_get_style_context(prog), "kmono");
        struct stat st;
        k_mark(prog, "kbad", t->program[0] != '/' || stat(t->program, &st) || !(st.st_mode & 0111));
        g_signal_connect(prog, "changed", G_CALLBACK(k_program_changed), key);
        gtk_box_pack_start(GTK_BOX(l2), prog, TRUE, TRUE, 0);
        GtkWidget *choose = gtk_button_new_with_label("Choose…");
        g_signal_connect(choose, "clicked", G_CALLBACK(k_choose_program), prog);
        gtk_box_pack_start(GTK_BOX(l2), choose, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), l2, FALSE, FALSE, 0);

        GtkWidget *l3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_widget_set_margin_start(l3, S(19));
        gtk_box_pack_start(GTK_BOX(l3), label_new("Arguments", "value", 0), FALSE, FALSE, 0);
        GtkWidget *args = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(args), KC_ARGS - 2);
        gtk_entry_set_width_chars(GTK_ENTRY(args), 14);
        gtk_entry_set_text(GTK_ENTRY(args), t->args);
        gtk_style_context_add_class(gtk_widget_get_style_context(args), "kmono");
        g_signal_connect(args, "changed", G_CALLBACK(k_args_changed), key);
        gtk_box_pack_start(GTK_BOX(l3), args, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(l3), label_new("Run as", "value", 0), FALSE, FALSE, S(6));
        GtkWidget *who = gtk_combo_box_text_new();
        int sel = 0;
        for (int u = 0; u < k_nusers; u++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(who), k_users[u].name);
            if (k_users[u].uid == t->uid)
                sel = u;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(who), sel);
        g_signal_connect(who, "changed", G_CALLBACK(k_user_changed), key);
        gtk_box_pack_start(GTK_BOX(l3), who, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), l3, FALSE, FALSE, 0);

        GtkWidget *l4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(14));
        gtk_widget_set_margin_start(l4, S(19));
        GtkWidget *boot = gtk_check_button_new_with_label("Start at boot");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(boot), t->start_at_boot);
        g_signal_connect(boot, "toggled", G_CALLBACK(k_boot_toggled), key);
        GtkWidget *again = gtk_check_button_new_with_label("Restart if it stops");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(again), t->restart);
        g_signal_connect(again, "toggled", G_CALLBACK(k_restart_toggled), key);
        gtk_box_pack_start(GTK_BOX(l4), boot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(l4), again, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), l4, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(k_tasks), row, FALSE, FALSE, 0);
    }
    GtkWidget *foot = label_new("Only the program set here, started by the system, can use a task's slots. A program "
                                "that merely calls itself by a task's name gets nothing.", "footnote", 0);
    gtk_box_pack_start(GTK_BOX(k_tasks), foot, FALSE, FALSE, 0);
    gtk_widget_show_all(k_tasks);
}

static void k_budget_changed(GtkRange *r, gpointer label)
{
    kc.latency_budget_us = (unsigned)gtk_range_get_value(r);
    const struct kcstate *cs = kc_soft_cstate(&kcm, kc.latency_budget_us);
    char t[200];
    snprintf(t, sizeof t, "Wake within %u µs: the processor may rest down to %s (wakes in about %d µs). "
             "Nothing else saves power in this mode.", kc.latency_budget_us, cs->name, cs->wake_us);
    gtk_label_set_text(GTK_LABEL(label), t);
    k_update_status();
}

/* --- the power cards --- */

static gboolean k_rebuild_cards_idle(gpointer unused);

static void k_switch_toggled(GObject *sw, GParamSpec *ps, gpointer data)
{
    (void)ps;
    int bit = GPOINTER_TO_INT(data);
    if (gtk_switch_get_active(GTK_SWITCH(sw)))
        kc.power_on |= 1u << bit;
    else
        kc.power_on &= ~(1u << bit);
    g_idle_add(k_rebuild_cards_idle, NULL);   /* not from inside the switch's own signal */
    k_update_status();
}

static void k_spin_changed(GtkSpinButton *s, gpointer field)
{
    *(int *)field = gtk_spin_button_get_value_as_int(s);
    k_update_status();
}

static void k_cstate_changed(GtkComboBox *c, gpointer unused)
{
    (void)unused;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id)
        kc.cstate_max = atoi(id);
    k_update_status();
}

static GtkWidget *k_spin(int lo, int hi, int *field)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), *field);
    g_signal_connect(s, "value-changed", G_CALLBACK(k_spin_changed), field);
    return s;
}

/* A saving's own settings, inside its card once it is switched on. */
static void k_card_settings(GtkWidget *card, const struct kfeature *f)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(5));
    if (!strcmp(f->key, "cpu_speed")) {
        gtk_box_pack_start(GTK_BOX(h), k_spin(10, 100, &kc.cpu_min_pct), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), label_new("to", "footnote", 0), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), k_spin(10, 100, &kc.cpu_max_pct), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), label_new("% speed", "footnote", 0), FALSE, FALSE, 0);
    } else if (!strcmp(f->key, "deep_idle")) {
        gtk_box_pack_start(GTK_BOX(h), label_new("Deepest", "footnote", 0), FALSE, FALSE, 0);
        GtkWidget *c = gtk_combo_box_text_new();
        for (int i = 0; i < kc_ncstates(); i++) {
            const struct kcstate *cs = kc_cstate(i);
            if (!kc_cstate_ok(&kcm, cs->n))
                continue;
            char id[4];
            snprintf(id, sizeof id, "%d", cs->n);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, cs->name);
        }
        char cur[4];
        snprintf(cur, sizeof cur, "%d", kc.cstate_max);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), cur))
            gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
        g_signal_connect(c, "changed", G_CALLBACK(k_cstate_changed), NULL);
        gtk_box_pack_start(GTK_BOX(h), c, FALSE, FALSE, 0);
    } else if (!strcmp(f->key, "disk_spindown")) {
        gtk_box_pack_start(GTK_BOX(h), label_new("After", "footnote", 0), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), k_spin(1, 600, &kc.spindown_min), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), label_new("minutes", "footnote", 0), FALSE, FALSE, 0);
    } else {
        gtk_widget_destroy(h);
        return;
    }
    gtk_box_pack_start(GTK_BOX(card), h, FALSE, FALSE, 0);
}

static void k_rebuild_cards(void)
{
    if (!k_cards)
        return;
    gtk_container_foreach(GTK_CONTAINER(k_cards), destroy_child, NULL);
    for (int i = 0; i < kc_nfeatures(); i++) {
        const struct kfeature *f = kc_feature(i);
        const char *why = NULL;
        int ok = kc_feature_ok(&kcm, f, &why), on = ok && (kc.power_on >> f->bit & 1);
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(5));
        GtkStyleContext *sc = gtk_widget_get_style_context(card);
        gtk_style_context_add_class(sc, "kcard");
        gtk_style_context_add_class(sc, !ok ? "off" : on ? "on" : "idle");
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
        GtkWidget *name = label_new(f->label, "kcard-title", 0);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 18);
        gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
        GtkWidget *sw = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(sw), on);
        gtk_widget_set_sensitive(sw, ok);
        gtk_widget_set_valign(sw, GTK_ALIGN_START);
        g_signal_connect(sw, "notify::active", G_CALLBACK(k_switch_toggled), GINT_TO_POINTER(f->bit));
        gtk_box_pack_end(GTK_BOX(top), sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), top, FALSE, FALSE, 0);
        GtkWidget *what = label_new(ok ? f->what : why, ok ? "footnote" : "kwhy", 0);
        gtk_label_set_max_width_chars(GTK_LABEL(what), 24);
        gtk_box_pack_start(GTK_BOX(card), what, FALSE, FALSE, 0);
        if (ok == KC_MAYBE)                 /* only the kernel can tell: it skips what isn't there */
            gtk_box_pack_start(GTK_BOX(card), label_new("Checked when MyOS starts", "kmeta", 0), FALSE, FALSE, 0);
        if (on)
            k_card_settings(card, f);
        char meta[96];
        size_t at = (size_t)snprintf(meta, sizeof meta, "%s saves",
                                     f->saves >= 3 ? "●●●" : f->saves == 2 ? "●●○" : "●○○");
        if (f->wake_us)
            at += (size_t)snprintf(meta + at, sizeof meta - at, " · wakes ~%d µs", f->wake_us);
        if (f->slows_cpu && at < sizeof meta)
            snprintf(meta + at, sizeof meta - at, " · slower code");
        GtkWidget *m = label_new(meta, "kmeta", 0);
        gtk_widget_set_valign(m, GTK_ALIGN_END);
        gtk_box_pack_end(GTK_BOX(card), m, TRUE, TRUE, 0);
        gtk_grid_attach(GTK_GRID(k_cards), card, i % 3, i / 3, 1, 1);
    }
    gtk_widget_show_all(k_cards);
}

static gboolean k_rebuild_cards_idle(gpointer unused)
{
    (void)unused;
    k_rebuild_cards();
    return G_SOURCE_REMOVE;
}

/* --- the page --- */

static void k_rebuild_body(void)
{
    gtk_container_foreach(GTK_CONTAINER(k_body), destroy_child, NULL);
    k_tasks = k_cards = k_wheel = NULL;
    char names[KC_MAX_TASKS][KC_NAME];
    int n = kc_tasks(&kc, names);
    if (kc.mode == KC_POWER) {
        GtkWidget *b = k_box(k_body, "Time slots are off in this mode", NULL);
        char t[400];
        size_t at = (size_t)snprintf(t, sizeof t, "Every program takes its turn on the processor.");
        if (n) {
            at += (size_t)snprintf(t + at, sizeof t - at, " Your tasks (");
            for (int i = 0; i < n && at < sizeof t; i++)
                at += (size_t)snprintf(t + at, sizeof t - at, "%s%s", i ? ", " : "", names[i]);
            if (at < sizeof t)
                snprintf(t + at, sizeof t - at, ") still start after boot, as ordinary programs, and get their "
                         "slots back when you move the slider back.");
        }
        gtk_box_pack_start(GTK_BOX(b), label_new(t, "blurb", 0), FALSE, FALSE, 0);
        char sub[160];
        snprintf(sub, sizeof sub, "%s", kcm.family >= 0 ? kc_family_name(kcm.family) : kcm.cpu);
        b = k_box(k_body, "Savings on this computer", sub);
        if (kcm.virt)
            gtk_box_pack_start(GTK_BOX(b), label_new("This is a virtual machine: the host (your Mac) manages the "
                                                     "real processor's power, so most processor savings don't apply "
                                                     "here. The device savings still do.", "blurb", 0), FALSE, FALSE, 0);
        k_cards = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(k_cards), S(8));
        gtk_grid_set_row_spacing(GTK_GRID(k_cards), S(8));
        gtk_grid_set_column_homogeneous(GTK_GRID(k_cards), TRUE);
        gtk_box_pack_start(GTK_BOX(b), k_cards, FALSE, FALSE, 0);
        k_rebuild_cards();
    } else {
        GtkWidget *b = k_box(k_body, "Time slots", "Each wedge is 1 ms. The same name in several slots is one task.");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
        GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(3)), *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(3));
        gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
        for (int s = 19; s >= 10; s--)  /* beside their wedges: 19 at the top-left, 10 at the bottom */
            gtk_box_pack_start(GTK_BOX(left), k_slot_row(s, 1), FALSE, FALSE, 0);
        for (int s = 0; s <= 9; s++)
            gtk_box_pack_start(GTK_BOX(right), k_slot_row(s, 0), FALSE, FALSE, 0);
        k_wheel = gtk_drawing_area_new();
        gtk_widget_set_size_request(k_wheel, S(236), S(236));
        gtk_widget_set_valign(k_wheel, GTK_ALIGN_CENTER);
        gtk_widget_add_events(k_wheel, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(k_wheel, "draw", G_CALLBACK(k_draw_wheel), NULL);
        g_signal_connect(k_wheel, "button-press-event", G_CALLBACK(k_wheel_click), NULL);
        gtk_box_pack_start(GTK_BOX(row), left, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), k_wheel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), right, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(b), row, FALSE, FALSE, 0);

        b = k_box(k_body, "Tasks", "Started after boot, each running only in its own slots");
        k_tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
        gtk_box_pack_start(GTK_BOX(b), k_tasks, FALSE, FALSE, 0);
        k_rebuild_tasks();

        if (kc.mode == KC_SOFT) {
            b = k_box(k_body, "Latency budget", "How late a task may start because the processor was resting");
            GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(12));
            GtkWidget *l = label_new("", "footnote", 0);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
            GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 500, 1);
            gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
            gtk_widget_set_size_request(sc, S(200), -1);
            gtk_range_set_value(GTK_RANGE(sc), kc.latency_budget_us);
            g_signal_connect(sc, "value-changed", G_CALLBACK(k_budget_changed), l);
            gtk_box_pack_start(GTK_BOX(h), l, TRUE, TRUE, 0);
            gtk_box_pack_end(GTK_BOX(h), sc, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(b), h, FALSE, FALSE, 0);
            k_budget_changed(GTK_RANGE(sc), l);
        }
    }
    gtk_widget_show_all(k_body);
}

static void k_update_status(void)
{
    static struct kc_msgs m;            /* 13 KB: not on the stack */
    char t[400];
    kc_check(&kc, &kcm, 1, &m);
    k_mark(k_status, "error", m.nerr > 0);
    if (m.nerr)
        snprintf(t, sizeof t, "✕ %d problem%s: %s", m.nerr, m.nerr > 1 ? "s" : "", m.err[0]);
    else if (m.nwarn)
        snprintf(t, sizeof t, "! %s — you can still save", m.warn[0]);
    else if (kc_dev[0])
        snprintf(t, sizeof t, "✓ Ready to save, to myos-config (%s).", kc_dev);
    else
        snprintf(t, sizeof t, "✓ Ready, but this disk has no myos-config partition to save to.");
    gtk_label_set_text(GTK_LABEL(k_status), t);
    gtk_widget_set_sensitive(k_save, !m.nerr && kc_dev[0]);
}

/* Ask the helper; its whole answer comes back in *out (g_free it). */
static int k_helper(const char *request, char **out)
{
    char *argv[] = {KCONFIG_HELPER, NULL};
    GPid pid;
    int in, fd;
    *out = NULL;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &in, &fd, NULL,
                                  NULL)) {
        *out = g_strdup("Can't start " KCONFIG_HELPER ".");
        return -1;
    }
    size_t n = strlen(request), done = 0;
    while (done < n) {
        ssize_t w = write(in, request + done, n - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    close(in);
    GString *s = g_string_new(NULL);
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        g_string_append_len(s, buf, r);
    close(fd);
    struct helper_wait w = {g_main_loop_new(NULL, FALSE), -1};
    g_child_watch_add(pid, helper_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    *out = g_string_free(s, FALSE);
    return w.status == 0 ? 0 : -1;
}

/* What is on the disk now. */
static void k_load(void)
{
    char *reply;
    k_helper("read\n", &reply);
    kc_default(&kc);
    kc_dev[0] = 0;
    kc_cleared = 0;
    if (!strncmp(reply, "ok ", 3)) {
        char which = '?', err[200];
        unsigned gen = 0;
        sscanf(reply + 3, "%c %u %63s", &which, &gen, kc_dev);
        const char *text = strchr(reply, '\n');
        if (text && !kc_parse(text + 1, &kc, err, sizeof err))
            snprintf(kc_badge_text, sizeof kc_badge_text, "Saved: generation %u, copy %c of myos-config (%s).", gen,
                     which, kc_dev);
        else
            snprintf(kc_badge_text, sizeof kc_badge_text, "What's saved can't be read: %s", text ? err : reply);
    } else if (!strncmp(reply, "none ", 5)) {
        char path[64];
        if (!kc_find_partition(path, sizeof path))
            snprintf(kc_dev, sizeof kc_dev, "%s", path);
        snprintf(kc_badge_text, sizeof kc_badge_text, "Nothing saved yet: %s", reply + 5);
        kc_badge_text[strcspn(kc_badge_text, "\n")] = 0;
    } else
        snprintf(kc_badge_text, sizeof kc_badge_text, "%s", reply);
    g_free(reply);
    /* Savings this computer lacks (the disk may have come from another). */
    for (int i = 0; i < kc_nfeatures(); i++) {
        const char *why;
        const struct kfeature *f = kc_feature(i);
        if ((kc.power_on >> f->bit & 1) && !kc_feature_ok(&kcm, f, &why)) {
            kc.power_on &= ~(1u << f->bit);
            kc_cleared++;
        }
    }
}

static void k_show_badge(void)
{
    char t[400];
    snprintf(t, sizeof t, "%s%s\nTakes effect at the next start: the slots go to the tasks named here. "
             "The modes, starting the programs and the power savings come later "
             "(docs/KERNEL-CONFIG.md §10).", kc_badge_text,
             kc_cleared ? " Savings this computer lacks were switched off." : "");
    gtk_label_set_text(GTK_LABEL(k_badge), t);
}

static int ask_admin(const char *why, char *name, char *pass);

static void k_save_clicked(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    char admin[FIELD_MAX + 1], apass[FIELD_MAX + 1];
    if (!ask_admin("Settings wants to change how the kernel runs. Enter an administrator's name and password to "
                   "allow this.", admin, apass))
        return;
    kc.power_seen = 0;
    for (int i = 0; i < kc_nfeatures(); i++) {
        const char *why;
        if (kc_feature_ok(&kcm, kc_feature(i), &why))
            kc.power_seen |= 1u << kc_feature(i)->bit;
    }
    char *text = kc_format(&kc), *reply;
    char *req = g_strdup_printf("save\n%s\n%s\n%s", admin, apass, text);
    explicit_bzero(apass, sizeof apass);
    free(text);
    gtk_widget_set_sensitive(k_save, FALSE);
    int rc = k_helper(req, &reply);
    explicit_bzero(req, strlen(req));
    g_free(req);
    reply[strcspn(reply, "\n")] = 0;
    if (!rc && !strncmp(reply, "ok ", 3)) {
        char which = '?';
        unsigned gen = 0;
        sscanf(reply + 3, "%c %u %63s", &which, &gen, kc_dev);
        snprintf(kc_badge_text, sizeof kc_badge_text, "Saved: generation %u, copy %c of myos-config (%s).", gen,
                 which, kc_dev);
        kc_cleared = 0;
        k_show_badge();
        k_update_status();
    } else {
        k_update_status();
        char t[400];
        snprintf(t, sizeof t, "✕ Not saved: %s", reply);
        gtk_label_set_text(GTK_LABEL(k_status), t);
        k_mark(k_status, "error", 1);
    }
    g_free(reply);
}

static void k_revert_clicked(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    show("kernel");
}

static void fit_window_to(int wpts, int hpts);

static void page_kernel(GtkWidget *page)
{
    kc_detect(&kcm);
    k_load_users();
    k_load();
    banner(page, IC_KERNEL, "Kernel", "How the processor's time is shared, and how hard the computer saves power.");
    k_badge = label_new("", "footnote", .5);
    gtk_label_set_justify(GTK_LABEL(k_badge), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(page), k_badge, FALSE, FALSE, 0);
    k_show_badge();

    GtkWidget *mb = k_box(page, NULL, NULL);
    k_rail = gtk_drawing_area_new();
    gtk_widget_set_size_request(k_rail, -1, S(64));
    gtk_widget_add_events(k_rail, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(k_rail, "draw", G_CALLBACK(k_draw_rail), NULL);
    g_signal_connect(k_rail, "button-press-event", G_CALLBACK(k_rail_click), NULL);
    gtk_box_pack_start(GTK_BOX(mb), k_rail, FALSE, FALSE, 0);
    GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(names), TRUE);
    for (int i = 0; i < 3; i++) {
        k_mode_label[i] = label_new(k_mode_name[i], "kmode", i == 0 ? 0 : i == 1 ? .5 : 1);
        if (i == kc.mode)
            k_mark(k_mode_label[i], "kmode-on", 1);
        gtk_box_pack_start(GTK_BOX(names), k_mode_label[i], TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(mb), names, FALSE, FALSE, 0);
    k_mode_desc = label_new(k_mode_text[kc.mode], "blurb", 0);
    gtk_box_pack_start(GTK_BOX(mb), k_mode_desc, FALSE, FALSE, 0);

    k_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(14));
    gtk_box_pack_start(GTK_BOX(page), k_body, FALSE, FALSE, 0);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    k_status = label_new("", NULL, 0);
    gtk_box_pack_start(GTK_BOX(foot), k_status, TRUE, TRUE, 0);
    GtkWidget *revert = gtk_button_new_with_label("Revert");
    g_signal_connect(revert, "clicked", G_CALLBACK(k_revert_clicked), NULL);
    k_save = gtk_button_new_with_label("Save…");
    gtk_style_context_add_class(gtk_widget_get_style_context(k_save), "suggested-action");
    g_signal_connect(k_save, "clicked", G_CALLBACK(k_save_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(foot), k_save, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), revert, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), foot, FALSE, FALSE, 0);

    k_rebuild_body();
    k_update_status();
    fit_window_to(860, 720);            /* the wheel and its two columns of names */
}

struct page {
    const char *id, *title, *parent;
    void (*fill)(GtkWidget *page);
    int icon;               /* sidebar icon; -1: a sub-page, not in the sidebar */
    const char *keywords;   /* what the search field also finds it by */
};

static const struct page pages[] = {
    {"users", "Users & Groups", NULL, page_users, -2, "account people login password admin"},
    {"wifi", "Wi-Fi", NULL, page_wifi, IC_WIFI, "wifi wi-fi wireless wlan airport hotspot"},
    {"network", "Network", NULL, page_network, IC_NETWORK, "ethernet internet ip address dns"},
    {"general", "General", NULL, page_general, IC_GENERAL, "about storage disk date time clock"},
    {"kernel", "Kernel", NULL, page_kernel, IC_KERNEL,
     "kernel realtime real-time slots time tasks power saving savings cpu processor speed idle battery"},
    {"displays", "Displays", NULL, page_displays, IC_DISPLAYS,
     "scale scaling size text bigger larger smaller resolution screen monitor hidpi retina"},
    {"wallpaper", "Wallpaper", NULL, page_wallpaper, IC_WALLPAPER,
     "wallpaper background picture image photo desktop fill screen color"},
    {"mouse", "Mouse & Keyboard", NULL, page_mouse, IC_MOUSE,
     "pointer cursor arrow size bigger larger mouse keyboard shortcuts keys"},
    {"usersgroups", "Users & Groups", NULL, page_users, IC_USERS, "account people login password admin add new user"},
    {"ethernet", "Ethernet", "network", page_ethernet, -1, NULL},
    {"about", "About", "general", page_about, -1, NULL},
    {"storage", "Storage", "general", page_storage, -1, NULL},
    {"datetime", "Date & Time", "general", page_datetime, -1, NULL},
};
#define NPAGES (int)(sizeof pages / sizeof pages[0])

/* The narrowest the window can be is about SIDEBAR_W + 5 thumbnails + margins
   = 780 points: it must fit what 200% leaves of a 1920-pixel screen (960). */
#define SIDEBAR_W 210
static GtkWidget *window, *sidebar, *sidebar_box, *search, *content, *title, *back_btn, *fwd_btn, *scroller;
static const char *current = "displays";
static const char *history[64];
static int hpos = -1, hlen;
static int selecting;

static const struct page *find(const char *id)
{
    for (int i = 0; i < NPAGES; i++)
        if (!strcmp(pages[i].id, id))
            return &pages[i];
    return &pages[0];
}

/* The sidebar entry a page belongs to (a sub-page highlights its parent). */
static const char *top_of(const char *id)
{
    const struct page *p = find(id);
    return p->parent ? p->parent : p->id;
}

static void destroy_child(GtkWidget *w, gpointer unused)
{
    (void)unused;
    gtk_widget_destroy(w);
}

static void show(const char *id)
{
    const struct page *p = find(id);
    current = p->id;
    clock_label = looks_like = NULL;
    gtk_container_foreach(GTK_CONTAINER(content), destroy_child, NULL);
    gtk_box_set_spacing(GTK_BOX(content), S(14));
    gtk_widget_set_margin_start(content, S(24));
    gtk_widget_set_margin_end(content, S(24));
    gtk_widget_set_margin_top(content, S(18));
    gtk_widget_set_margin_bottom(content, S(24));
    p->fill(content);
    gtk_widget_show_all(content);
    gtk_label_set_text(GTK_LABEL(title), p->title);
    gtk_widget_set_sensitive(back_btn, hpos > 0);
    gtk_widget_set_sensitive(fwd_btn, hpos < hlen - 1);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller)), 0);

    /* Keep the sidebar in step, without its signal coming back here. */
    selecting = 1;
    const char *top = top_of(id);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(sidebar));
    for (GList *r = rows; r; r = r->next) {
        const char *rid = g_object_get_data(r->data, "page");
        if (rid && !strcmp(rid, top))
            gtk_list_box_select_row(GTK_LIST_BOX(sidebar), r->data);
    }
    g_list_free(rows);
    selecting = 0;
}

static void go(const char *id, gboolean record)
{
    if (record) {
        if (hpos >= 0 && !strcmp(history[hpos], id))
            return;
        if (hpos == (int)(sizeof history / sizeof history[0]) - 1) {
            memmove(history, history + 1, sizeof history - sizeof history[0]);
            hpos--;
        }
        history[++hpos] = find(id)->id;
        hlen = hpos + 1;
    }
    show(id);
}

static void back_clicked(GtkButton *b, gpointer u)
{
    (void)b, (void)u;
    if (hpos > 0)
        show(history[--hpos]);
}

static void fwd_clicked(GtkButton *b, gpointer u)
{
    (void)b, (void)u;
    if (hpos < hlen - 1)
        show(history[++hpos]);
}

static void sidebar_selected(GtkListBox *box, GtkListBoxRow *row, gpointer u)
{
    (void)box, (void)u;
    if (selecting || !row)
        return;
    const char *id = g_object_get_data(G_OBJECT(row), "page");
    if (id && strcmp(top_of(current), id))
        go(id, TRUE);
}

static gboolean sidebar_filter(GtkListBoxRow *row, gpointer u)
{
    (void)u;
    const char *q = gtk_entry_get_text(GTK_ENTRY(search));
    const char *id = g_object_get_data(G_OBJECT(row), "page");
    if (!*q)
        return TRUE;
    if (!id)
        return FALSE;               /* the gaps between groups */
    const struct page *p = find(id);
    char *hay = g_utf8_strdown(p->keywords ? p->keywords : "", -1);
    char *t = g_utf8_strdown(p->title, -1), *needle = g_utf8_strdown(q, -1);
    gboolean hit = strstr(t, needle) || strstr(hay, needle);
    g_free(hay), g_free(t), g_free(needle);
    return hit;
}

static void search_changed(GtkSearchEntry *e, gpointer u)
{
    (void)e, (void)u;
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(sidebar));
}

static void sidebar_gap(void)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "gap");
    gtk_container_add(GTK_CONTAINER(sidebar), row);
}

static void build_sidebar(void)
{
    gtk_container_foreach(GTK_CONTAINER(sidebar), destroy_child, NULL);
    /* The user's card at the top, as the Mac has the Apple Account. */
    struct passwd *pw = getpwuid(getuid());
    const char *who = pw ? pw->pw_name : "user";
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(10));
    gtk_box_pack_start(GTK_BOX(h), avatar_new(who, 36), FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(v), label_new(who, NULL, 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), label_new("Users & Groups", "subtitle", 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), h);
    g_object_set_data(G_OBJECT(row), "page", (gpointer) "users");
    gtk_container_add(GTK_CONTAINER(sidebar), row);

    /* The Mac's groups, in its order: Wi-Fi and Network first; General and
       Displays; Users & Groups with the security panes; the input devices
       (Keyboard, Trackpad there) last. */
    static const char *const groups[][4] = {{"wifi", "network"},
                                            {"general", "kernel", "displays", "wallpaper"},
                                            {"usersgroups"},
                                            {"mouse"}};
    for (size_t gi = 0; gi < sizeof groups / sizeof groups[0]; gi++) {
        sidebar_gap();
        for (int j = 0; j < 4 && groups[gi][j]; j++) {
            const struct page *p = find(groups[gi][j]);
            h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
            gtk_box_pack_start(GTK_BOX(h), icon_new(p->icon, 20), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(h), label_new(p->title, NULL, 0), TRUE, TRUE, 0);
            row = gtk_list_box_row_new();
            gtk_container_add(GTK_CONTAINER(row), h);
            g_object_set_data(G_OBJECT(row), "page", (gpointer)p->id);
            gtk_container_add(GTK_CONTAINER(sidebar), row);
        }
    }
    gtk_widget_show_all(sidebar);
}

/* Rebuild everything at the new size: our pixel sizes follow k, the fonts
   follow the DPI myos-xsettings now publishes. */
static void relayout(void)
{
    apply_css();
    gtk_widget_set_size_request(sidebar_box, S(SIDEBAR_W), -1);
    build_sidebar();
    show(current);
}

/* The window's size at this scale, but never more than the screen holds
   (200% on a 1920x1200 screen leaves 960x600 to work in), and on screen. */
static void fit_window_to(int wpts, int hpts)
{
    int wsf = effective() >= 200 ? 2 : 1;
    int maxw = screen_w * 94 / 100 / wsf, maxh = screen_h * 86 / 100 / wsf;
    int w = S(wpts) < maxw ? S(wpts) : maxw, h = S(hpts) < maxh ? S(hpts) : maxh;
    int x, y;
    gtk_window_resize(GTK_WINDOW(window), w, h);
    gtk_window_get_position(GTK_WINDOW(window), &x, &y);
    if (x + w > screen_w / wsf || y + h > screen_h * 92 / 100 / wsf)
        gtk_window_move(GTK_WINDOW(window), (screen_w / wsf - w) / 2, (screen_h * 92 / 100 / wsf - h) / 2);
}

static void fit_window(void)
{
    fit_window_to(780, 560);
}

static void set_scale(int pct)
{
    /* Picking the default means "automatic": it follows the screen if the
       screen changes. */
    int c = pct == auto_pct ? 0 : pct;
    if (c == chosen && pct == effective())
        return;
    chosen = c;
    if (scale_write_conf(chosen) < 0)
        g_warning("can't write ~/.config/myos/display.conf");
    Display *d = gdk_x11_get_default_xdisplay();
    long v = chosen;
    XChangeProperty(d, DefaultRootWindow(d), a_scale, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(d);
    k = k_for(effective());
    relayout();
    fit_window();
}

static void set_pointer(int pct)
{
    if (pct == pointer)
        return;
    pointer = pct;
    if (pointer_write_conf(pointer) < 0)
        g_warning("can't write ~/.config/myos/mouse.conf");
    Display *d = gdk_x11_get_default_xdisplay();
    long v = pointer;
    XChangeProperty(d, DefaultRootWindow(d), a_pointer, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(d);
    show(current);
}

int main(int argc, char **argv)
{
    const char *start = NULL;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            start = argv[i];            /* myos-settings displays */
    gtk_init(&argc, &argv);
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default()))
        return 1;
    read_scale();
    read_pointer();
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    apply_css();

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Settings");
    gtk_window_set_wmclass(GTK_WINDOW(window), "myos-settings", "Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "settings");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), outer);

    /* Left: search, then the panes. */
    GtkWidget *left = sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(left), "sidebar");
    gtk_widget_set_size_request(left, S(SIDEBAR_W), -1);
    search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search");
    g_signal_connect(search, "search-changed", G_CALLBACK(search_changed), NULL);
    gtk_box_pack_start(GTK_BOX(left), search, FALSE, FALSE, 0);
    GtkWidget *sscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    sidebar = gtk_list_box_new();
    gtk_list_box_set_filter_func(GTK_LIST_BOX(sidebar), sidebar_filter, NULL, NULL);
    g_signal_connect(sidebar, "row-selected", G_CALLBACK(sidebar_selected), NULL);
    gtk_container_add(GTK_CONTAINER(sscroll), sidebar);
    gtk_box_pack_start(GTK_BOX(left), sscroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), left, FALSE, FALSE, 0);

    /* Right: the title bar with back and forward, then the pane. */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right), "pane");
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(header), "header");
    back_btn = gtk_button_new_with_label("\u2039");
    fwd_btn = gtk_button_new_with_label("\u203a");
    gtk_widget_set_tooltip_text(back_btn, "Back");
    gtk_widget_set_tooltip_text(fwd_btn, "Forward");
    g_signal_connect(back_btn, "clicked", G_CALLBACK(back_clicked), NULL);
    g_signal_connect(fwd_btn, "clicked", G_CALLBACK(fwd_clicked), NULL);
    title = label_new("", "title", 0);
    gtk_box_pack_start(GTK_BOX(header), back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 8);
    gtk_box_pack_start(GTK_BOX(right), header, FALSE, FALSE, 0);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scroller), content);
    gtk_box_pack_start(GTK_BOX(right), scroller, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), right, TRUE, TRUE, 0);

    build_sidebar();
    g_timeout_add_seconds(1, tick, NULL);
    fit_window();
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_widget_show_all(window);
    go(start ? find(start)->id : "displays", TRUE);
    gtk_main();
    return 0;
}
