/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-files — My Files, the file explorer, exactly as the Golden Hour
 * mockup draws it (assets/themes/golden-hour/mockup.png): menu bar, a
 * toolbar with round Back / Forward / Up buttons and the address box, the
 * sand-coloured task pane with its two cards, big icons on white, and the
 * status bar. IceWM draws the title bar, from the same theme.
 *
 * Every number here is the mockup's: make-theme.py draws the window at
 * (wx, wy) and its inside starts one pixel in and 30 down (the title bar),
 * so "x = 199" here is the mockup's wx + 200. The program draws in those
 * units and multiplies by the desktop's scale (1.5 at 150%), so it is the
 * mockup at any scale. GTK supplies only the window, the mouse and the
 * keyboard; all drawing is cairo, and the icons are the theme's own SVGs
 * drawn by librsvg — the same library that drew the mockup. Text is
 * DejaVu Sans (the mockup's Verdana can't be shipped).
 *
 * What it does: browse folders; open files with the program for them;
 * Back, Forward, Up; type a path; make, rename, copy, paste and delete
 * (to ~/Trash — the same can `rm` uses) files and folders; Disks
 * (the mounted file systems), Network (Settings) and Terminal (here).
 *
 * One addition to the mockup, from the Mac's Finder: a View button right
 * of Up whose menu lays the files out as Icons (the mockup), a List with
 * dates, sizes and kinds, Columns (one per folder on the way down) or a
 * Gallery (a big preview over a strip of icons). Pictures use a thumbnail
 * of the file itself, cached in a hidden sidecar named ._file (the name
 * the Mac uses on a disk that isn't HFS). A PDF's sidecar is its first
 * page — Columns' far-right pane and the icon both draw that.
 *
 * Usage: myos-files [folder]
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <librsvg/rsvg.h>
#include <poppler.h>
#include <X11/Xatom.h>

#include "../myos-heic/heif-pixbuf.h"
#include "scale.h"

/* --- the theme's colours (make-theme.py's palette, C[...]) ----------------- */
#define PANEL_WHITE 0xfbfbf9
#define SAND        0xf9e6c1
#define SAND_DEEP   0xefd3a0
#define MENU_BAR    0xefece4
#define BORDER      0xc9c2b2
#define TEXT        0x1d2430
#define TEXT_SOFT   0x5b6573
#define LABEL_TEAL  0x1f6d72
#define TITLE_MID   0x1f7f86
#define TITLE_EDGE  0x0a4048
#define TEAL_TOP    0xd6f6f7
#define TEAL_BOTTOM 0x5cc3cb
#define TEAL_RIM    0x2b8a92
#define AMBER_MID   0xf08a12
#define DISABLED    0x9aa3ad

/* --- the mockup's layout, inside the window ------------------------------- */
#define DEF_W     568       /* the mockup's window, 570x400, less frame and title */
#define DEF_H     370
#define MENU_H    22
#define TOOL_Y    22
#define BODY_Y    56        /* below the toolbar */
#define STATUS_H  24
#define PANE_W    170
#define GRID_X    199       /* the first file's cell */
#define GRID_Y    (BODY_Y + 16)
#define CELL_W    120
#define CELL_H    100
#define SCROLL_W  17
#define VIEW_X    238       /* the View button, right of Up (not in the mockup) */
#define VIEW_W    40
#define ADDR_X    288       /* the address box: the mockup's 269, moved for View */
#define LHEAD_H   22        /* List: the column titles */
#define LROW_H    22
#define COL_W     200       /* Columns: one folder */
#define CROW_H    22
#define MAXCOL    32
#define STRIP_H   80        /* Gallery: the strip of icons along the bottom */
#define PREVIEW_MAX (40 << 20)  /* bigger pictures aren't drawn in previews */
#define PDF_THUMB_MAX (80 << 20) /* poppler only paints page 1; the file can be bigger */
#define THUMB_PX  256       /* sidecar PNG: fills Columns' 140-unit preview */

#define FONT "DejaVu Sans"
#define ICONS "/usr/share/myos/icons/"
#define DISKS ":disks"      /* the Disks view's place in the history */

/* --- state ------------------------------------------------------------------ */
struct item {
    char *name;
    const char *icon;
    int dir;
    long long size;
    time_t mtime;           /* to notice a file changed in place */
    int exec;               /* a regular file anyone may run */
    int pic;                /* a picture: the icon is a thumbnail of it */
};

/* How the files are laid out: the View button's four choices. */
enum { V_ICONS, V_LIST, V_COLUMNS, V_GALLERY };
static const char *const VIEW_NAMES[] = { "icons", "list", "columns", "gallery" };
static int vmode = V_ICONS;

/* Columns: the folders from col_root down to the one shown (anc), and
   what is in the selected folder (peek), each with its own selection and
   scroll. The folder shown is the column with the keyboard, so the menus
   act on it, as everywhere else. */
struct col { char path[PATH_MAX]; struct item *items; int n, sel; double scroll; };
static struct col anc[MAXCOL], peek;
static int nanc;
static char col_root[PATH_MAX];

static GtkWidget *win, *da;
static int desk_scale = 100;          /* percent, from myos-xsettings */
static Atom a_scale, a_scale_auto;
static double W = DEF_W, H = DEF_H;   /* the window, in mockup units */
static double K = 1;                  /* GTK pixels per mockup unit */

static char cwd[PATH_MAX];            /* the folder shown, or DISKS */
static struct item *items;
static int nitems, sel = -1;
static double press_x, press_y;
static int dragging;
static int show_hidden;
static GPtrArray *back_hist, *fwd_hist;
static char clip[PATH_MAX];           /* Copy remembers a path; Paste copies it here */
static time_t dir_mtime;
static char status_msg[256];
static guint status_timer;

static double scroll, max_scroll;     /* the file area's scroll, mockup units */
static int cols = 1, has_scroll;
static int active = 1;                /* the window has the focus */

/* What the mouse is over, and what it pressed. */
enum { T_NONE, T_MENU, T_TOOL, T_ADDR, T_TASK, T_FILE, T_FILES, T_THUMB, T_TRACK, T_VIEW,
       T_COL };     /* T_COL: idx = column * COL_IDX + row; column MAXCOL is peek */
#define COL_IDX 1000000
struct target { int kind, idx; };
static struct target hover, press;
static double drag_y, drag_scroll;

/* A one-line text editor: the address box, and a file name being renamed. */
struct edit {
    int on;
    char s[PATH_MAX];
    int caret;          /* byte offset */
    int all;            /* all of it is selected: typing replaces it */
};
static struct edit addr, ren;
static int ren_idx = -1;

/* Actions: from the menus, the task pane, the toolbar and the keyboard. */
enum { A_NONE, A_OPEN, A_NEWFOLDER, A_RENAME, A_COPY, A_PASTE, A_DELETE, A_PUTBACK,
       A_EMPTY, A_CLOSE,
       A_HIDDEN, A_REFRESH, A_TERMINAL, A_SETTINGS, A_ABOUT, A_DISKS, A_NETWORK,
       A_HOME, A_BACK, A_FORWARD, A_UP, A_V_ICONS, A_V_LIST, A_V_COLUMNS, A_V_GALLERY };

struct task { const char *icon; char label[48]; int act; };
static struct { const char *title; double y, h; int first, n; } groups[2];
static struct task tasks[12];
static int ngroups, ntasks;
static int enabled(int a);

static void reload(const char *keep);
static void navigate(const char *path, int remember);
static void watch_cwd(void);
static void col_free_all(void);
static void do_action(int a);
static void join(char *out, size_t size, const char *dir, const char *name);
static int thumb_make(const char *path, time_t mtime);
static void thumbs_kick(void);
static void note_dir_now(void);
static void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void redraw(void) { gtk_widget_queue_draw(da); }

/* --- colours, shapes, text, icons ----------------------------------------- */
static void rgb(cairo_t *cr, unsigned c, double a)
{
    cairo_set_source_rgba(cr, (c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0, a);
}

/* An SVG <rect rx=...>: corners of radius r. */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r <= 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* A top-to-bottom (or left-to-right) gradient over the box, as SVG's
   objectBoundingBox gradients in make-theme.py run. */
static cairo_pattern_t *grad(double x, double y, double w, double h, int across,
                             unsigned c0, unsigned c1)
{
    cairo_pattern_t *p = across ? cairo_pattern_create_linear(x, 0, x + w, 0)
                                : cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgb(p, 0, (c0 >> 16 & 255) / 255.0, (c0 >> 8 & 255) / 255.0, (c0 & 255) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, (c1 >> 16 & 255) / 255.0, (c1 >> 8 & 255) / 255.0, (c1 & 255) / 255.0);
    return p;
}

/* make-theme.py's draw_bar: a filled, outlined, rounded box. */
static void bar(cairo_t *cr, double x, double y, double w, double h, double r,
                cairo_pattern_t *fill, unsigned solid, int border, double bw)
{
    rrect(cr, x, y, w, h, r);
    if (fill)
        cairo_set_source(cr, fill);
    else
        rgb(cr, solid, 1);
    cairo_fill_preserve(cr);
    if (border >= 0) {
        rgb(cr, (unsigned)border, 1);
        cairo_set_line_width(cr, bw);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }
    if (fill)
        cairo_pattern_destroy(fill);
}

/* Text the way the mockup's rsvg drew it: grey anti-aliasing, no hinting,
   so the shapes and widths are the same at every scale. */
static PangoContext *measure_ctx;

static void font_options(PangoContext *pc)
{
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pc, fo);
    cairo_font_options_destroy(fo);
}

static PangoLayout *layout(PangoContext *pc, const char *s, double size, int bold, double maxw)
{
    PangoLayout *l = pango_layout_new(pc);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    if (bold)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s, -1);
    if (maxw > 0) {
        pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    return l;
}

static double text_width(const char *s, double size, int bold, double maxw)
{
    PangoLayout *l = layout(measure_ctx, s, size, bold, maxw);
    PangoRectangle r;
    pango_layout_get_extents(l, NULL, &r);
    g_object_unref(l);
    return r.width / (double)PANGO_SCALE;
}

/* SVG <text>: y is the baseline; anchor 0 = start, 1 = middle. Returns the
   width drawn. */
static double text(cairo_t *cr, double x, double y, const char *s, double size,
                   unsigned color, int bold, int middle, double maxw)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    font_options(pango_layout_get_context(l));
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    if (bold)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s, -1);
    if (maxw > 0) {
        pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    PangoRectangle r;
    pango_layout_get_extents(l, NULL, &r);
    double w = r.width / (double)PANGO_SCALE;
    rgb(cr, color, 1);
    cairo_move_to(cr, middle ? x - w / 2 : x, y - pango_layout_get_baseline(l) / (double)PANGO_SCALE);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* The theme's icons, drawn once per size from their SVGs and kept. */
static struct { char name[32]; int px; cairo_surface_t *s; } icache[128];
static int nicache;

static cairo_surface_t *icon_surface(const char *name, int px)
{
    for (int i = 0; i < nicache; i++)
        if (icache[i].px == px && !strcmp(icache[i].name, name))
            return icache[i].s;
    if (nicache == (int)G_N_ELEMENTS(icache)) {
        for (int i = 0; i < nicache; i++)
            cairo_surface_destroy(icache[i].s);
        nicache = 0;
    }
    char path[256];
    snprintf(path, sizeof path, ICONS "%s.svg", name);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
    if (h) {
        cairo_t *c = cairo_create(s);
        RsvgRectangle vp = { 0, 0, px, px };
        rsvg_handle_render_document(h, c, &vp, NULL);
        cairo_destroy(c);
        g_object_unref(h);
    }
    snprintf(icache[nicache].name, sizeof icache[0].name, "%s", name);
    icache[nicache].px = px;
    icache[nicache].s = s;
    nicache++;
    return s;
}

static void icon_flush(void)
{
    for (int i = 0; i < nicache; i++)
        cairo_surface_destroy(icache[i].s);
    nicache = 0;
}

/* The icon's top-left lands on a whole pixel, so it stays sharp at 150%. */
static void icon(cairo_t *cr, const char *name, double x, double y, double size, double alpha)
{
    int gs = gtk_widget_get_scale_factor(da);
    int px = (int)lround(size * K * gs);
    if (px < 1)
        return;
    cairo_surface_t *s = icon_surface(name, px);
    double ux = x, uy = y;
    cairo_user_to_device(cr, &ux, &uy);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_translate(cr, round(ux * gs) / gs, round(uy * gs) / gs);
    cairo_scale(cr, 1.0 / gs, 1.0 / gs);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

/* Thumbnails the way the Mac does on a disk that isn't HFS: a hidden
   sidecar next to the file, named ._name (AppleDouble's name). Ours is a
   small PNG of the picture, so the next visit does not decode the original.
   The explorer never lists these files. */
static void sidecar_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(out, size, "%.*s._%s", (int)(slash - path + 1), path, slash + 1);
    else
        snprintf(out, size, "._%s", path);
}

static int is_sidecar(const char *name)
{
    return !strncmp(name, "._", 2) || !strcmp(name, ".DS_Store");
}

/* Kept next to the sidecar code so the icon path does not depend on
   file_icon's later tables (or on the pic flag surviving the struct).
   PDF is here because its first page lives in the same sidecar. */
static int name_is_pdf(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && !strcasecmp(dot + 1, "pdf");
}

static int name_is_pic(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return 0;
    return !strcasecmp(dot + 1, "jpg") || !strcasecmp(dot + 1, "jpeg") ||
           !strcasecmp(dot + 1, "jpe") || !strcasecmp(dot + 1, "png") ||
           !strcasecmp(dot + 1, "gif") || !strcasecmp(dot + 1, "bmp") ||
           !strcasecmp(dot + 1, "webp") || !strcasecmp(dot + 1, "tif") ||
           !strcasecmp(dot + 1, "tiff") || !strcasecmp(dot + 1, "heic") ||
           !strcasecmp(dot + 1, "heif") || !strcasecmp(dot + 1, "avif") ||
           !strcasecmp(dot + 1, "svg") || !strcasecmp(dot + 1, "ico") ||
           !strcasecmp(dot + 1, "pdf");
}

/* HEIC/HEIF/AVIF through libheif; everything else through gdk-pixbuf. */
static GdkPixbuf *pixbuf_at_scale(const char *path, int w, int h)
{
    guchar head[32] = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    gsize n = 0;
    if (fd >= 0) {
        ssize_t got = read(fd, head, sizeof head);
        close(fd);
        if (got > 0)
            n = (gsize)got;
    }
    if (myos_heif_sniff(head, n) || myos_heif_path(path))
        return myos_heif_pixbuf_from_file_at_scale(path, w, h, TRUE, NULL);
    return gdk_pixbuf_new_from_file_at_scale(path, w, h, TRUE, NULL);
}

/* In memory: the sidecar we already read, so a redraw does not open it again. */
static struct {
    char path[PATH_MAX];
    time_t mtime;
    GdkPixbuf *pb;          /* the picture, already small */
    int failed;
} tcache[96];
static int ntcache, tclock;
static guint thumb_src;
static int thumbs_this_frame;           /* at most one decode per redraw */

static void tcache_put(const char *path, time_t mtime, GdkPixbuf *pb, int failed)
{
    for (int i = 0; i < ntcache; i++)
        if (!strcmp(tcache[i].path, path)) {
            if (tcache[i].pb)
                g_object_unref(tcache[i].pb);
            tcache[i].pb = pb ? g_object_ref(pb) : NULL;
            tcache[i].mtime = mtime;
            tcache[i].failed = failed;
            return;
        }
    int i = ntcache < (int)G_N_ELEMENTS(tcache) ? ntcache++ : (tclock++ % (int)G_N_ELEMENTS(tcache));
    if (tcache[i].pb)
        g_object_unref(tcache[i].pb);
    snprintf(tcache[i].path, sizeof tcache[0].path, "%s", path);
    tcache[i].mtime = mtime;
    tcache[i].pb = pb ? g_object_ref(pb) : NULL;
    tcache[i].failed = failed;
}

static GdkPixbuf *tcache_get(const char *path, time_t mtime, int *failed)
{
    if (failed)
        *failed = 0;
    for (int i = 0; i < ntcache; i++)
        if (!strcmp(tcache[i].path, path) && tcache[i].mtime == mtime) {
            if (failed)
                *failed = tcache[i].failed;
            return tcache[i].pb;
        }
    return NULL;
}

/* Load the sidecar if it is at least as new as the picture. */
static GdkPixbuf *thumb_load_side(const char *path, time_t mtime)
{
    char side[PATH_MAX];
    struct stat st;
    sidecar_of(path, side, sizeof side);
    if (stat(side, &st) || st.st_mtime < mtime)
        return NULL;
    return gdk_pixbuf_new_from_file(side, NULL);
}

static GdkPixbuf *thumb_pixbuf(const char *path, time_t mtime)
{
    int failed = 0;
    GdkPixbuf *pb = tcache_get(path, mtime, &failed);
    if (pb || failed)
        return pb;
    pb = thumb_load_side(path, mtime);
    if (pb) {
        tcache_put(path, mtime, pb, 0);
        g_object_unref(pb);
        return tcache_get(path, mtime, NULL);
    }
    /* The first picture in view is decoded here so you see it on the first
       paint. The rest are done in idle, one at a time. */
    if (thumbs_this_frame < 1 && thumb_make(path, mtime)) {
        thumbs_this_frame++;
        note_dir_now();
        return tcache_get(path, mtime, NULL);
    }
    thumbs_kick();
    return NULL;
}

/* Page 1 of a PDF, painted onto a white page at THUMB_PX. Poppler opens
   the file by URI, the same way Preview does. */
static GdkPixbuf *thumb_from_pdf(const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (!uri) {
        g_clear_error(&err);
        return NULL;
    }
    PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &err);
    g_free(uri);
    if (!doc) {
        g_clear_error(&err);
        return NULL;
    }
    PopplerPage *page = poppler_document_get_page(doc, 0);
    if (!page) {
        g_object_unref(doc);
        return NULL;
    }
    double pw = 0, ph = 0;
    poppler_page_get_size(page, &pw, &ph);
    if (pw < 1 || ph < 1) {
        g_object_unref(page);
        g_object_unref(doc);
        return NULL;
    }
    double s = fmin((double)THUMB_PX / pw, (double)THUMB_PX / ph);
    int w = (int)ceil(pw * s), h = (int)ceil(ph * s);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, s, s);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);
    cairo_surface_destroy(surf);
    g_object_unref(page);
    g_object_unref(doc);
    return pb;
}

/* One picture (or a PDF's first page) → sidecar PNG, then the memory cache.
   Write is best-effort: a read-only folder still shows the thumb this session. */
static int thumb_make(const char *path, time_t mtime)
{
    struct stat st;
    if (stat(path, &st))
        return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    long long cap = name_is_pdf(base) ? PDF_THUMB_MAX : PREVIEW_MAX;
    if (st.st_size > cap)
        return 0;
    GdkPixbuf *pb = name_is_pdf(base) ? thumb_from_pdf(path)
                                     : pixbuf_at_scale(path, THUMB_PX, THUMB_PX);
    if (!pb) {
        tcache_put(path, mtime, NULL, 1);
        return 0;
    }
    char side[PATH_MAX], tmp[PATH_MAX];
    sidecar_of(path, side, sizeof side);
    snprintf(tmp, sizeof tmp, "%s.tmp", side);
    if (gdk_pixbuf_save(pb, tmp, "png", NULL, NULL)) {
        if (rename(tmp, side))
            unlink(tmp);
    } else {
        unlink(tmp);
    }
    tcache_put(path, mtime, pb, 0);
    g_object_unref(pb);
    return 1;
}

static void note_dir_now(void)
{
    struct stat st;
    if (strcmp(cwd, DISKS) && !stat(cwd, &st))
        dir_mtime = st.st_mtime;
}

static int thumb_one(const struct item *v, int n, const char *dir)
{
    if (!dir || !dir[0] || !strcmp(dir, DISKS))
        return 0;
    for (int i = 0; i < n; i++) {
        if (!name_is_pic(v[i].name))
            continue;
        char path[PATH_MAX];
        join(path, sizeof path, dir, v[i].name);
        int failed = 0;
        if (tcache_get(path, v[i].mtime, &failed) || failed)
            continue;
        GdkPixbuf *pb = thumb_load_side(path, v[i].mtime);
        if (pb) {
            tcache_put(path, v[i].mtime, pb, 0);
            g_object_unref(pb);
            return 1;
        }
        if (thumb_make(path, v[i].mtime)) {
            note_dir_now();
            return 1;
        }
    }
    return 0;
}

static gboolean thumb_idle(gpointer d)
{
    (void)d;
    if (ren.on) {
        thumb_src = 0;
        return G_SOURCE_REMOVE;
    }
    int did = thumb_one(items, nitems, cwd);
    if (!did && vmode == V_COLUMNS)
        did = thumb_one(peek.items, peek.n, peek.path);
    if (!did && vmode == V_COLUMNS)
        for (int k = 0; !did && k < nanc; k++)
            did = thumb_one(anc[k].items, anc[k].n, anc[k].path);
    if (did) {
        redraw();
        return G_SOURCE_CONTINUE;
    }
    thumb_src = 0;
    return G_SOURCE_REMOVE;
}

static void thumbs_kick(void)
{
    if (!thumb_src)
        thumb_src = g_idle_add(thumb_idle, NULL);
}

/* A picture icon: the thumbnail on a white card (big icons), or just the
   picture (the 16 px list). The type icon is the stand-in until idle
   finishes the sidecar. */
static void paint_thumb(cairo_t *cr, GdkPixbuf *pb, double x, double y, double size, int card)
{
    double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
    if (iw < 1 || ih < 1)
        return;
    /* The photo fills the icon; a 2 px white rim is enough to look like a
       picture, not a blank document. */
    double pad = card ? 2 : 0.5;
    double box = size - 2 * pad;
    double f = fmin(box / iw, box / ih);
    double dw = iw * f, dh = ih * f;
    double dx = x + (size - dw) / 2, dy = y + (size - dh) / 2;
    if (card) {
        rrect(cr, dx - pad + 1, dy - pad + 2, dw + 2 * pad, dh + 2 * pad, 3);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
        cairo_fill(cr);
        rrect(cr, dx - pad, dy - pad, dw + 2 * pad, dh + 2 * pad, 3);
        rgb(cr, 0xffffff, 1);
        cairo_fill_preserve(cr);
        rgb(cr, BORDER, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    double ddw = dw, ddh = dh;
    int gs = gtk_widget_get_scale_factor(da);
    cairo_user_to_device(cr, &dx, &dy);
    cairo_user_to_device_distance(cr, &ddw, &ddh);
    cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(pb, gs, gtk_widget_get_window(da));
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, ddw / iw, ddh / ih);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(s);
}

static void file_pic(cairo_t *cr, const struct item *it, const char *dir,
                     double x, double y, double size, double alpha)
{
    if (name_is_pic(it->name) && dir && strcmp(dir, DISKS)) {
        char path[PATH_MAX];
        join(path, sizeof path, dir, it->name);
        GdkPixbuf *pb = thumb_pixbuf(path, it->mtime);
        if (pb) {
            cairo_save(cr);
            if (alpha < 1)
                cairo_push_group(cr);
            paint_thumb(cr, pb, x, y, size, size >= 28);
            if (alpha < 1) {
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, alpha);
            }
            cairo_restore(cr);
            return;
        }
    }
    icon(cr, it->icon, x, y, size, alpha);
}

/* --- the desktop's scale ---------------------------------------------------- */
static long root_cardinal(Atom a)
{
    Display *d = gdk_x11_get_default_xdisplay();
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    long v = 0;
    if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type, &fmt,
                           &n, &after, &p) == Success && p && n == 1 && fmt == 32)
        v = *(long *)p;
    if (p)
        XFree(p);
    return v;
}

/* _MYOS_SCALE, or 0 for "automatic", which is _MYOS_SCALE_AUTO; without
   myos-xsettings, what automatic would pick. */
static int read_scale(void)
{
    long s = root_cardinal(a_scale);
    if (!s)
        s = root_cardinal(a_scale_auto);
    if (!s)
        s = auto_scale(gdk_screen_get_width(gdk_screen_get_default()));
    return clamp_scale((int)s);
}

/* GTK doubles everything itself at 200% (Gdk/WindowScalingFactor), so
   only the rest of the scale is ours. */
static void update_k(void)
{
    K = desk_scale / 100.0 / gtk_widget_get_scale_factor(da);
}

static GdkFilterReturn root_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    (void)data;
    XEvent *x = xev;
    if (x->type == PropertyNotify && (x->xproperty.atom == a_scale || x->xproperty.atom == a_scale_auto)) {
        int s = read_scale();
        if (s != desk_scale) {
            int w, h;
            gtk_window_get_size(GTK_WINDOW(win), &w, &h);
            double old = K;
            desk_scale = s;
            update_k();
            icon_flush();
            gtk_window_resize(GTK_WINDOW(win), (int)(w * K / old), (int)(h * K / old));
            redraw();
        }
    }
    return GDK_FILTER_CONTINUE;
}

/* --- files ------------------------------------------------------------------ */
static int has_ext(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
    for (; *list; list++)
        if (!strcasecmp(dot + 1, *list))
            return 1;
    return 0;
}

static const char *const TEXT_EXT[] = { "txt", "md", "c", "h", "cc", "cpp", "hpp", "py", "sh", "s",
    "asm", "conf", "cfg", "ini", "log", "csv", "json", "xml", "css", "js", "mk",
    "ld", "patch", "diff", "rc", "yml", "yaml", "tsv", "lua", "pl", "rs", "go", "java", NULL };
/* What Preview opens: PDFs and pictures. */
static const char *const PDF_EXT[] = { "pdf", "png", "jpg", "jpeg", "jpe", "gif", "bmp", "tif", "tiff",
    "heic", "heif", "avif", "jxl", "webp", "ico", "icns", "tga", "jp2", "j2k", "jpf", "exr", "psd",
    "svg", "pbm", "pgm", "ppm", "pnm", "xbm", "xpm", "cr2", "nef", "arw", "dng", "orf", "rw2", "raf", NULL };
static const char *const WP_EXT[] = { "wpd", "wp", "wp5", "wp6", NULL };
static const char *const DOC_EXT[] = { "doc", "docx", "rtf", "abw", NULL };
static const char *const LO_EXT[] = { "odt", "ods", "odp", "odg", "odf", "xls", "xlsx",
    "xlsm", "ppt", "pptx", "pps", "ppsx", NULL };
static const char *const WEB_EXT[] = { "html", "htm", "xhtml", NULL };
static const char *const MEDIA_EXT[] = { "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm",
    "mpg", "mpeg", "m4v", "ts", "m2ts", "mp3", "flac", "ogg", "oga", "wav", "aac", "m4a",
    "wma", "opus", NULL };
static const char *const IMG_EXT[] = { "png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "tif",
    "tiff", "heic", "heif", "avif", "ico", "xpm", "tga", "pnm", "ppm", "pgm", "pbm", NULL };

static const char *file_icon(const char *name, const char *path, const struct stat *st)
{
    if (S_ISDIR(st->st_mode))
        return "folder";
    if (has_ext(name, PDF_EXT))
        return "preview";
    if (has_ext(name, WP_EXT))
        return "writeperfect";
    if (has_ext(name, DOC_EXT))
        return "text-editor";
    if (has_ext(name, LO_EXT))
        return "text-editor";
    if (has_ext(name, WEB_EXT))
        return "web-browser";
    if (has_ext(name, MEDIA_EXT))
        return "sound";
    if (!S_ISREG(st->st_mode))
        return "binary-file";
    if (has_ext(name, TEXT_EXT))
        return "text-file";
    (void)path;
    return "binary-file";
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

/* ~/Trash: the same can `rm` uses. Sidecars (._name.where) stay hidden. */
static int in_trash(void)
{
    const char *home = getenv("HOME");
    char t[PATH_MAX];
    if (!home || !home[0] || !strcmp(cwd, DISKS))
        return 0;
    snprintf(t, sizeof t, "%s/Trash", home);
    return !strcmp(cwd, t);
}

/* Folders first, then by name, ignoring case — the mockup's order. */
static int item_cmp(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static void free_list(struct item **v, int *n)
{
    for (int i = 0; i < *n; i++)
        g_free((*v)[i].name);
    g_free(*v);
    *v = NULL;
    *n = 0;
}

static void push_item(struct item **v, int *n, const char *name, const char *icon, int dir,
                      long long size, time_t mtime, int exec)
{
    if ((*n & (*n - 1)) == 0)               /* 0, 1, 2, 4, 8...: double the room */
        *v = g_renew(struct item, *v, *n ? *n * 2 : 1);
    (*v)[*n] = (struct item){
        .name = g_strdup(name), .icon = icon, .dir = dir, .size = size,
        .mtime = mtime, .exec = exec, .pic = !dir && has_ext(name, IMG_EXT)
    };
    (*n)++;
}

static void add_item(const char *name, const char *icon, int dir, long long size, time_t mtime)
{
    push_item(&items, &nitems, name, icon, dir, size, mtime, 0);
}

/* A folder's files, sorted, into *v (which starts out NULL); the count. */
static int list_dir(const char *dir, struct item **v)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (is_sidecar(e->d_name))          /* Mac's hidden sidecar, never listed */
            continue;
        if (e->d_name[0] == '.' && !show_hidden)
            continue;
        char path[PATH_MAX];
        struct stat st;
        join(path, sizeof path, dir, e->d_name);
        if (stat(path, &st) && lstat(path, &st))
            continue;
        int reg = S_ISREG(st.st_mode);
        push_item(v, &n, e->d_name, file_icon(e->d_name, path, &st), S_ISDIR(st.st_mode),
                  reg ? (long long)st.st_size : 0, st.st_mtime, reg && (st.st_mode & 0111));
    }
    closedir(d);
    qsort(*v, n, sizeof **v, item_cmp);
    return n;
}

/* An item of the folder shown, as a path. */
static void item_path(int i, char *out, size_t size)
{
    if (!strcmp(cwd, DISKS))
        snprintf(out, size, "%s", items[i].name);
    else
        join(out, size, cwd, items[i].name);
}

/* "12 KB", the way the status bar and the List count. */
static void fmt_size(long long b, char *out, size_t size)
{
    if (b < 1024)
        snprintf(out, size, "%lld bytes", b);
    else if (b < 1024 * 1024 * 10LL)
        snprintf(out, size, "%lld KB", (b + 1023) / 1024);
    else if (b < 1024LL * 1024 * 1024)
        snprintf(out, size, "%.1f MB", b / 1048576.0);
    else
        snprintf(out, size, "%.1f GB", b / 1073741824.0);
}

/* "Today, 21:05", "Yesterday, 09:30" or "4 Oct 2026, 21:05". */
static void fmt_date(time_t t, char *out, size_t size)
{
    struct tm a, b;
    time_t now = time(NULL);
    out[0] = 0;
    if (!t || !localtime_r(&t, &a) || !localtime_r(&now, &b))
        return;
    char hm[8];
    strftime(hm, sizeof hm, "%H:%M", &a);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, size, "Today, %s", hm);
    else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1)
        snprintf(out, size, "Yesterday, %s", hm);
    else
        strftime(out, size, "%e %b %Y, %H:%M", &a);
    if (out[0] == ' ')
        memmove(out, out + 1, strlen(out));
}

/* What the List's Kind column says: "Folder", "PDF document", "PNG image"... */
static void fmt_kind(const struct item *it, char *out, size_t size)
{
    char ext[16] = "";
    const char *dot = strrchr(it->name, '.');
    if (dot && dot != it->name && strlen(dot + 1) < sizeof ext)
        for (int i = 0; dot[i + 1]; i++)
            ext[i] = (char)g_ascii_toupper(dot[i + 1]), ext[i + 1] = 0;
    if (!strcmp(it->icon, "disks"))
        snprintf(out, size, "Disk");
    else if (it->dir)
        snprintf(out, size, "Folder");
    else if (!strcmp(it->icon, "evince"))
        snprintf(out, size, "PDF document");
    else if (!strcmp(it->icon, "writeperfect"))
        snprintf(out, size, "WordPerfect document");
    else if (has_ext(it->name, IMG_EXT))
        snprintf(out, size, "%s image", ext);
    else if (!strcmp(it->icon, "text-editor"))
        snprintf(out, size, "%s document", ext);
    else if (!strcmp(it->icon, "web-browser"))
        snprintf(out, size, "Web page");
    else if (!strcmp(it->icon, "text-file"))
        snprintf(out, size, !ext[0] || !strcmp(ext, "TXT") ? "Plain text" : "%s text", ext);
    else if (it->exec)
        snprintf(out, size, "Program");
    else
        snprintf(out, size, ext[0] ? "%s file" : "Binary file", ext);
}

/* /proc/mounts escapes a space in a path as \040 (and \t, \n, \\ the
   same way): back to the characters, in place. */
static void unescape_mount(char *s)
{
    char *o = s;
    for (; *s; s++, o++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *o = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else {
            *o = *s;
        }
    }
    *o = 0;
}

/* Disks: "/" and every other disk's file system (ext2/ext4 or FAT) that is
   mounted, from /proc/mounts — /data, a USB stick at /mnt, wherever it is.
   Without /proc (an old MyOS), the folders in "/" that are the top of
   another disk, as before. */
static void load_disks(void)
{
    struct stat root;
    stat("/", &root);
    add_item("/", "disks", 1, 0, 0);
    FILE *m = fopen("/proc/mounts", "re");
    if (m) {
        char line[1024];
        while (fgets(line, sizeof line, m)) {
            char src[PATH_MAX], dir[PATH_MAX], type[32];
            if (sscanf(line, "%4095s %4095s %31s", src, dir, type) != 3)
                continue;
            unescape_mount(dir);
            if (strcmp(dir, "/") && (!strcmp(type, "ext2") || !strcmp(type, "ext4") ||
                                     !strcmp(type, "vfat")))
                add_item(dir, "disks", 1, 0, 0);
        }
        fclose(m);
        return;
    }
    DIR *d = opendir("/");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[PATH_MAX];
        struct stat st;
        struct statfs sf;
        if (e->d_name[0] == '.')
            continue;
        join(path, sizeof path, "/", e->d_name);
        if (stat(path, &st) || !S_ISDIR(st.st_mode) || st.st_dev == root.st_dev || statfs(path, &sf))
            continue;
        if (sf.f_type == 0xEF53 || sf.f_type == 0x4d44)
            add_item(path, "disks", 1, 0, 0);
    }
    closedir(d);
}

/* Read the folder again; keep the selection on `keep` if it's still there. */
static void reload(const char *keep)
{
    char *k = keep ? g_strdup(keep) : NULL;
    free_list(&items, &nitems);
    col_free_all();
    sel = -1;
    if (!strcmp(cwd, DISKS)) {
        load_disks();
    } else {
        nitems = list_dir(cwd, &items);
        struct stat st;
        dir_mtime = stat(cwd, &st) ? 0 : st.st_mtime;
    }
    watch_cwd();
    if (k) {
        for (int i = 0; i < nitems; i++)
            if (!strcmp(items[i].name, k))
                sel = i;
        g_free(k);
    }
    if (vmode == V_GALLERY && sel < 0 && nitems)    /* the Gallery always shows one */
        sel = 0;
    thumbs_kick();
    redraw();
}

static void set_title(void)
{
    char t[PATH_MAX + 16];
    snprintf(t, sizeof t, "My Files - %s", strcmp(cwd, DISKS) ? cwd : "Disks");
    gtk_window_set_title(GTK_WINDOW(win), t);
}

static void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static gboolean status_clear(gpointer d)
{
    (void)d;
    status_msg[0] = 0;
    status_timer = 0;
    redraw();
    return G_SOURCE_REMOVE;
}

/* A note in the status bar for a few seconds ("Copied hello.txt"). */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof status_msg, fmt, ap);
    va_end(ap);
    if (status_timer)
        g_source_remove(status_timer);
    status_timer = g_timeout_add_seconds(5, status_clear, NULL);
    redraw();
}

static void navigate(const char *path, int remember)
{
    char real[PATH_MAX];
    const char *came_from = NULL;
    char old[PATH_MAX];
    snprintf(old, sizeof old, "%s", cwd);
    if (strcmp(path, DISKS)) {
        struct stat st;
        if (!realpath(path, real) || stat(real, &st)) {
            say("Can't open %s: %s", path, strerror(errno));
            return;
        }
        if (!S_ISDIR(st.st_mode)) {
            say("%s isn't a folder", path);
            return;
        }
        if (access(real, R_OK | X_OK)) {
            say("You aren't allowed to look in %s", real);
            return;
        }
        path = real;
    }
    if (remember && strcmp(old, path)) {
        g_ptr_array_add(back_hist, g_strdup(old));
        g_ptr_array_set_size(fwd_hist, 0);
    }
    /* Up from /data/dir: dir stays selected, so you see where you were. */
    size_t n = strlen(path);
    if (!strncmp(old, path, n) && (old[n] == '/' || (n == 1 && old[1])) && !strchr(old + n + 1, '/'))
        came_from = old + n + (old[n] == '/');
    snprintf(cwd, sizeof cwd, "%s", path);
    scroll = 0;
    addr.on = ren.on = 0;
    set_title();
    reload(came_from);
}

/* --- layout ------------------------------------------------------------------ */
static double files_right(void) { return has_scroll ? W - SCROLL_W : W; }
static double files_bottom(void) { return H - STATUS_H; }
/* Where the scrolling part starts: below the List's column titles. */
static double view_top(void) { return BODY_Y + (vmode == V_LIST ? LHEAD_H : 0); }

/* --- Columns ----------------------------------------------------------------- */
static void col_set(struct col *c, const char *path)
{
    if (!strcmp(c->path, path))
        return;
    free_list(&c->items, &c->n);
    snprintf(c->path, sizeof c->path, "%s", path);
    c->scroll = 0;
    c->sel = -1;
    if (path[0])
        c->n = list_dir(path, &c->items);
}

static void col_free_all(void)
{
    for (int k = 0; k < MAXCOL; k++)
        col_set(&anc[k], "");
    col_set(&peek, "");
    nanc = 0;
}

static double col_max_scroll(int n)
{
    double m = 8 + n * CROW_H - (files_bottom() - BODY_Y);
    return m > 0 ? m : 0;
}

static void col_scroll_by(struct col *c, double d)
{
    c->scroll += d;
    if (c->scroll > col_max_scroll(c->n))
        c->scroll = col_max_scroll(c->n);
    if (c->scroll < 0)
        c->scroll = 0;
}

/* p is p itself or a folder inside it. */
static int under(const char *root, const char *p)
{
    size_t n = strlen(root);
    if (!strcmp(root, "/"))
        return p[0] == '/';
    return !strncmp(p, root, n) && (p[n] == 0 || p[n] == '/');
}

/* Bring the columns in line with the folder shown: one column per folder
   from col_root down to it, each with the next one on the way selected,
   and the selected folder's files after it. A column is read only when
   its folder changes. */
static void col_sync(void)
{
    int k = 0;
    if (strcmp(cwd, DISKS)) {
        if (!col_root[0] || !strcmp(col_root, DISKS) || !under(col_root, cwd))
            snprintf(col_root, sizeof col_root, "%s", cwd);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s", col_root);
        while (strcmp(p, cwd) && k < MAXCOL) {
            const char *rest = cwd + strlen(p) + (strcmp(p, "/") ? 1 : 0);
            char child[NAME_MAX + 1];
            snprintf(child, sizeof child, "%.*s", (int)strcspn(rest, "/"), rest);
            col_set(&anc[k], p);
            int old = anc[k].sel;
            anc[k].sel = -1;
            for (int i = 0; i < anc[k].n; i++)
                if (!strcmp(anc[k].items[i].name, child))
                    anc[k].sel = i;
            if (anc[k].sel != old && anc[k].sel >= 0) {    /* scroll it into the middle */
                double y = 4 + anc[k].sel * CROW_H, h = files_bottom() - BODY_Y;
                anc[k].scroll = 0;
                col_scroll_by(&anc[k], y + CROW_H / 2 - h / 2);
            }
            size_t pl = strlen(p);
            snprintf(p + pl, sizeof p - pl, "%s%s", strcmp(p, "/") ? "/" : "", child);
            k++;
        }
        if (k == MAXCOL) {                  /* too deep: start the columns here */
            snprintf(col_root, sizeof col_root, "%s", cwd);
            k = 0;
        }
    } else {
        snprintf(col_root, sizeof col_root, "%s", DISKS);
    }
    for (int j = k; j < MAXCOL; j++)
        col_set(&anc[j], "");
    nanc = k;
    char want[PATH_MAX] = "";
    if (sel >= 0 && items[sel].dir)
        item_path(sel, want, sizeof want);
    col_set(&peek, want);
}

/* The columns: the way down, the folder shown, then what's selected in it
   (a folder's files, or a file's preview). Too many to fit: the left ones
   slide out of sight, so the folder shown stays in view. */
static int col_count(void) { return nanc + 1 + (sel >= 0); }

static double col_x(int k)
{
    double over = col_count() * COL_W - (W - PANE_W);
    return PANE_W + k * COL_W - (over > 0 ? over : 0);
}

static int col_row(double y, double scr, int n)
{
    if (y < BODY_Y)
        return -1;
    int i = (int)floor((y - BODY_Y - 4 + scr) / CROW_H);
    return i >= 0 && i < n ? i : -1;
}

/* --- Gallery ------------------------------------------------------------------ */
/* The strip's first cell: centred when all fit, else slid to keep the
   selected one in the middle. */
static double gallery_x0(void)
{
    double left = PANE_W + 8, right = W - 8, total = nitems * 64.0;
    if (total <= right - left)
        return (left + right) / 2 - total / 2;
    double x0 = (left + right) / 2 - (sel < 0 ? 0 : sel) * 64.0 - 32;
    if (x0 > left)
        x0 = left;
    if (x0 + total < right)
        x0 = right - total;
    return x0;
}

static void relayout(void)
{
    double view = files_bottom() - BODY_Y, content = 0;
    cols = 1;
    has_scroll = 0;
    switch (vmode) {
    case V_ICONS: {
        cols = (int)((W - GRID_X - 104 + CELL_W) / CELL_W);
        if (cols < 1)
            cols = 1;
        int rows = (nitems + cols - 1) / cols;
        content = 16 + rows * CELL_H;
        has_scroll = content > view;
        if (has_scroll) {
            cols = (int)((W - SCROLL_W - GRID_X - 104 + CELL_W) / CELL_W);
            if (cols < 1)
                cols = 1;
            rows = (nitems + cols - 1) / cols;
            content = 16 + rows * CELL_H;
        }
        break;
    }
    case V_LIST:
        view -= LHEAD_H;
        content = 4 + nitems * LROW_H;
        has_scroll = content > view;
        break;
    case V_COLUMNS:
        col_sync();
        content = 8 + nitems * CROW_H;
        break;
    }
    max_scroll = content > view ? content - view : 0;
    if (scroll > max_scroll)
        scroll = max_scroll;
    if (scroll < 0)
        scroll = 0;

    /* The task pane's two cards: what you can do here, and other places. */
    int disks = !strcmp(cwd, DISKS);
    ntasks = ngroups = 0;
    double gy = BODY_Y + 10;
    if (!disks) {
        groups[ngroups].title = "File tasks";
        groups[ngroups].first = ntasks;
        tasks[ntasks++] = (struct task){ "folder", "Make a new folder", A_NEWFOLDER };
        /* Always there, greyed out until something is selected, so the
           cards keep the mockup's size and place. */
        const char *what = sel >= 0 && items[sel].dir ? "folder" : "file";
        tasks[ntasks] = (struct task){ "text-file", "", A_COPY };
        snprintf(tasks[ntasks++].label, sizeof tasks[0].label, "Copy this %s", what);
        if (in_trash()) {
            tasks[ntasks] = (struct task){ "folder", "", A_PUTBACK };
            snprintf(tasks[ntasks++].label, sizeof tasks[0].label, "Put back this %s", what);
            tasks[ntasks] = (struct task){ "recycle", "", A_DELETE };
            snprintf(tasks[ntasks++].label, sizeof tasks[0].label, "Delete this %s for good", what);
            tasks[ntasks] = (struct task){ "recycle", "Empty Trash", A_EMPTY };
            ntasks++;
        } else {
            tasks[ntasks] = (struct task){ "recycle", "", A_DELETE };
            snprintf(tasks[ntasks++].label, sizeof tasks[0].label, "Delete this %s", what);
        }
        groups[ngroups].n = ntasks - groups[ngroups].first;
        groups[ngroups].y = gy;
        groups[ngroups].h = 33 + groups[ngroups].n * 25;
        gy += groups[ngroups].h + 10;
        ngroups++;
    }
    groups[ngroups].title = "Other places";
    groups[ngroups].first = ntasks;
    if (disks)
        tasks[ntasks++] = (struct task){ "my-files", "My Files", A_HOME };
    else
        tasks[ntasks++] = (struct task){ "disks", "Disks", A_DISKS };
    tasks[ntasks++] = (struct task){ "network", "Network", A_NETWORK };
    tasks[ntasks++] = (struct task){ "terminal", "Terminal", A_TERMINAL };
    groups[ngroups].n = ntasks - groups[ngroups].first;
    groups[ngroups].y = gy;
    groups[ngroups].h = 33 + groups[ngroups].n * 25;
    ngroups++;
}

/* Skip the layout work when the window is only being redrawn (the mouse
   moved): col_sync would otherwise re-walk the path on every paint. */
static void relayout_maybe(void)
{
    static char last_cwd[PATH_MAX];
    static double last_W, last_H, last_scroll;
    static int last_n, last_sel, last_v, last_hid;
    if (!strcmp(last_cwd, cwd) && last_W == W && last_H == H && last_n == nitems &&
        last_sel == sel && last_v == vmode && last_scroll == scroll && last_hid == show_hidden)
        return;
    snprintf(last_cwd, sizeof last_cwd, "%s", cwd);
    last_W = W;
    last_H = H;
    last_n = nitems;
    last_sel = sel;
    last_v = vmode;
    last_scroll = scroll;
    last_hid = show_hidden;
    relayout();
}

static void cell(int i, double *x, double *y)
{
    *x = GRID_X + (i % cols) * CELL_W;
    *y = GRID_Y + (i / cols) * CELL_H - scroll;
}

static const char *MENUS[] = { "File", "Edit", "View", "Tools", "Help" };
static const char *TOOLS[] = { "Back", "Forward", "Up" };
static const char *TOOL_ICONS[] = { "back", "forward", "up" };

static double menu_x(int i) { return 11 + i * 50; }
static double tool_x(int i) { return 9 + i * 86; }

static int tool_enabled(int i)
{
    if (i == 0)
        return back_hist->len > 0;
    if (i == 1)
        return fwd_hist->len > 0;
    return strcmp(cwd, "/") && strcmp(cwd, DISKS);
}

static void scroll_thumb(double *y, double *h)
{
    double top = view_top() + 2, len = files_bottom() - view_top() - 4;
    double view = files_bottom() - view_top();
    *h = len * view / (view + max_scroll);
    if (*h < 24)
        *h = 24;
    *y = top + (len - *h) * (max_scroll > 0 ? scroll / max_scroll : 0);
}

static struct target hit(double x, double y)
{
    struct target t = { T_NONE, 0 };
    if (y < MENU_H) {
        for (int i = 0; i < 5; i++)
            if (x >= menu_x(i) - 6 && x < menu_x(i) + text_width(MENUS[i], 12, 0, 0) + 6)
                return (struct target){ T_MENU, i };
        return t;
    }
    if (y < BODY_Y) {
        for (int i = 0; i < 3; i++)
            if (x >= tool_x(i) - 4 && x < tool_x(i) + 29 + text_width(TOOLS[i], 12, 0, 0) + 5 &&
                y >= TOOL_Y + 3 && y < BODY_Y - 3)
                return (struct target){ T_TOOL, i };
        if (x >= VIEW_X && x < VIEW_X + VIEW_W && y >= TOOL_Y + 3 && y < BODY_Y - 3)
            return (struct target){ T_VIEW, 0 };
        if (x >= ADDR_X && x < W - 11 && y >= 28 && y < 50)
            return (struct target){ T_ADDR, 0 };
        return t;
    }
    if (y >= files_bottom())
        return t;
    if (x < PANE_W) {
        for (int g = 0; g < ngroups; g++)
            for (int k = 0; k < groups[g].n; k++) {
                double iy = groups[g].y + 28 + k * 25;
                int ti = groups[g].first + k;
                if (enabled(tasks[ti].act) && x >= 14 &&
                    x < 40 + text_width(tasks[ti].label, 11, 0, 0) + 4 && y >= iy && y < iy + 22)
                    return (struct target){ T_TASK, ti };
            }
        return t;
    }
    if (has_scroll && x >= W - SCROLL_W) {
        double ty, th;
        scroll_thumb(&ty, &th);
        return (struct target){ y >= ty && y < ty + th ? T_THUMB : T_TRACK, 0 };
    }
    if (vmode == V_LIST) {
        if (y < BODY_Y + LHEAD_H)
            return t;
        double r = y - BODY_Y - LHEAD_H - 2 + scroll;
        int i = (int)floor(r / LROW_H);
        if (r >= 0 && i < nitems)
            return (struct target){ T_FILE, i };
        return (struct target){ T_FILES, 0 };
    }
    if (vmode == V_COLUMNS) {
        for (int k = 0; k < col_count(); k++) {
            double cx = col_x(k);
            if (x < cx || x >= cx + COL_W)
                continue;
            if (k == nanc) {
                int i = col_row(y, scroll, nitems);
                return (struct target){ i >= 0 ? T_FILE : T_FILES, i >= 0 ? i : 0 };
            }
            if (k < nanc) {
                int i = col_row(y, anc[k].scroll, anc[k].n);
                return i >= 0 ? (struct target){ T_COL, k * COL_IDX + i } : t;
            }
            if (items[sel].dir) {
                int i = col_row(y, peek.scroll, peek.n);
                return i >= 0 ? (struct target){ T_COL, MAXCOL * COL_IDX + i } : t;
            }
            return (struct target){ T_FILE, sel };      /* the file's preview */
        }
        return t;
    }
    if (vmode == V_GALLERY) {
        double sy = files_bottom() - STRIP_H, x0 = gallery_x0();
        if (y >= sy + 8 && y < sy + 64) {
            int i = (int)floor((x - x0) / 64);
            if (x >= x0 && i < nitems)
                return (struct target){ T_FILE, i };
            return t;
        }
        if (y < sy && sel >= 0)
            return (struct target){ T_FILE, sel };
        return t;
    }
    for (int i = 0; i < nitems; i++) {
        double cx, cy;
        cell(i, &cx, &cy);
        if (x >= cx + 14 && x < cx + 90 && y >= cy - 6 && y < cy + 72)
            return (struct target){ T_FILE, i };
    }
    return (struct target){ T_FILES, 0 };
}

/* --- drawing -------------------------------------------------------------- */
static void edit_draw(cairo_t *cr, struct edit *e, double x, double base, double size, double maxw)
{
    char save = e->s[e->caret];
    if (e->all && e->s[0]) {
        double w = text_width(e->s, size, 0, 0);
        rgb(cr, TITLE_MID, 1);
        cairo_rectangle(cr, x - 1, base - size, w + 2, size * 1.3);
        cairo_fill(cr);
        text(cr, x, base, e->s, size, 0xffffff, 0, 0, maxw);
    } else {
        text(cr, x, base, e->s, size, TEXT, 0, 0, maxw);
    }
    e->s[e->caret] = 0;
    double cx = x + text_width(e->s, size, 0, 0);
    e->s[e->caret] = save;
    rgb(cr, TEXT, 1);
    cairo_rectangle(cr, cx, base - size + 1, 1, size * 1.2);
    cairo_fill(cr);
}

/* A name being renamed: a focused Text box at (x, y), 18 high. */
static void ren_draw(cairo_t *cr, double x, double y, double minw)
{
    double tw = text_width(ren.s, 12, 0, 0);
    double bw = tw + 12 < minw ? minw : tw + 12;
    rgb(cr, TEAL_BOTTOM, 0.4);
    cairo_set_line_width(cr, 2);
    rrect(cr, x - 2, y - 2, bw + 4, 22, 5);
    cairo_stroke(cr);
    bar(cr, x, y, bw, 18, 3, NULL, 0xffffff, TEAL_RIM, 1.5);
    edit_draw(cr, &ren, x + 6, y + 14, 12, 0);
}

static double text_right(cairo_t *cr, double right, double y, const char *s, double size, unsigned c)
{
    return text(cr, right - text_width(s, size, 0, 0), y, s, size, c, 0, 0, 0);
}

static void empty_note(cairo_t *cr, double y)
{
    if (!nitems && strcmp(cwd, DISKS))
        text(cr, (PANE_W + files_right()) / 2, y, "This folder is empty.", 12, TEXT_SOFT, 0, 1, 0);
}

/* The View button's picture of each layout, in a 16x16 box, drawn rather
   than taken from the icon set (it has none): four tiles, three lines,
   three columns, a picture over a strip. */
static void view_glyph(cairo_t *cr, int v, double x, double y, unsigned c)
{
    rgb(cr, c, 1);
    cairo_set_line_width(cr, 1.5);
    switch (v) {
    case V_ICONS:
        for (int k = 0; k < 4; k++)
            rrect(cr, x + 1 + (k & 1) * 8, y + 1 + (k >> 1) * 8, 6, 6, 1.5);
        cairo_fill(cr);
        break;
    case V_LIST:
        for (int k = 0; k < 3; k++) {
            cairo_arc(cr, x + 2, y + 3 + k * 5, 1.4, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_move_to(cr, x + 6, y + 3 + k * 5);
            cairo_line_to(cr, x + 15, y + 3 + k * 5);
            cairo_stroke(cr);
        }
        break;
    case V_COLUMNS:
        rrect(cr, x + 0.75, y + 1.75, 14.5, 12.5, 2);
        cairo_move_to(cr, x + 5.75, y + 2);
        cairo_line_to(cr, x + 5.75, y + 14);
        cairo_move_to(cr, x + 10.25, y + 2);
        cairo_line_to(cr, x + 10.25, y + 14);
        cairo_stroke(cr);
        break;
    case V_GALLERY:
        rrect(cr, x + 0.75, y + 0.75, 14.5, 9.5, 2);
        cairo_stroke(cr);
        for (int k = 0; k < 3; k++)
            rrect(cr, x + 1 + k * 5.25, y + 12, 3.75, 3.5, 1);
        cairo_fill(cr);
        break;
    }
}

/* The Mac's "there's a menu here" mark: a small up and down chevron. */
static void chevrons(cairo_t *cr, double x, double y, unsigned c)
{
    rgb(cr, c, 1);
    cairo_set_line_width(cr, 1.3);
    cairo_move_to(cr, x - 3, y - 2);
    cairo_line_to(cr, x, y - 5);
    cairo_line_to(cr, x + 3, y - 2);
    cairo_move_to(cr, x - 3, y + 2);
    cairo_line_to(cr, x, y + 5);
    cairo_line_to(cr, x + 3, y + 2);
    cairo_stroke(cr);
}

/* The > after a folder's name in Columns. */
static void chevron_right(cairo_t *cr, double x, double y, unsigned c)
{
    rgb(cr, c, 1);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, x - 2, y - 4);
    cairo_line_to(cr, x + 2, y);
    cairo_line_to(cr, x - 2, y + 4);
    cairo_stroke(cr);
}

/* --- previews: Gallery's big picture, and a file's column in Columns --------- */
/* Fit a surface into the box, with a soft page shadow. iw/ih are the
   surface's pixel size — cairo_image_surface_get_width is 0 on a pixbuf
   surface, so the caller has to pass them. */
static int preview_fit(cairo_t *cr, cairo_surface_t *s, double iw, double ih,
                       double bx, double by, double bw, double bh)
{
    if (!s || iw < 1 || ih < 1)
        return 0;
    int gs = gtk_widget_get_scale_factor(da);
    double pw = bw * K * gs, ph = bh * K * gs;
    if (pw < 8 || ph < 8)
        return 0;
    double f = fmin(pw / iw, ph / ih);
    double ux = bx, uy = by;
    cairo_user_to_device(cr, &ux, &uy);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_translate(cr, round(ux * gs + (pw - iw * f) / 2) / gs,
                       round(uy * gs + (ph - ih * f) / 2) / gs);
    cairo_scale(cr, f / gs, f / gs);
    cairo_rectangle(cr, 3, 4, iw, ih);           /* a soft shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_fill(cr);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    return 1;
}

/* A picture file, read once at the size it's shown (gdk-pixbuf's loaders:
   PNG, JPEG, GIF, SVG...) and kept until another file is shown. */
static struct { char path[PATH_MAX]; time_t mtime; int w, h, failed, iw, ih; cairo_surface_t *s; } pimg;

static int preview_image(cairo_t *cr, const char *path, time_t mtime, double bx, double by,
                         double bw, double bh)
{
    int gs = gtk_widget_get_scale_factor(da);
    int pw = (int)(bw * K * gs), ph = (int)(bh * K * gs);
    if (pw < 8 || ph < 8)
        return 0;
    int same = !strcmp(pimg.path, path) && pimg.mtime == mtime;
    /* A bigger box (the window grew) reads it again; a smaller one scales it. */
    if (!same || (!pimg.failed && (pw > pimg.w * 1.1 + 1 || ph > pimg.h * 1.1 + 1))) {
        if (pimg.s)
            cairo_surface_destroy(pimg.s);
        pimg.s = NULL;
        snprintf(pimg.path, sizeof pimg.path, "%s", path);
        pimg.mtime = mtime;
        pimg.w = pw;
        pimg.h = ph;
        pimg.failed = 1;
        struct stat st;
        if (!stat(path, &st) && st.st_size <= PREVIEW_MAX) {
            GdkPixbuf *pb = pixbuf_at_scale(path, pw, ph);
            if (pb) {
                pimg.iw = gdk_pixbuf_get_width(pb);
                pimg.ih = gdk_pixbuf_get_height(pb);
                pimg.s = gdk_cairo_surface_create_from_pixbuf(pb, 1, NULL);
                pimg.failed = 0;
                g_object_unref(pb);
            }
        }
    }
    if (!pimg.s)
        return 0;
    return preview_fit(cr, pimg.s, pimg.iw, pimg.ih, bx, by, bw, bh);
}

/* Sidecar (or any already-small pixbuf) in the same box: used for a PDF's
   first page, which gdk-pixbuf cannot open. */
static int preview_thumb(cairo_t *cr, GdkPixbuf *pb, double bx, double by, double bw, double bh)
{
    if (!pb)
        return 0;
    double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
    cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(pb, 1, NULL);
    int ok = preview_fit(cr, s, iw, ih, bx, by, bw, bh);
    cairo_surface_destroy(s);
    return ok;
}

/* A text file: its first lines on a white page, as Quick Look shows it. */
static struct { char path[PATH_MAX]; time_t mtime; char *text; } ptxt;

static int preview_text(cairo_t *cr, const char *path, time_t mtime, double bx, double by,
                        double bw, double bh)
{
    if (strcmp(ptxt.path, path) || ptxt.mtime != mtime) {
        g_free(ptxt.text);
        ptxt.text = NULL;
        snprintf(ptxt.path, sizeof ptxt.path, "%s", path);
        ptxt.mtime = mtime;
        char buf[3000];
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            close(fd);
            if (n >= 0)
                ptxt.text = g_utf8_make_valid(buf, n);
        }
    }
    if (!ptxt.text)
        return 0;
    double ph = bh, pw = ph * 0.77;
    if (pw > bw) {
        pw = bw;
        ph = pw / 0.77;
    }
    if (pw < 40)
        return 0;
    double px = round(bx + (bw - pw) / 2), py = round(by + (bh - ph) / 2), m = pw * 0.08;
    cairo_rectangle(cr, px + 2, py + 3, pw, ph);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
    cairo_fill(cr);
    bar(cr, px + 0.5, py + 0.5, pw, ph, 0, NULL, 0xffffff, BORDER, 1);
    cairo_save(cr);
    cairo_rectangle(cr, px + m, py + m, pw - 2 * m, ph - 2 * m);
    cairo_clip(cr);
    PangoContext *pc = pango_cairo_create_context(cr);
    font_options(pc);
    PangoLayout *l = pango_layout_new(pc);
    PangoFontDescription *fd = pango_font_description_from_string("DejaVu Sans Mono");
    pango_font_description_set_absolute_size(fd, ph / 44 * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, (int)((pw - 2 * m) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(l, ptxt.text, -1);
    rgb(cr, TEXT, 1);
    cairo_move_to(cr, px + m, py + m);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    g_object_unref(pc);
    cairo_restore(cr);
    return 1;
}

/* Item i, as big as fits the box: the picture, the page, or the icon. */
static void draw_preview(cairo_t *cr, int i, double bx, double by, double bw, double bh)
{
    const struct item *it = &items[i];
    char path[PATH_MAX];
    item_path(i, path, sizeof path);
    if (!it->dir && name_is_pdf(it->name)) {
        GdkPixbuf *pb = thumb_pixbuf(path, it->mtime);
        if (pb && preview_thumb(cr, pb, bx, by, bw, bh))
            return;
    } else if (!it->dir && has_ext(it->name, IMG_EXT) &&
               preview_image(cr, path, it->mtime, bx, by, bw, bh))
        return;
    if (!strcmp(it->icon, "text-file") && preview_text(cr, path, it->mtime, bx, by, bw, bh))
        return;
    double s = floor(fmin(fmin(bw, bh), 160));
    if (s >= 16)
        icon(cr, it->icon, bx + (bw - s) / 2, by + (bh - s) / 2, s, 1);
}

/* "PNG image — 12 KB" under a preview. */
static void fmt_info(int i, char *out, size_t size)
{
    char kind[48], sz[32];
    fmt_kind(&items[i], kind, sizeof kind);
    if (items[i].dir) {
        snprintf(out, size, "%s", kind);
        return;
    }
    fmt_size(items[i].size, sz, sizeof sz);
    snprintf(out, size, "%s \xe2\x80\x94 %s", kind, sz);
}

/* --- the four layouts --------------------------------------------------------- */
static void draw_icons(cairo_t *cr, double bottom)
{
    for (int i = 0; i < nitems; i++) {
        double x, y;
        cell(i, &x, &y);
        if (y > bottom || y + 80 < BODY_Y)
            continue;
        const char *label = items[i].name;
        if (i == sel) {
            if (active)
                bar(cr, x + 22, y - 6, 60, 60, 3, NULL, 0xcdeef0, TEAL_BOTTOM, 1);
            else
                bar(cr, x + 22, y - 6, 60, 60, 3, NULL, 0xe4e6e2, BORDER, 1);
        }
        file_pic(cr, &items[i], cwd, x + 28, y, 48, 1);
        if (ren.on && i == ren_idx) {
            double tw = text_width(ren.s, 12, 0, 0);
            double bw = tw + 12 < 80 ? 80 : tw + 12;
            ren_draw(cr, x + 52 - bw / 2, y + 52, bw);
            continue;
        }
        if (i == sel && active) {
            double tw = text_width(label, 12, 0, 116);
            rrect(cr, x + 52 - (tw + 18) / 2, y + 53, tw + 18, 18, 3);
            rgb(cr, TITLE_MID, 1);
            cairo_fill(cr);
            text(cr, x + 52, y + 66, label, 12, 0xffffff, 0, 1, 116);
        } else {
            text(cr, x + 52, y + 66, label, 12, TEXT, 0, 1, 116);
        }
    }
    empty_note(cr, GRID_Y + 20);
}

/* List: Name | Date Modified | Size | Kind, the Finder's order. Narrow
   windows drop Kind, then the date, then the size. */
struct lcols { double name, name_w, date, size, kind; int d, s, k; };

static void list_cols(struct lcols *c)
{
    double r = files_right(), a = r - PANE_W, x = r;
    c->k = a >= 520;
    c->d = a >= 400;
    c->s = a >= 290;
    if (c->k)
        c->kind = x -= 130;
    if (c->s)
        c->size = x -= 80;
    if (c->d)
        c->date = x -= 150;
    c->name = PANE_W + 10;
    c->name_w = x - c->name - 10;
}

static void draw_list(cairo_t *cr, double bottom)
{
    struct lcols c = {0};
    list_cols(&c);
    double r = files_right(), top = BODY_Y + LHEAD_H;
    char buf[64];
    cairo_save(cr);
    cairo_rectangle(cr, PANE_W, top, r - PANE_W, bottom - top);
    cairo_clip(cr);
    for (int i = 0; i < nitems; i++) {
        double y = top + 2 + i * LROW_H - scroll;
        if (y > bottom || y + LROW_H < top)
            continue;
        int on = i == sel, lit = on && active;
        if (i & 1) {                            /* the Finder's faint stripes */
            rgb(cr, 0xf3f1eb, 1);
            cairo_rectangle(cr, PANE_W, y, r - PANE_W, LROW_H);
            cairo_fill(cr);
        }
        if (on)
            bar(cr, PANE_W + 4, y, r - PANE_W - 8, LROW_H, 4, NULL, lit ? TITLE_MID : 0xe4e6e2, -1, 0);
        unsigned fg = lit ? 0xffffff : TEXT, soft = lit ? 0xffffff : TEXT_SOFT;
        file_pic(cr, &items[i], cwd, c.name, y + 3, 16, 1);
        if (ren.on && i == ren_idx)
            ren_draw(cr, c.name + 20, y + 2, 120);
        else
            text(cr, c.name + 22, y + 15, items[i].name, 12, fg, 0, 0, c.name_w - 22);
        if (c.d) {
            fmt_date(items[i].mtime, buf, sizeof buf);
            text(cr, c.date, y + 15, buf, 11, soft, 0, 0, 140);
        }
        if (c.s) {
            if (items[i].dir)
                snprintf(buf, sizeof buf, "--");
            else
                fmt_size(items[i].size, buf, sizeof buf);
            text_right(cr, c.size + 66, y + 15, buf, 11, soft);
        }
        if (c.k) {
            fmt_kind(&items[i], buf, sizeof buf);
            text(cr, c.kind, y + 15, buf, 11, soft, 0, 0, 120);
        }
    }
    empty_note(cr, top + 30);
    cairo_restore(cr);

    /* the column titles */
    rgb(cr, 0xf4f2ec, 1);
    cairo_rectangle(cr, PANE_W, BODY_Y, r - PANE_W, LHEAD_H);
    cairo_fill(cr);
    rgb(cr, BORDER, 1);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, PANE_W, top - 0.5);
    cairo_line_to(cr, r, top - 0.5);
    double sep[3];
    int ns = 0;
    if (c.d)
        sep[ns++] = c.date;
    if (c.s)
        sep[ns++] = c.size;
    if (c.k)
        sep[ns++] = c.kind;
    for (int k = 0; k < ns; k++) {
        cairo_move_to(cr, sep[k] - 7.5, BODY_Y + 5);
        cairo_line_to(cr, sep[k] - 7.5, top - 5);
    }
    cairo_stroke(cr);
    text(cr, c.name, BODY_Y + 15, "Name", 11, TEXT_SOFT, 1, 0, 0);
    if (c.d)
        text(cr, c.date, BODY_Y + 15, "Date Modified", 11, TEXT_SOFT, 1, 0, 0);
    if (c.s)
        text(cr, c.size + 66 - text_width("Size", 11, 1, 0), BODY_Y + 15, "Size", 11, TEXT_SOFT, 1, 0, 0);
    if (c.k)
        text(cr, c.kind, BODY_Y + 15, "Kind", 11, TEXT_SOFT, 1, 0, 0);
}

/* One folder's column. focus: the folder shown (teal selection); else a
   folder on the way down (its selection, grey, is the next column). */
static void draw_col(cairo_t *cr, double x, const char *dir, const struct item *v, int n, int s,
                     double scr, int focus, double bottom)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, BODY_Y, COL_W, bottom - BODY_Y);
    cairo_clip(cr);
    for (int i = 0; i < n; i++) {
        double y = BODY_Y + 4 + i * CROW_H - scr;
        if (y > bottom || y + CROW_H < BODY_Y)
            continue;
        int on = i == s, lit = on && focus && active;
        if (on)
            bar(cr, x + 4, y, COL_W - 8, CROW_H, 4, NULL, lit ? TITLE_MID : 0xe0ddd4, -1, 0);
        file_pic(cr, &v[i], dir, x + 10, y + 3, 16, 1);
        if (focus && ren.on && i == ren_idx)
            ren_draw(cr, x + 30, y + 2, COL_W - 44);
        else
            text(cr, x + 32, y + 15, v[i].name, 12, lit ? 0xffffff : TEXT, 0, 0, COL_W - 32 - 24);
        if (v[i].dir)
            chevron_right(cr, x + COL_W - 14, y + CROW_H / 2, lit ? 0xffffff : TEXT_SOFT);
    }
    /* a thin scroll mark, the Mac's overlay scroll bar, when they don't fit */
    double content = 8 + n * CROW_H, h = bottom - BODY_Y;
    if (content > h) {
        double th = fmax(h * h / content, 20), ty = BODY_Y + (h - th) * scr / (content - h);
        bar(cr, x + COL_W - 7, ty + 2, 4, th - 4, 2, NULL, 0xb8b2a4, -1, 0);
    }
    cairo_restore(cr);
    rgb(cr, BORDER, 1);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x + COL_W - 0.5, BODY_Y);
    cairo_line_to(cr, x + COL_W - 0.5, bottom);
    cairo_stroke(cr);
}

static void draw_columns(cairo_t *cr, double bottom)
{
    for (int k = 0; k < nanc; k++)
        draw_col(cr, col_x(k), anc[k].path, anc[k].items, anc[k].n, anc[k].sel, anc[k].scroll, 0, bottom);
    double x = col_x(nanc);
    draw_col(cr, x, cwd, items, nitems, sel, scroll, 1, bottom);
    if (!nitems && strcmp(cwd, DISKS))
        text(cr, x + COL_W / 2, BODY_Y + 30, "Empty", 12, TEXT_SOFT, 0, 1, 0);
    if (sel < 0)
        return;
    x = col_x(nanc + 1);
    if (items[sel].dir) {
        draw_col(cr, x, peek.path, peek.items, peek.n, -1, peek.scroll, 0, bottom);
        return;
    }
    /* a file: its preview, name and facts */
    char buf[64];
    double mid = x + COL_W / 2;
    draw_preview(cr, sel, x + 16, BODY_Y + 16, COL_W - 32, 140);
    text(cr, mid, BODY_Y + 178, items[sel].name, 12, TEXT, 1, 1, COL_W - 20);
    fmt_kind(&items[sel], buf, sizeof buf);
    text(cr, mid, BODY_Y + 198, buf, 11, TEXT_SOFT, 0, 1, COL_W - 20);
    fmt_size(items[sel].size, buf, sizeof buf);
    text(cr, mid, BODY_Y + 215, buf, 11, TEXT_SOFT, 0, 1, COL_W - 20);
    fmt_date(items[sel].mtime, buf, sizeof buf);
    text(cr, mid, BODY_Y + 232, buf, 11, TEXT_SOFT, 0, 1, COL_W - 20);
}

static void draw_gallery(cairo_t *cr, double bottom)
{
    double left = PANE_W, right = W, sy = bottom - STRIP_H, mid = (left + right) / 2;
    if (sel >= 0) {
        double top = BODY_Y + 14, h = sy - 50 - top;
        if (h > 16)
            draw_preview(cr, sel, left + 20, top, right - left - 40, h);
        if (ren.on && ren_idx == sel) {
            double tw = text_width(ren.s, 12, 0, 0), bw = tw + 12 < 120 ? 120 : tw + 12;
            ren_draw(cr, mid - bw / 2, sy - 42, bw);
        } else {
            text(cr, mid, sy - 28, items[sel].name, 13, TEXT, 1, 1, right - left - 40);
        }
        char info[96];
        fmt_info(sel, info, sizeof info);
        text(cr, mid, sy - 11, info, 11, TEXT_SOFT, 0, 1, right - left - 40);
    }
    empty_note(cr, BODY_Y + 40);
    rgb(cr, 0xf4f2ec, 1);
    cairo_rectangle(cr, left, sy, right - left, STRIP_H);
    cairo_fill(cr);
    rgb(cr, BORDER, 1);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, left, sy + 0.5);
    cairo_line_to(cr, right, sy + 0.5);
    cairo_stroke(cr);
    double x0 = gallery_x0();
    for (int i = 0; i < nitems; i++) {
        double cx = x0 + i * 64;
        if (cx + 64 < left || cx > right)
            continue;
        if (i == sel)
            bar(cr, cx + 4, sy + 8, 56, 56, 4, NULL, active ? 0xcdeef0 : 0xe4e6e2,
                active ? TEAL_BOTTOM : BORDER, 1);
        file_pic(cr, &items[i], cwd, cx + 8, sy + 12, 48, 1);
    }
}

static void draw_menubar(cairo_t *cr, int open);

static int pop_menubar = -1;            /* the menu bar's menu that is open */
static int pop_view;                    /* the open menu is the View button's */

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    update_k();
    W = gtk_widget_get_allocated_width(w) / K;
    H = gtk_widget_get_allocated_height(w) / K;
    thumbs_this_frame = 0;
    relayout_maybe();
    cairo_scale(cr, K, K);

    rgb(cr, PANEL_WHITE, 1);
    cairo_paint(cr);

    /* menu bar and toolbar */
    rgb(cr, MENU_BAR, 1);
    cairo_rectangle(cr, 0, 0, W, MENU_H);
    cairo_fill(cr);
    draw_menubar(cr, pop_menubar);
    rgb(cr, MENU_BAR, 1);
    cairo_rectangle(cr, 0, TOOL_Y, W, 34);
    cairo_fill(cr);
    rgb(cr, BORDER, 1);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, TOOL_Y);
    cairo_line_to(cr, W, TOOL_Y);
    cairo_stroke(cr);
    for (int i = 0; i < 3; i++) {
        double x = tool_x(i);
        int on = tool_enabled(i);
        if (on && hover.kind == T_TOOL && hover.idx == i) {
            int down = press.kind == T_TOOL && press.idx == i;
            bar(cr, x - 4, TOOL_Y + 3, 29 + text_width(TOOLS[i], 12, 0, 0) + 9, 28, 3, NULL,
                down ? 0xc4e4e7 : 0xd7ecee, 0x8fc7cb, 1);
        }
        icon(cr, TOOL_ICONS[i], x, 27, 24, on ? 1 : 0.4);
        text(cr, x + 29, 44, TOOLS[i], 12, on ? TEXT : DISABLED, 0, 0, 0);
    }

    /* the View button: the layout's picture and the Mac's chevrons */
    int vopen = pop_view;
    if (vopen || hover.kind == T_VIEW)
        bar(cr, VIEW_X, TOOL_Y + 3, VIEW_W, 28, 3, NULL,
            vopen || press.kind == T_VIEW ? 0xc4e4e7 : 0xd7ecee, 0x8fc7cb, 1);
    view_glyph(cr, vmode, VIEW_X + 7, TOOL_Y + 9, TEXT);
    chevrons(cr, VIEW_X + 31, TOOL_Y + 17, TEXT_SOFT);

    /* the address box: a focused Text box while you type in it */
    int disks = !strcmp(cwd, DISKS);
    double aw = W - ADDR_X - 11;
    if (addr.on) {
        rgb(cr, TEAL_BOTTOM, 0.4);
        cairo_set_line_width(cr, 2);
        rrect(cr, ADDR_X - 2, 26, aw + 4, 26, 5);
        cairo_stroke(cr);
        bar(cr, ADDR_X, 28, aw, 22, 3, NULL, 0xffffff, TEAL_RIM, 1.5);
    } else {
        bar(cr, ADDR_X, 28, aw, 22, 3, NULL, 0xffffff, BORDER, 1);
    }
    icon(cr, disks ? "disks" : "folder", ADDR_X + 5, 31, 16, 1);
    cairo_save(cr);
    cairo_rectangle(cr, ADDR_X + 23, 28, aw - 26, 22);
    cairo_clip(cr);
    if (addr.on)
        edit_draw(cr, &addr, ADDR_X + 26, 44, 12, 0);
    else
        text(cr, ADDR_X + 26, 44, disks ? "Disks" : cwd, 12, TEXT, 0, 0, aw - 30);
    cairo_restore(cr);

    /* the task pane */
    double bottom = files_bottom();
    cairo_rectangle(cr, 0, BODY_Y, PANE_W, bottom - BODY_Y);
    cairo_set_source(cr, grad(0, BODY_Y, PANE_W, bottom - BODY_Y, 0, SAND, SAND_DEEP));
    cairo_pattern_destroy(cairo_get_source(cr));
    cairo_fill(cr);
    cairo_save(cr);
    cairo_rectangle(cr, 0, BODY_Y, PANE_W, bottom - BODY_Y);
    cairo_clip(cr);
    for (int g = 0; g < ngroups; g++) {
        double gy = groups[g].y;
        rrect(cr, 8, gy, PANE_W - 16, groups[g].h, 6);
        rgb(cr, 0xffffff, 0.55);
        cairo_fill_preserve(cr);
        rgb(cr, BORDER, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        icon(cr, "forward", 14, gy + 6, 16, 1);
        text(cr, 36, gy + 19, groups[g].title, 12, LABEL_TEAL, 1, 0, 0);
        for (int k = 0; k < groups[g].n; k++) {
            int ti = groups[g].first + k;
            int ok = enabled(tasks[ti].act);
            icon(cr, tasks[ti].icon, 16, gy + 30 + k * 25, 18, ok ? 1 : 0.4);
            int on = hover.kind == T_TASK && hover.idx == ti;
            double tw = text(cr, 40, gy + 44 + k * 25, tasks[ti].label, 11,
                             !ok ? DISABLED : on ? LABEL_TEAL : TEXT, 0, 0, 0);
            if (on) {               /* a link, the way it looks under the mouse */
                rgb(cr, LABEL_TEAL, 1);
                cairo_rectangle(cr, 40, gy + 45.5 + k * 25, tw, 1);
                cairo_fill(cr);
            }
        }
    }
    cairo_restore(cr);

    /* the files */
    cairo_save(cr);
    cairo_rectangle(cr, PANE_W, BODY_Y, files_right() - PANE_W, bottom - BODY_Y);
    cairo_clip(cr);
    switch (vmode) {
    case V_ICONS: draw_icons(cr, bottom); break;
    case V_LIST: draw_list(cr, bottom); break;
    case V_COLUMNS: draw_columns(cr, bottom); break;
    case V_GALLERY: draw_gallery(cr, bottom); break;
    }
    cairo_restore(cr);

    /* the scroll bar, when the files don't fit */
    if (has_scroll) {
        double sx = W - SCROLL_W, ty, th;
        rgb(cr, MENU_BAR, 1);
        cairo_rectangle(cr, sx, BODY_Y, SCROLL_W, bottom - BODY_Y);
        cairo_fill(cr);
        rgb(cr, BORDER, 1);
        cairo_move_to(cr, sx + 0.5, BODY_Y);
        cairo_line_to(cr, sx + 0.5, bottom);
        cairo_stroke(cr);
        scroll_thumb(&ty, &th);
        int on = hover.kind == T_THUMB || press.kind == T_THUMB;
        bar(cr, sx + 2.5, ty, SCROLL_W - 5, th, 4,
            grad(sx + 2.5, ty, SCROLL_W - 5, th, 1, on ? 0xeffdfd : TEAL_TOP, on ? 0x7fd8de : TEAL_BOTTOM),
            0, TEAL_RIM, 1);
    }

    /* the status bar */
    rgb(cr, MENU_BAR, 1);
    cairo_rectangle(cr, 0, H - 23, W, 23);
    cairo_fill(cr);
    rgb(cr, BORDER, 1);
    cairo_move_to(cr, 0, H - 24);
    cairo_line_to(cr, W, H - 24);
    cairo_stroke(cr);
    char st[300];
    if (status_msg[0]) {
        snprintf(st, sizeof st, "%s", status_msg);
    } else if (disks) {
        struct statvfs v;
        if (sel >= 0 && !statvfs(items[sel].name, &v))
            snprintf(st, sizeof st, "%s: %.1f GB free of %.1f GB", items[sel].name,
                     (double)v.f_bavail * v.f_frsize / 1e9, (double)v.f_blocks * v.f_frsize / 1e9);
        else
            snprintf(st, sizeof st, "%d disk%s", nitems, nitems == 1 ? "" : "s");
    } else {
        long long total = 0;
        for (int i = 0; i < nitems; i++)
            total += items[i].size;
        char size[32];
        if (total < 1024)
            snprintf(size, sizeof size, "%lld bytes", total);
        else if (total < 1024 * 1024 * 10LL)
            snprintf(size, sizeof size, "%lld KB", (total + 1023) / 1024);
        else if (total < 1024LL * 1024 * 1024)
            snprintf(size, sizeof size, "%.1f MB", total / 1048576.0);
        else
            snprintf(size, sizeof size, "%.1f GB", total / 1073741824.0);
        snprintf(st, sizeof st, "%d object%s, %s", nitems, nitems == 1 ? "" : "s", size);
    }
    text(cr, 9, H - 8, st, 11, TEXT_SOFT, 0, 0, W - 40);
    /* Two strokes in the corner, so it reads as a place to drag and resize.
       They run down toward the corner ( \ ), the way a window's grip does,
       parallel and clear of the round corner. */
    rgb(cr, 0x8a8478, 1);
    cairo_set_line_width(cr, 1.25);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 2; i++) {
        double x = W - 8 - i * 4, y = H - 14;
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x - 8, y + 8);
        cairo_stroke(cr);
    }
    return TRUE;
}

/* The menu bar: an item lights up under the mouse, and goes teal while
   its menu is open (the theme's "Menu bar item"). */
static void draw_menubar(cairo_t *cr, int open)
{
    for (int i = 0; i < 5; i++) {
        double x = menu_x(i), tw = text_width(MENUS[i], 12, 0, 0);
        if (i == open) {
            bar(cr, x - 6, 2, tw + 12, 19, 3, grad(0, 2, 0, 19, 0, 0x3aa7ae, 0x1f7f86), 0, 0x17666c, 1);
            text(cr, x, 15, MENUS[i], 12, 0xffffff, 0, 0, 0);
        } else {
            if (hover.kind == T_MENU && hover.idx == i)
                bar(cr, x - 6, 2, tw + 12, 19, 3, NULL, 0xd7ecee, 0x8fc7cb, 1);
            text(cr, x, 15, MENUS[i], 12, TEXT, 0, 0, 0);
        }
    }
}

/* --- menus --------------------------------------------------------------------- */
struct mitem { const char *label, *keys; int act; };   /* label NULL: a line */
static const struct mitem M_FILE[] = {
    { "Open", "Enter", A_OPEN }, { "New Folder", "Ctrl+Shift+N", A_NEWFOLDER },
    { "Rename", "F2", A_RENAME }, { "Delete", "Delete", A_DELETE }, { NULL, NULL, 0 },
    { "Close", "Ctrl+W", A_CLOSE }, { NULL, NULL, -1 } };
static const struct mitem M_EDIT[] = {
    { "Copy", "Ctrl+C", A_COPY }, { "Paste", "Ctrl+V", A_PASTE }, { NULL, NULL, -1 } };
static const struct mitem M_VIEW[] = {
    { "as Icons", "Ctrl+1", A_V_ICONS }, { "as List", "Ctrl+2", A_V_LIST },
    { "as Columns", "Ctrl+3", A_V_COLUMNS }, { "as Gallery", "Ctrl+4", A_V_GALLERY },
    { NULL, NULL, 0 }, { "Hidden Files", "Ctrl+H", A_HIDDEN }, { NULL, NULL, 0 },
    { "Refresh", "F5", A_REFRESH }, { NULL, NULL, -1 } };
/* The View button's menu: just the four, as on the Mac. */
static const struct mitem M_VIEWS[] = {
    { "as Icons", "", A_V_ICONS }, { "as List", "", A_V_LIST },
    { "as Columns", "", A_V_COLUMNS }, { "as Gallery", "", A_V_GALLERY }, { NULL, NULL, -1 } };
static const struct mitem M_TOOLS[] = {
    { "Open Terminal Here", "", A_TERMINAL }, { "Disks", "", A_DISKS },
    { "Settings", "", A_SETTINGS }, { NULL, NULL, -1 } };
static const struct mitem M_HELP[] = { { "About My Files", "", A_ABOUT }, { NULL, NULL, -1 } };
static const struct mitem *const MENU_ITEMS[] = { M_FILE, M_EDIT, M_VIEW, M_TOOLS, M_HELP };
static const struct mitem M_ON_FILE[] = {
    { "Open", "", A_OPEN }, { NULL, NULL, 0 }, { "Copy", "", A_COPY }, { "Put Back", "", A_PUTBACK },
    { "Delete", "", A_DELETE },
    { "Rename", "", A_RENAME }, { NULL, NULL, -1 } };
static const struct mitem M_ON_FOLDER[] = {
    { "New Folder", "", A_NEWFOLDER }, { "Paste", "", A_PASTE }, { NULL, NULL, 0 },
    { "Open Terminal Here", "", A_TERMINAL }, { "Refresh", "", A_REFRESH }, { NULL, NULL, -1 } };

static int writable_here(void) { return strcmp(cwd, DISKS) && !access(cwd, W_OK); }

/* Menu items with a tick: Hidden Files when on, the layout in use. */
static int ticked(int a)
{
    if (a == A_HIDDEN)
        return show_hidden;
    return a >= A_V_ICONS && a <= A_V_GALLERY && vmode == a - A_V_ICONS;
}

static int enabled(int a)
{
    int disks = !strcmp(cwd, DISKS);
    switch (a) {
    case A_OPEN: return sel >= 0;
    case A_RENAME: case A_DELETE: return sel >= 0 && !disks;
    case A_PUTBACK: return sel >= 0 && in_trash();
    case A_EMPTY: return in_trash();
    case A_COPY: return sel >= 0 && !disks;
    case A_NEWFOLDER: return writable_here();
    case A_PASTE: return clip[0] && writable_here();
    case A_TERMINAL: return !disks;
    default: return 1;
    }
}

static struct {
    GtkWidget *win, *da;
    const struct mitem *items;
    int n, hover;
    double w, h;
} pop;

#define ROW_H 24
#define SEP_H 9

static double pop_row_y(int i)
{
    double y = 4;
    for (int k = 0; k < i; k++)
        y += pop.items[k].label ? ROW_H : SEP_H;
    return y;
}

static int pop_hit(double x, double y)
{
    if (x < 0 || x >= pop.w)
        return -1;
    for (int i = 0; i < pop.n; i++) {
        double ry = pop_row_y(i);
        if (pop.items[i].label && y >= ry && y < ry + ROW_H)
            return i;
    }
    return -1;
}

/* Drawn like the Start menu's white column: white, a hairline frame in the
   title bar's dark edge, the Menu highlight under the mouse. */
static gboolean pop_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)w;
    (void)d;
    cairo_scale(cr, K, K);
    rgb(cr, PANEL_WHITE, 1);
    cairo_paint(cr);
    rgb(cr, TITLE_EDGE, 1);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, pop.w - 1, pop.h - 1);
    cairo_stroke(cr);
    for (int i = 0; i < pop.n; i++) {
        const struct mitem *m = &pop.items[i];
        double y = pop_row_y(i);
        if (!m->label) {
            rgb(cr, BORDER, 1);
            cairo_move_to(cr, 8, y + 4.5);
            cairo_line_to(cr, pop.w - 8, y + 4.5);
            cairo_stroke(cr);
            continue;
        }
        int on = enabled(m->act), lit = on && i == pop.hover;
        if (lit)
            bar(cr, 3, y + 1, pop.w - 6, ROW_H - 2, 3, grad(0, y + 1, 0, ROW_H - 2, 0, 0x3aa7ae, 0x1f7f86),
                0, 0x17666c, 1);
        unsigned c = lit ? 0xffffff : on ? TEXT : DISABLED;
        if (ticked(m->act)) {                        /* a tick */
            rgb(cr, c, 1);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, 9, y + 12);
            cairo_line_to(cr, 13, y + 16);
            cairo_line_to(cr, 20, y + 7);
            cairo_stroke(cr);
        }
        text(cr, 28, y + 16, m->label, 12, c, 0, 0, 0);
        if (m->keys && m->keys[0])
            text(cr, pop.w - 12 - text_width(m->keys, 11, 0, 0), y + 16, m->keys, 11,
                 lit ? 0xffffff : on ? TEXT_SOFT : DISABLED, 0, 0, 0);
    }
    return TRUE;
}

static void pop_close(void)
{
    if (!pop.win)
        return;
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
    gtk_widget_destroy(pop.win);
    pop.win = NULL;
    pop_menubar = -1;
    pop_view = 0;
    redraw();
}

static void pop_open(const struct mitem *items, int menubar, double x, double y);

static void pop_pick(int i)
{
    if (i < 0 || !pop.items[i].label || !enabled(pop.items[i].act))
        return;
    int a = pop.items[i].act;
    pop_close();
    do_action(a);
}

static gboolean pop_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
    (void)w;
    (void)d;
    int h = pop_hit(ev->x / K, ev->y / K);
    if (h != pop.hover) {
        pop.hover = h;
        gtk_widget_queue_draw(pop.da);
    }
    /* Sliding along the menu bar with a menu open opens the next one. */
    if (pop_menubar >= 0) {
        int ox, oy, px, py;
        gdk_window_get_origin(gtk_widget_get_window(da), &ox, &oy);
        gdk_window_get_origin(gtk_widget_get_window(pop.da), &px, &py);
        double mx = (ev->x + px - ox) / K, my = (ev->y + py - oy) / K;
        struct target t = hit(mx, my);
        if (t.kind == T_MENU && t.idx != pop_menubar) {
            pop_close();
            pop_open(MENU_ITEMS[t.idx], t.idx, menu_x(t.idx) - 6, MENU_H);
        }
    }
    return TRUE;
}

static gboolean pop_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w;
    (void)d;
    double x = ev->x / K, y = ev->y / K;
    if (x < 0 || y < 0 || x >= pop.w || y >= pop.h) {     /* outside: close */
        if (ev->type == GDK_BUTTON_PRESS)
            pop_close();
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_RELEASE)
        pop_pick(pop_hit(x, y));
    return TRUE;
}

static void pop_step(int dir)
{
    for (int k = 0; k < pop.n; k++) {
        pop.hover = (pop.hover + dir + pop.n) % pop.n;
        if (pop.items[pop.hover].label && enabled(pop.items[pop.hover].act))
            break;
    }
    gtk_widget_queue_draw(pop.da);
}

static gboolean pop_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    (void)d;
    switch (ev->keyval) {
    case GDK_KEY_Escape: pop_close(); break;
    case GDK_KEY_Down: pop_step(1); break;
    case GDK_KEY_Up: pop_step(-1); break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_space: pop_pick(pop.hover); break;
    case GDK_KEY_Left: case GDK_KEY_Right:
        if (pop_menubar >= 0) {
            int m = (pop_menubar + (ev->keyval == GDK_KEY_Right ? 1 : 4)) % 5;
            pop_close();
            pop_open(MENU_ITEMS[m], m, menu_x(m) - 6, MENU_H);
            pop_step(1);
        }
        break;
    }
    return TRUE;
}

static gboolean pop_mapped(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)ev;
    (void)d;
    gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()), gtk_widget_get_window(w),
                  GDK_SEAT_CAPABILITY_ALL_POINTING | GDK_SEAT_CAPABILITY_KEYBOARD, TRUE, NULL, NULL,
                  NULL, NULL);
    return FALSE;
}

/* A menu at (x, y) in the window, in mockup units. */
static void pop_open(const struct mitem *items, int menubar, double x, double y)
{
    pop_close();
    pop.items = items;
    pop.hover = -1;
    double lw = 0, kw = 0;
    for (pop.n = 0; items[pop.n].act >= 0; pop.n++) {
        if (!items[pop.n].label)
            continue;
        double a = text_width(items[pop.n].label, 12, 0, 0);
        double b = items[pop.n].keys ? text_width(items[pop.n].keys, 11, 0, 0) : 0;
        lw = a > lw ? a : lw;
        kw = b > kw ? b : kw;
    }
    pop.w = 28 + lw + (kw ? 28 + kw : 0) + 16;
    if (pop.w < 160)
        pop.w = 160;
    pop.h = pop_row_y(pop.n) + 4;
    pop_menubar = menubar;

    int ox, oy;
    gdk_window_get_origin(gtk_widget_get_window(da), &ox, &oy);
    pop.win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_transient_for(GTK_WINDOW(pop.win), GTK_WINDOW(win));
    pop.da = gtk_drawing_area_new();
    gtk_widget_set_size_request(pop.da, (int)ceil(pop.w * K), (int)ceil(pop.h * K));
    gtk_container_add(GTK_CONTAINER(pop.win), pop.da);
    gtk_widget_add_events(pop.da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    gtk_widget_add_events(pop.win, GDK_KEY_PRESS_MASK);
    g_signal_connect(pop.da, "draw", G_CALLBACK(pop_draw), NULL);
    g_signal_connect(pop.da, "motion-notify-event", G_CALLBACK(pop_motion), NULL);
    g_signal_connect(pop.da, "button-press-event", G_CALLBACK(pop_button), NULL);
    g_signal_connect(pop.da, "button-release-event", G_CALLBACK(pop_button), NULL);
    g_signal_connect(pop.win, "key-press-event", G_CALLBACK(pop_key), NULL);
    g_signal_connect(pop.win, "map-event", G_CALLBACK(pop_mapped), NULL);
    gtk_window_move(GTK_WINDOW(pop.win), ox + (int)(x * K), oy + (int)(y * K));
    gtk_widget_show_all(pop.win);
    redraw();
}

/* --- dialogs -------------------------------------------------------------- */
/* "Are you sure...?" in the theme: white, the message, and Push buttons
   (75x23, the default one with a teal border, amber under the mouse). */
static struct {
    GtkWidget *win;
    const char *msg, *icon, *ok, *cancel;
    double w, h;
    int hover, pressed, result;
    GMainLoop *loop;
} dlg;

static void dlg_button(int b, double *x, double *y)
{
    *y = dlg.h - 36;
    *x = dlg.cancel ? (b == 0 ? dlg.w - 172 : dlg.w - 91) : dlg.w - 91;
}

static int dlg_hit(double x, double y)
{
    for (int b = 0; b < (dlg.cancel ? 2 : 1); b++) {
        double bx, by;
        dlg_button(b, &bx, &by);
        if (x >= bx && x < bx + 75 && y >= by && y < by + 23)
            return b;
    }
    return -1;
}

static gboolean dlg_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)w;
    (void)d;
    cairo_scale(cr, K, K);
    rgb(cr, PANEL_WHITE, 1);
    cairo_paint(cr);
    double tx = 16;
    if (dlg.icon) {
        icon(cr, dlg.icon, 16, 16, 32, 1);
        tx = 60;
    }
    PangoContext *pc = pango_cairo_create_context(cr);
    font_options(pc);
    PangoLayout *l = layout(pc, dlg.msg, 12, 0, 0);
    pango_layout_set_width(l, (int)((dlg.w - tx - 16) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    rgb(cr, TEXT, 1);
    cairo_move_to(cr, tx, 18);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    g_object_unref(pc);
    for (int b = 0; b < (dlg.cancel ? 2 : 1); b++) {
        double x, y;
        dlg_button(b, &x, &y);
        int down = dlg.pressed == b && dlg.hover == b;
        cairo_pattern_t *f = down ? grad(x, y, 75, 23, 0, 0xe2ddd2, 0xf6f4ef)
                                  : grad(x, y, 75, 23, 0, 0xffffff, 0xe6e2d8);
        if (dlg.hover == b && !down)
            bar(cr, x, y, 75, 23, 4, f, 0, AMBER_MID, 2);
        else if (b == 0)
            bar(cr, x, y, 75, 23, 4, f, 0, TEAL_RIM, 2);
        else
            bar(cr, x, y, 75, 23, 4, f, 0, 0xa59d8c, 1);
        text(cr, x + 37.5, y + 11.5 + 12 * 0.36, b == 0 ? dlg.ok : dlg.cancel, 12, TEXT, 0, 1, 0);
    }
    return TRUE;
}

static void dlg_done(int r)
{
    dlg.result = r;
    if (dlg.loop)
        g_main_loop_quit(dlg.loop);
}

static gboolean dlg_event(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)d;
    int h;
    switch (ev->type) {
    case GDK_MOTION_NOTIFY:
        h = dlg_hit(ev->motion.x / K, ev->motion.y / K);
        if (h != dlg.hover) {
            dlg.hover = h;
            gtk_widget_queue_draw(w);
        }
        return TRUE;
    case GDK_BUTTON_PRESS:
        dlg.pressed = dlg_hit(ev->button.x / K, ev->button.y / K);
        gtk_widget_queue_draw(w);
        return TRUE;
    case GDK_BUTTON_RELEASE:
        h = dlg_hit(ev->button.x / K, ev->button.y / K);
        if (h >= 0 && h == dlg.pressed)
            dlg_done(h == 0);
        dlg.pressed = -1;
        gtk_widget_queue_draw(w);
        return TRUE;
    case GDK_KEY_PRESS:
        if (ev->key.keyval == GDK_KEY_Return || ev->key.keyval == GDK_KEY_KP_Enter)
            dlg_done(1);
        else if (ev->key.keyval == GDK_KEY_Escape)
            dlg_done(0);
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean dlg_delete(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)w;
    (void)ev;
    (void)d;
    dlg_done(0);
    return TRUE;
}

/* 1 for ok, 0 for cancel (or the window closed). */
static int ask(const char *title, const char *icon_name, const char *msg, const char *ok, const char *cancel)
{
    dlg.msg = msg;
    dlg.icon = icon_name;
    dlg.ok = ok;
    dlg.cancel = cancel;
    dlg.hover = dlg.pressed = -1;
    dlg.result = 0;
    dlg.w = 380;
    PangoLayout *l = layout(measure_ctx, msg, 12, 0, 0);
    pango_layout_set_width(l, (int)((dlg.w - (icon_name ? 60 : 16) - 16) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    PangoRectangle r;
    pango_layout_get_extents(l, NULL, &r);
    g_object_unref(l);
    double th = r.height / (double)PANGO_SCALE;
    dlg.h = (th > 32 ? th : 32) + 18 + 20 + 48;

    dlg.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dlg.win), title);
    gtk_window_set_transient_for(GTK_WINDOW(dlg.win), GTK_WINDOW(win));
    gtk_window_set_modal(GTK_WINDOW(dlg.win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(dlg.win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dlg.win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg.win), FALSE);
    gtk_window_set_position(GTK_WINDOW(dlg.win), GTK_WIN_POS_CENTER_ON_PARENT);
    GtkWidget *a = gtk_drawing_area_new();
    gtk_widget_set_size_request(a, (int)ceil(dlg.w * K), (int)ceil(dlg.h * K));
    gtk_widget_add_events(a, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_widget_add_events(dlg.win, GDK_KEY_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(dlg.win), a);
    g_signal_connect(a, "draw", G_CALLBACK(dlg_draw), NULL);
    g_signal_connect(a, "event", G_CALLBACK(dlg_event), NULL);
    g_signal_connect(dlg.win, "key-press-event", G_CALLBACK(dlg_event), NULL);
    g_signal_connect(dlg.win, "delete-event", G_CALLBACK(dlg_delete), NULL);
    gtk_widget_show_all(dlg.win);
    dlg.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(dlg.loop);
    g_main_loop_unref(dlg.loop);
    dlg.loop = NULL;
    gtk_widget_destroy(dlg.win);
    dlg.win = NULL;
    return dlg.result;
}

static void oops(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void oops(const char *fmt, ...)
{
    char m[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    ask("My Files", NULL, m, "OK", NULL);
}

/* --- doing things ----------------------------------------------------------- */
static void spawn(const char *dir, const char *const *argv)
{
    GError *e = NULL;
    if (!g_spawn_async(dir, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
        oops("Can't start %s: %s", argv[0], e->message);
        g_error_free(e);
    }
}

static void open_item(int i)
{
    if (i < 0 || i >= nitems)
        return;
    char path[PATH_MAX];
    if (!strcmp(cwd, DISKS))
        snprintf(path, sizeof path, "%s", items[i].name);
    else
        join(path, sizeof path, cwd, items[i].name);
    if (items[i].dir) {
        navigate(path, 1);
        return;
    }
    const char *name = items[i].name, *prog = NULL;
    if (has_ext(name, PDF_EXT))
        prog = "myos-preview";
    else if (has_ext(name, WP_EXT))
        prog = "writeperfect";
    else if (has_ext(name, DOC_EXT))
        prog = "myos-office";     /* LibreOffice, or AbiWord without it */
    else if (has_ext(name, LO_EXT))
        prog = "libreoffice";
    else if (has_ext(name, WEB_EXT))
        prog = "myos-browser";      /* Chromium or Firefox, whichever the disk has */
    else if (has_ext(name, MEDIA_EXT)) {
        const char *argv[] = { "myos-player", path, NULL };     /* Player: QuickTime's look */
        spawn(cwd, argv);
        say("Opening %s...", name);
        return;
    } else if (!strcmp(items[i].icon, "text-file"))
        prog = "geany";
    if (!prog) {
        const char *dot = strrchr(name, '.');
        oops(dot ? "MyOS doesn't have a program that opens .%s files yet."
                 : "MyOS doesn't know which program opens \"%s\".", dot ? dot + 1 : name);
        return;
    }
    const char *argv[] = { prog, path, NULL };
    spawn(cwd, argv);
    say("Opening %s...", name);
}

static void start_rename(int i)
{
    if (i < 0 || !strcmp(cwd, DISKS))
        return;
    ren_idx = i;
    ren.on = 1;
    snprintf(ren.s, sizeof ren.s, "%s", items[i].name);
    ren.caret = (int)strlen(ren.s);
    ren.all = 1;
    addr.on = 0;
    redraw();
}

static void finish_rename(int keep)
{
    if (!ren.on)
        return;
    ren.on = 0;
    if (!keep || ren_idx < 0 || ren_idx >= nitems || !strcmp(ren.s, items[ren_idx].name) || !ren.s[0]) {
        redraw();
        return;
    }
    if (strchr(ren.s, '/') || !strcmp(ren.s, ".") || !strcmp(ren.s, "..")) {
        oops("A file name can't contain a / or be . or ..");
        redraw();
        return;
    }
    char from[PATH_MAX], to[PATH_MAX];
    join(from, sizeof from, cwd, items[ren_idx].name);
    join(to, sizeof to, cwd, ren.s);
    if (!access(to, F_OK))
        oops("There is already a file called \"%s\" here.", ren.s);
    else if (rename(from, to))
        oops("Can't rename \"%s\": %s", items[ren_idx].name, strerror(errno));
    else {
        char a[PATH_MAX], b[PATH_MAX];
        sidecar_of(from, a, sizeof a);
        sidecar_of(to, b, sizeof b);
        rename(a, b);
    }
    reload(ren.s);
}

static void new_folder(void)
{
    char path[PATH_MAX], name[64];
    for (int n = 1; n < 1000; n++) {
        if (n == 1)
            snprintf(name, sizeof name, "New Folder");
        else
            snprintf(name, sizeof name, "New Folder (%d)", n);
        join(path, sizeof path, cwd, name);
        if (!mkdir(path, 0755)) {
            reload(name);
            start_rename(sel);
            return;
        }
        if (errno != EEXIST) {
            oops("Can't make a folder here: %s", strerror(errno));
            return;
        }
    }
}

static void copy_done(GPid pid, gint status, gpointer data)
{
    char *name = data;
    g_spawn_close_pid(pid);
    if (g_spawn_check_exit_status(status, NULL))
        say("Copied %s", name);
    else
        oops("The copy of \"%s\" didn't finish.", name);
    reload(name);
    g_free(name);
}

static void paste(void)
{
    if (!clip[0])
        return;
    const char *base = strrchr(clip, '/');
    base = base ? base + 1 : clip;
    char name[NAME_MAX + 32], to[PATH_MAX];
    snprintf(name, sizeof name, "%s", base);
    join(to, sizeof to, cwd, name);
    /* Pasting next to the original makes "Copy of x", as Windows does. */
    for (int n = 1; !access(to, F_OK) && n < 1000; n++) {
        if (n == 1)
            snprintf(name, sizeof name, "Copy of %s", base);
        else
            snprintf(name, sizeof name, "Copy (%d) of %s", n, base);
        join(to, sizeof to, cwd, name);
    }
    size_t cl = strlen(clip);
    if (!strncmp(to, clip, cl) && to[cl] == '/') {
        oops("\"%s\" can't be copied into itself.", base);
        return;
    }
    const char *argv[] = { "cp", "-a", "--", clip, to, NULL };
    GPid pid;
    GError *e = NULL;
    if (!g_spawn_async(cwd, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &pid, &e)) {
        oops("Can't copy: %s", e->message);
        g_error_free(e);
        return;
    }
    g_child_watch_add(pid, copy_done, g_strdup(name));
    {
        char side[PATH_MAX], sideto[PATH_MAX];
        gchar *data = NULL;
        gsize len = 0;
        sidecar_of(clip, side, sizeof side);
        sidecar_of(to, sideto, sizeof sideto);
        if (g_file_get_contents(side, &data, &len, NULL)) {
            g_file_set_contents(sideto, data, (gssize)len, NULL);
            g_free(data);
        }
    }
    say("Copying %s...", base);
}

/* `rm` is ours: it moves to ~/Trash and writes ._name.where (path, uid,
   gid, mode). --reallyDelete is the Unix rm. */
static int run_rm(char **argv)
{
    gint status = 0;
    GError *e = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                      &status, &e) || !g_spawn_check_exit_status(status, NULL)) {
        if (e) {
            oops("%s", e->message);
            g_error_free(e);
        } else {
            oops("rm could not finish.");
        }
        return -1;
    }
    return 0;
}

static void delete_item(void)
{
    if (sel < 0 || !strcmp(cwd, DISKS))
        return;
    char path[PATH_MAX], m[NAME_MAX + 160], name[NAME_MAX + 1];
    snprintf(name, sizeof name, "%s", items[sel].name);
    join(path, sizeof path, cwd, name);
    int gone = in_trash();
    if (gone)
        snprintf(m, sizeof m, "Delete \"%s\" for good? This cannot be undone.", name);
    else
        snprintf(m, sizeof m, "Send \"%s\" to the Recycle Bin?", name);
    if (!ask("Delete", "recycle", m, gone ? "Delete" : "Yes", gone ? "Cancel" : "No"))
        return;
    int next = sel + 1 < nitems ? sel + 1 : sel - 1;
    char keep[NAME_MAX + 1] = "";
    if (next >= 0)
        snprintf(keep, sizeof keep, "%s", items[next].name);
    char *argv_trash[] = { "rm", "-r", "--", path, NULL };
    char *argv_gone[] = { "rm", "--reallyDelete", "-rf", "--", path, NULL };
    if (run_rm(gone ? argv_gone : argv_trash))
        return;
    reload(keep[0] ? keep : NULL);
}

static void put_back_item(void)
{
    if (sel < 0 || !in_trash())
        return;
    char *argv[] = { "rm", "--putBack", items[sel].name, NULL };
    if (run_rm(argv))
        return;
    reload(NULL);
}

static void empty_trash_here(void)
{
    if (!in_trash())
        return;
    if (!ask("Empty Trash", "recycle",
             "Empty the Recycle Bin? Items in it will be deleted for good.",
             "Empty Trash", "Cancel"))
        return;
    char *argv[] = { "rm", "--emptyTrash", NULL };
    run_rm(argv);
    reload(NULL);
}

static void go_back(int forward)
{
    GPtrArray *from = forward ? fwd_hist : back_hist, *to = forward ? back_hist : fwd_hist;
    if (!from->len)
        return;
    char *p = g_strdup(g_ptr_array_index(from, from->len - 1));
    g_ptr_array_remove_index(from, from->len - 1);
    g_ptr_array_add(to, g_strdup(cwd));
    navigate(p, 0);
    g_free(p);
}

static void go_up(void)
{
    if (!tool_enabled(2))
        return;
    char up[PATH_MAX];
    snprintf(up, sizeof up, "%s", cwd);
    char *s = strrchr(up, '/');
    if (s == up)
        up[1] = 0;
    else if (s)
        *s = 0;
    navigate(up, 1);
}

/* The layout is remembered for next time in ~/.config/myos/files.conf. */
static void conf_path(char *out, size_t size, int dir_only)
{
    const char *home = getenv("HOME");
    snprintf(out, size, "%s/.config/myos%s", home ? home : "", dir_only ? "" : "/files.conf");
}

static void load_view(void)
{
    char path[PATH_MAX], line[64];
    conf_path(path, sizeof path, 0);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        for (int v = 0; v < 4; v++)
            if (!strncmp(line, "view=", 5) && !strncmp(line + 5, VIEW_NAMES[v], strlen(VIEW_NAMES[v])))
                vmode = v;
    fclose(f);
}

static void save_view(void)
{
    char path[PATH_MAX];
    conf_path(path, sizeof path, 1);
    g_mkdir_with_parents(path, 0755);
    conf_path(path, sizeof path, 0);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "view=%s\n", VIEW_NAMES[vmode]);
        fclose(f);
    }
}

static void ensure_visible(int i);

static void set_view(int v)
{
    if (v == vmode)
        return;
    vmode = v;
    scroll = 0;
    if (vmode == V_GALLERY && sel < 0 && nitems)
        sel = 0;
    save_view();
    relayout();
    if (sel >= 0)
        ensure_visible(sel);
    redraw();
}

static void do_action(int a)
{
    if (!enabled(a))
        return;
    switch (a) {
    case A_OPEN: open_item(sel); break;
    case A_NEWFOLDER: new_folder(); break;
    case A_RENAME: start_rename(sel); break;
    case A_COPY:
        join(clip, sizeof clip, cwd, items[sel].name);
        say("Copied %s: open a folder and choose Edit > Paste", items[sel].name);
        break;
    case A_PASTE: paste(); break;
    case A_DELETE: delete_item(); break;
    case A_PUTBACK: put_back_item(); break;
    case A_EMPTY: empty_trash_here(); break;
    case A_CLOSE: gtk_widget_destroy(win); break;
    case A_HIDDEN:
        show_hidden = !show_hidden;
        reload(sel >= 0 ? items[sel].name : NULL);
        break;
    case A_REFRESH: reload(sel >= 0 ? items[sel].name : NULL); break;
    case A_TERMINAL: { const char *argv[] = { "xterm", NULL }; spawn(cwd, argv); break; }
    case A_SETTINGS: { const char *argv[] = { "myos-settings", NULL }; spawn(NULL, argv); break; }
    case A_NETWORK: { const char *argv[] = { "myos-settings", "network", NULL }; spawn(NULL, argv); break; }
    case A_ABOUT:
        ask("About My Files", "my-files", "My Files\n\nThe file explorer of MyOS, drawn from the "
            "Golden Hour theme.", "OK", NULL);
        break;
    case A_DISKS: navigate(DISKS, 1); break;
    case A_HOME: navigate(getenv("HOME") ? getenv("HOME") : "/", 1); break;
    case A_BACK: go_back(0); break;
    case A_FORWARD: go_back(1); break;
    case A_UP: go_up(); break;
    case A_V_ICONS: case A_V_LIST: case A_V_COLUMNS: case A_V_GALLERY: set_view(a - A_V_ICONS); break;
    }
}

/* --- the line editor ---------------------------------------------------------- */
static void edit_insert(struct edit *e, const char *s)
{
    if (e->all) {
        e->s[0] = 0;
        e->caret = 0;
        e->all = 0;
    }
    size_t n = strlen(s), len = strlen(e->s);
    if (len + n >= sizeof e->s)
        return;
    memmove(e->s + e->caret + n, e->s + e->caret, len - e->caret + 1);
    memcpy(e->s + e->caret, s, n);
    e->caret += (int)n;
}

/* 1 if the key was the editor's. */
static int edit_key(struct edit *e, GdkEventKey *ev)
{
    int ctrl = ev->state & GDK_CONTROL_MASK;
    int len = (int)strlen(e->s);
    switch (ev->keyval) {
    case GDK_KEY_BackSpace:
        if (e->all) {
            e->s[0] = 0;
            e->caret = 0;
            e->all = 0;
        } else if (e->caret > 0) {
            int p = (int)(g_utf8_prev_char(e->s + e->caret) - e->s);
            memmove(e->s + p, e->s + e->caret, len - e->caret + 1);
            e->caret = p;
        }
        return 1;
    case GDK_KEY_Delete:
        if (e->all) {
            e->s[0] = 0;
            e->caret = 0;
            e->all = 0;
        } else if (e->caret < len) {
            int n = (int)(g_utf8_next_char(e->s + e->caret) - e->s);
            memmove(e->s + e->caret, e->s + n, len - n + 1);
        }
        return 1;
    case GDK_KEY_Left:
        if (e->all)
            e->caret = 0;
        else if (e->caret > 0)
            e->caret = (int)(g_utf8_prev_char(e->s + e->caret) - e->s);
        e->all = 0;
        return 1;
    case GDK_KEY_Right:
        if (e->all)
            e->caret = len;
        else if (e->caret < len)
            e->caret = (int)(g_utf8_next_char(e->s + e->caret) - e->s);
        e->all = 0;
        return 1;
    case GDK_KEY_Home: e->caret = 0; e->all = 0; return 1;
    case GDK_KEY_End: e->caret = len; e->all = 0; return 1;
    }
    if (ctrl && (ev->keyval == GDK_KEY_a || ev->keyval == GDK_KEY_A)) {
        e->all = 1;
        e->caret = len;
        return 1;
    }
    if (ctrl && (ev->keyval == GDK_KEY_v || ev->keyval == GDK_KEY_V)) {
        char *t = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        if (t) {
            for (char *p = t; *p; p++)
                if (*p == '\n' || *p == '\r')
                    *p = ' ';
            edit_insert(e, t);
            g_free(t);
        }
        return 1;
    }
    gunichar u = gdk_keyval_to_unicode(ev->keyval);
    if (u >= 0x20 && u != 0x7f && !ctrl) {
        char buf[8];
        buf[g_unichar_to_utf8(u, buf)] = 0;
        edit_insert(e, buf);
        return 1;
    }
    return 0;
}

/* --- the mouse and the keyboard ------------------------------------------------- */
static void ensure_visible(int i)
{
    double y, h, top = view_top() + 4, bottom = files_bottom() - 4;
    switch (vmode) {
    case V_ICONS: {
        double x;
        cell(i, &x, &y);
        y -= 6;
        h = 78;
        break;
    }
    case V_LIST: y = view_top() + 2 + i * LROW_H - scroll; h = LROW_H; break;
    case V_COLUMNS: y = BODY_Y + 4 + i * CROW_H - scroll; h = CROW_H; break;
    default: relayout(); return;            /* the Gallery's strip follows by itself */
    }
    if (y < top)
        scroll -= top - y;
    else if (y + h > bottom)
        scroll += y + h - bottom;
    relayout();
}

static void select_name(const char *name)
{
    for (int i = 0; i < nitems; i++)
        if (!strcmp(items[i].name, name)) {
            sel = i;
            ensure_visible(i);
        }
    redraw();
}

static void select_item(int i)
{
    if (i < -1 || i >= nitems)
        return;
    sel = i;
    if (i >= 0)
        ensure_visible(i);
    redraw();
}

static void addr_start(void)
{
    finish_rename(1);
    addr.on = 1;
    snprintf(addr.s, sizeof addr.s, "%s", strcmp(cwd, DISKS) ? cwd : "Disks");
    addr.caret = (int)strlen(addr.s);
    addr.all = 1;
    gtk_widget_grab_focus(da);
    redraw();
}

static void addr_go(void)
{
    addr.on = 0;
    char path[PATH_MAX];
    if (!strcasecmp(addr.s, "Disks")) {
        navigate(DISKS, 1);
        return;
    }
    if (addr.s[0] == '~') {
        const char *home = getenv("HOME");
        snprintf(path, sizeof path, "%s%s", home ? home : "", addr.s + 1);
    } else {
        snprintf(path, sizeof path, "%s", addr.s);
    }
    navigate(path, 1);
}

/* Drag a selected file (to the Recycle can, or any URI drop). The move
   itself is the dest's job — `rm` on the can — so we only advertise the
   path and reload when the drag ends. */
static char drag_uri[PATH_MAX * 3];

static void on_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *data,
                             guint info, guint time, gpointer d)
{
    (void)w;
    (void)ctx;
    (void)info;
    (void)time;
    (void)d;
    char *uris[] = { drag_uri, NULL };
    gtk_selection_data_set_uris(data, uris);
}

static void on_drag_end(GtkWidget *w, GdkDragContext *ctx, gpointer d)
{
    (void)w;
    (void)ctx;
    (void)d;
    dragging = 0;
    press.kind = T_NONE;
    reload(sel >= 0 && sel < nitems ? items[sel].name : NULL);
}

static void start_file_drag(GdkEventMotion *ev)
{
    if (sel < 0 || !strcmp(cwd, DISKS) || in_trash())
        return;
    char path[PATH_MAX];
    item_path(sel, path, sizeof path);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri)
        return;
    snprintf(drag_uri, sizeof drag_uri, "%s", uri);
    g_free(uri);
    dragging = 1;
    GtkTargetEntry te = { "text/uri-list", 0, 0 };
    GtkTargetList *tl = gtk_target_list_new(&te, 1);
    gtk_drag_begin_with_coordinates(da, tl, GDK_ACTION_MOVE, 1, (GdkEvent *)ev, -1, -1);
    gtk_target_list_unref(tl);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)d;
    double x = ev->x / K, y = ev->y / K;
    press_x = x;
    press_y = y;
    struct target t = hit(x, y);
    if (ev->type == GDK_BUTTON_RELEASE) {
        if (press.kind == T_TOOL && t.kind == T_TOOL && t.idx == press.idx && ev->button == 1)
            do_action(t.idx == 0 ? A_BACK : t.idx == 1 ? A_FORWARD : A_UP);
        else if (press.kind == T_TASK && t.kind == T_TASK && t.idx == press.idx)
            do_action(tasks[t.idx].act);
        press.kind = T_NONE;
        redraw();
        return TRUE;
    }
    /* A click anywhere closes an open menu; on its own title, that's all. */
    int was_open = pop.win ? pop_menubar : -2, was_view = pop.win && pop_view;
    if (pop.win) {
        pop_close();
        if ((t.kind == T_MENU && t.idx == was_open) || (t.kind == T_VIEW && was_view))
            return TRUE;
    }
    gtk_widget_grab_focus(w);
    if (ev->button == 8 || ev->button == 9) {          /* the mouse's own back/forward */
        if (ev->type == GDK_BUTTON_PRESS)
            go_back(ev->button == 9);
        return TRUE;
    }
    if (t.kind != T_ADDR && addr.on) {
        addr.on = 0;
        redraw();
    }
    if (ren.on && !(t.kind == T_FILE && t.idx == ren_idx))
        finish_rename(1);
    if (ev->type == GDK_2BUTTON_PRESS) {
        if (t.kind == T_FILE && ev->button == 1)
            open_item(t.idx);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS)
        return TRUE;
    press = t;
    switch (t.kind) {
    case T_MENU:
        if (ev->button == 1)
            pop_open(MENU_ITEMS[t.idx], t.idx, menu_x(t.idx) - 6, MENU_H);
        break;
    case T_VIEW:
        if (ev->button == 1) {
            pop_open(M_VIEWS, -1, VIEW_X, BODY_Y - 2);
            pop_view = 1;
        }
        break;
    case T_ADDR:
        if (!addr.on)
            addr_start();
        break;
    case T_FILE:
        select_item(t.idx);
        if (ev->button == 3)
            pop_open(M_ON_FILE, -1, x, y);
        break;
    case T_COL: {
        /* A folder to the left (or inside the selected one) becomes the
           folder shown, with what was clicked selected in it. */
        int c = t.idx / COL_IDX, i = t.idx % COL_IDX;
        const struct col *cl = c == MAXCOL ? &peek : &anc[c];
        char dir[PATH_MAX], name[NAME_MAX + 1];
        snprintf(dir, sizeof dir, "%s", cl->path);
        snprintf(name, sizeof name, "%s", cl->items[i].name);
        navigate(dir, 1);
        select_name(name);
        press.kind = T_NONE;
        if (ev->button == 3 && sel >= 0)
            pop_open(M_ON_FILE, -1, x, y);
        break;
    }
    case T_FILES:
        if (vmode != V_GALLERY)
            select_item(-1);
        if (ev->button == 3)
            pop_open(M_ON_FOLDER, -1, x, y);
        break;
    case T_THUMB:
        drag_y = y;
        drag_scroll = scroll;
        break;
    case T_TRACK: {
        double ty, th;
        scroll_thumb(&ty, &th);
        scroll += (y < ty ? -1 : 1) * (files_bottom() - BODY_Y - CELL_H / 2);
        relayout();
        break;
    }
    }
    redraw();
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
    (void)w;
    (void)d;
    double x = ev->x / K, y = ev->y / K;
    if (press.kind == T_THUMB) {
        double ty, th, len = files_bottom() - view_top() - 4;
        scroll_thumb(&ty, &th);
        if (len > th)
            scroll = drag_scroll + (y - drag_y) * max_scroll / (len - th);
        relayout();
        redraw();
        return TRUE;
    }
    if (!dragging && press.kind == T_FILE && sel >= 0 && (ev->state & GDK_BUTTON1_MASK)) {
        double dx = x - press_x, dy = y - press_y;
        if (dx * dx + dy * dy > 64) {
            start_file_drag(ev);
            return TRUE;
        }
    }
    struct target t = hit(x, y);
    if (pop.win && pop_menubar >= 0 && t.kind == T_MENU && t.idx != pop_menubar)
        pop_open(MENU_ITEMS[t.idx], t.idx, menu_x(t.idx) - 6, MENU_H);
    if (t.kind != hover.kind || t.idx != hover.idx) {
        hover = t;
        GdkWindow *gw = gtk_widget_get_window(da);
        GdkCursor *c = NULL;
        if (t.kind == T_ADDR)
            c = gdk_cursor_new_from_name(gdk_display_get_default(), "text");
        else if (t.kind == T_TASK)
            c = gdk_cursor_new_from_name(gdk_display_get_default(), "pointer");
        gdk_window_set_cursor(gw, c);
        if (c)
            g_object_unref(c);
        redraw();
    }
    return TRUE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer d)
{
    (void)w;
    (void)ev;
    (void)d;
    if (hover.kind != T_NONE && press.kind != T_THUMB) {
        hover.kind = T_NONE;
        redraw();
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
    (void)w;
    (void)d;
    int dir = ev->direction == GDK_SCROLL_UP ? -1 : ev->direction == GDK_SCROLL_DOWN ? 1 : 0;
    if (!dir)
        return TRUE;
    if (vmode == V_GALLERY) {               /* the wheel walks along the strip */
        select_item(sel + dir < 0 ? 0 : sel + dir < nitems ? sel + dir : sel);
        return TRUE;
    }
    double step = vmode == V_ICONS ? CELL_H / 2 : 3 * LROW_H;
    if (vmode == V_COLUMNS) {               /* each column scrolls by itself */
        double x = ev->x / K;
        for (int k = 0; k < col_count(); k++) {
            double cx = col_x(k);
            if (x < cx || x >= cx + COL_W || k == nanc)
                continue;
            col_scroll_by(k < nanc ? &anc[k] : &peek, dir * step);
            redraw();
            return TRUE;
        }
    }
    scroll += dir * step;
    relayout();
    redraw();
    return TRUE;
}

/* Typing the first letters of a name selects it, as in Windows. */
static char typed[32];
static gint64 typed_at;

static void type_ahead(gunichar u)
{
    gint64 now = g_get_monotonic_time();
    size_t n = strlen(typed);
    if (now - typed_at > 1000000 || n + 6 >= sizeof typed)
        n = typed[0] = 0;
    typed_at = now;
    typed[n + g_unichar_to_utf8(u, typed + n)] = 0;
    for (int i = 0; i < nitems; i++)
        if (!strncasecmp(items[i].name, typed, strlen(typed))) {
            select_item(i);
            return;
        }
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    (void)d;
    int ctrl = ev->state & GDK_CONTROL_MASK, alt = ev->state & GDK_MOD1_MASK;
    unsigned k = ev->keyval;
    if (addr.on) {
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter)
            addr_go();
        else if (k == GDK_KEY_Escape)
            addr.on = 0;
        else
            edit_key(&addr, ev);
        redraw();
        return TRUE;
    }
    if (ren.on) {
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter)
            finish_rename(1);
        else if (k == GDK_KEY_Escape)
            finish_rename(0);
        else
            edit_key(&ren, ev);
        redraw();
        return TRUE;
    }
    if (alt && k == GDK_KEY_Left) { do_action(A_BACK); return TRUE; }
    if (alt && k == GDK_KEY_Right) { do_action(A_FORWARD); return TRUE; }
    if (alt && k == GDK_KEY_Up) { do_action(A_UP); return TRUE; }
    if (ctrl) {
        switch (k) {
        case GDK_KEY_c: case GDK_KEY_C: do_action(A_COPY); return TRUE;
        case GDK_KEY_v: case GDK_KEY_V: do_action(A_PASTE); return TRUE;
        case GDK_KEY_h: case GDK_KEY_H: do_action(A_HIDDEN); return TRUE;
        case GDK_KEY_l: case GDK_KEY_L: addr_start(); return TRUE;
        case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3: case GDK_KEY_4:   /* the Mac's Cmd+1..4 */
            set_view((int)(k - GDK_KEY_1));
            return TRUE;
        case GDK_KEY_w: case GDK_KEY_W: case GDK_KEY_q: case GDK_KEY_Q: do_action(A_CLOSE); return TRUE;
        case GDK_KEY_n: case GDK_KEY_N:
            if (ev->state & GDK_SHIFT_MASK)
                do_action(A_NEWFOLDER);
            return TRUE;
        }
        return FALSE;
    }
    switch (k) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: do_action(A_OPEN); return TRUE;
    case GDK_KEY_BackSpace: do_action(A_UP); return TRUE;
    case GDK_KEY_Delete: do_action(A_DELETE); return TRUE;
    case GDK_KEY_F2: do_action(A_RENAME); return TRUE;
    case GDK_KEY_F5: do_action(A_REFRESH); return TRUE;
    case GDK_KEY_Escape: select_item(-1); return TRUE;
    case GDK_KEY_Home: if (nitems) select_item(0); return TRUE;
    case GDK_KEY_End: if (nitems) select_item(nitems - 1); return TRUE;
    case GDK_KEY_Left:
        if (vmode == V_COLUMNS) {           /* back to the column on the left */
            if (strcmp(cwd, DISKS) && strcmp(cwd, col_root))
                go_up();
        } else if (vmode != V_LIST) {
            select_item(sel > 0 ? sel - 1 : 0);
        }
        return TRUE;
    case GDK_KEY_Right:
        if (vmode == V_COLUMNS) {           /* into the selected folder's column */
            if (sel >= 0 && items[sel].dir) {
                char p[PATH_MAX];
                item_path(sel, p, sizeof p);
                navigate(p, 1);
                if (nitems)
                    select_item(0);
            }
        } else if (vmode != V_LIST) {
            select_item(sel + 1 < nitems ? sel + 1 : sel);
        }
        return TRUE;
    case GDK_KEY_Up: {
        int step = vmode == V_ICONS ? cols : 1;
        select_item(sel >= step ? sel - step : (sel < 0 ? 0 : sel));
        return TRUE;
    }
    case GDK_KEY_Down: {
        int step = vmode == V_ICONS ? cols : 1;
        select_item(sel < 0 ? 0 : sel + step < nitems ? sel + step : sel);
        return TRUE;
    }
    }
    gunichar u = gdk_keyval_to_unicode(k);
    if (u > 0x20 && u != 0x7f) {
        type_ahead(u);
        return TRUE;
    }
    return FALSE;
}

static void on_active(GObject *o, GParamSpec *p, gpointer d)
{
    (void)p;
    (void)d;
    active = gtk_window_is_active(GTK_WINDOW(o));
    redraw();
}

/*
 * Seeing changes made elsewhere (a terminal, another program): inotify
 * on the folder shown. One watch, moved when the folder changes (from
 * reload). Events wake us only when something happens, where looking
 * every 2 s woke the program for nothing; and a file written over in
 * place now shows too. A burst (rm -r, a copy of many files) is read at
 * once and becomes one reload, 100 ms after it starts. Without inotify
 * (an old kernel), the old way: the folder's time every 2 s.
 */
static int ino_fd = -1, ino_wd = -1;
static char watched[PATH_MAX];
static guint ino_later;

static void watch_cwd(void)
{
    if (ino_fd < 0 || !strcmp(watched, cwd))
        return;
    if (ino_wd >= 0)
        inotify_rm_watch(ino_fd, ino_wd);
    ino_wd = -1;
    snprintf(watched, sizeof watched, "%s", cwd);
    if (strcmp(cwd, DISKS))
        ino_wd = inotify_add_watch(ino_fd, cwd, IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                                   IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB |
                                   IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
}

static gboolean reload_later(gpointer d)
{
    (void)d;
    if (ren.on)
        return G_SOURCE_CONTINUE;       /* a name is being typed: after that */
    ino_later = 0;
    struct stat st;
    char up[PATH_MAX];
    snprintf(up, sizeof up, "%s", cwd);
    while (strcmp(up, "/") && (stat(up, &st) || !S_ISDIR(st.st_mode))) {
        char *slash = strrchr(up, '/');  /* the folder went: its nearest parent */
        if (slash == up)
            slash[1] = 0;
        else
            *slash = 0;
    }
    if (strcmp(up, cwd))
        navigate(up, 0);
    else
        reload(sel >= 0 ? items[sel].name : NULL);
    return G_SOURCE_REMOVE;
}

static gboolean on_inotify(gint fd, GIOCondition cond, gpointer d)
{
    (void)cond;
    (void)d;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int ours = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; ) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            if (e->wd == ino_wd || (e->mask & IN_Q_OVERFLOW))
                ours = 1;
            p += sizeof *e + e->len;
        }
    if (ours && !ino_later)
        ino_later = g_timeout_add(100, reload_later, NULL);
    return G_SOURCE_CONTINUE;
}

/* The fallback, with no inotify: the folder's time every 2 s (a file
   written over in place then needs F5). */
static gboolean poll_dir(gpointer d)
{
    (void)d;
    struct stat st;
    if (!strcmp(cwd, DISKS) || ren.on || stat(cwd, &st))
        return G_SOURCE_CONTINUE;
    if (st.st_mtime != dir_mtime)
        reload(sel >= 0 ? items[sel].name : NULL);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    g_set_prgname("myos-files");
    gtk_init(&argc, &argv);
    back_hist = g_ptr_array_new_with_free_func(g_free);
    fwd_hist = g_ptr_array_new_with_free_func(g_free);
    load_view();

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *mc = cairo_create(s);
    measure_ctx = pango_cairo_create_context(mc);
    font_options(measure_ctx);

    Display *dpy = gdk_x11_get_default_xdisplay();
    a_scale = XInternAtom(dpy, "_MYOS_SCALE", False);
    a_scale_auto = XInternAtom(dpy, "_MYOS_SCALE_AUTO", False);
    GdkWindow *root = gdk_get_default_root_window();
    gdk_window_set_events(root, gdk_window_get_events(root) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(root, root_filter, NULL);
    desk_scale = read_scale();

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(win), "folder");
    da = gtk_drawing_area_new();
    gtk_widget_set_can_focus(da, TRUE);
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                          GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK |
                          GDK_FOCUS_CHANGE_MASK);
    gtk_container_add(GTK_CONTAINER(win), da);
    gtk_widget_realize(da);
    update_k();
    gtk_window_set_default_size(GTK_WINDOW(win), (int)(DEF_W * K), (int)(DEF_H * K));
    gtk_widget_set_size_request(da, (int)(400 * K), (int)(260 * K));
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(da, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    g_signal_connect(da, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(da, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(da, "button-release-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(da, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(da, "leave-notify-event", G_CALLBACK(on_leave), NULL);
    g_signal_connect(da, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(da, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0)
        g_unix_fd_add(ino_fd, G_IO_IN, on_inotify, NULL);
    else
        g_timeout_add_seconds(2, poll_dir, NULL);

    const char *start = argc > 1 ? argv[1] : getenv("HOME");
    snprintf(cwd, sizeof cwd, "/");
    navigate(start && start[0] ? start : "/", 0);
    if (!strcmp(cwd, "/") && start && strcmp(start, "/"))
        navigate("/", 0);

    gtk_widget_show_all(win);
    gtk_widget_grab_focus(da);
    gtk_main();
    return 0;
}
