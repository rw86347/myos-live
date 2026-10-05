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
 * (About, Storage, Date & Time), Displays; Users & Groups (Add User…, with
 * an administrator's password: myos-adduser.c); Mouse & Keyboard. Wi-Fi is
 * listed although MyOS can't drive Wi-Fi yet, and says so.
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
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "scale.h"

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
            IC_MOUSE, IC_WIFI };

static const uint32_t icon_colour[] = {
    [IC_NETWORK] = 0x2d7cf6, [IC_GENERAL] = 0x8e8e93, [IC_DISPLAYS] = 0x1c9ef0, [IC_USERS] = 0x5e5ce6,
    [IC_ABOUT] = 0x8e8e93,   [IC_STORAGE] = 0x8e8e93, [IC_CLOCK] = 0x2d7cf6,    [IC_ETHERNET] = 0x2d7cf6,
    [IC_MOUSE] = 0x8e8e93,   [IC_WIFI] = 0x2d7cf6,
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
    gtk_css_provider_load_from_data(css, s, -1, NULL);
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
   MyOS has no Wi-Fi driver yet (ROADMAP: WiFi comes last), so this pane
   shows where Wi-Fi will be and says so. A Linux kernel lists its wireless
   cards in /proc/net/wireless (two header lines, then "wlan0: ..."); the
   first one found is named, so the pane is right the day a driver lands. */
static int wifi_card(char *out, size_t size)
{
    char line[256];
    int n = 0, found = 0;
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f))
        if (++n > 2 && strchr(line, ':')) {
            char *p = line + strspn(line, " ");
            p[strcspn(p, ":")] = 0;
            snprintf(out, size, "%s", p);
            found = 1;
        }
    fclose(f);
    return found;
}

static void page_wifi(GtkWidget *page)
{
    char card[32], *s;
    int have = wifi_card(card, sizeof card);
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), FALSE);
    gtk_widget_set_sensitive(sw, FALSE);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    GtkWidget *g = group_new(page, NULL);
    s = have ? g_strdup_printf("%s found; MyOS can't drive it yet", card) : g_strdup("No Wi-Fi hardware found");
    row_link(g, IC_WIFI, "Wi-Fi", s, sw, NULL);
    g_free(s);
    g = group_new(page, "Known Networks");
    row_add(g, label_new("None", "value", 0));
    g = group_new(page, "Other Networks");
    row_add(g, label_new("Wi-Fi is off.", "value", 0));
    gtk_box_pack_start(GTK_BOX(page),
                       label_new("MyOS has no Wi-Fi drivers yet, so Wi-Fi can't be turned on. "
                                 "Ethernet works: see Network.", "footnote", 0),
                       FALSE, FALSE, 0);
}

static void page_network(GtkWidget *page)
{
    char ip[64];
    int up = our_address(ip, sizeof ip);
    GtkWidget *g = group_new(page, NULL);
    row_link(g, IC_ETHERNET, "Ethernet", NULL, status_dot(up), "ethernet");
    row_link(g, IC_WIFI, "Wi-Fi", NULL, label_new("Not available", "value", 1), "wifi");
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
static int ask_admin(char *name, char *pass)
{
    GtkWidget *grid, *error;
    GtkWidget *d = form_dialog("Settings", "Settings wants to add a user. Enter an administrator's name and "
                               "password to allow this.", "Unlock", &grid, &error);
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
    if (!ask_admin(admin, apass))
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

/* --- the window ------------------------------------------------------------------------------- */

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
    {"displays", "Displays", NULL, page_displays, IC_DISPLAYS,
     "scale scaling size text bigger larger smaller resolution screen monitor hidpi retina"},
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
    static const char *const groups[][4] = {{"wifi", "network"}, {"general", "displays"}, {"usersgroups"}, {"mouse"}};
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
static void fit_window(void)
{
    int wsf = effective() >= 200 ? 2 : 1;
    int maxw = screen_w * 94 / 100 / wsf, maxh = screen_h * 86 / 100 / wsf;
    int w = S(780) < maxw ? S(780) : maxw, h = S(560) < maxh ? S(560) : maxh;
    int x, y;
    gtk_window_resize(GTK_WINDOW(window), w, h);
    gtk_window_get_position(GTK_WINDOW(window), &x, &y);
    if (x + w > screen_w / wsf || y + h > screen_h * 92 / 100 / wsf)
        gtk_window_move(GTK_WINDOW(window), (screen_w / wsf - w) / 2, (screen_h * 92 / 100 / wsf - h) / 2);
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
