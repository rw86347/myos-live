/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-desktop — the files in ~/Desktop, drawn on the wallpaper.
 *
 * One small window per file, the same kind as the Recycle can
 * (myos-trash): desktop type, so IceWM keeps it under every other
 * window, with the wallpaper copied in behind the icon (no compositor,
 * so the window itself can't be see-through). The column is the
 * mockup's: 48 px icons, 82 px apart, starting at the left. The first
 * place is the Recycle Bin's (myos-trash), and is left empty.
 *
 * A click selects: the name gets a blue highlight, as on the Mac (not the
 * whole cell), until another icon is clicked. A
 * double-click opens the file the way My Files does. An icon can be
 * dragged onto the Recycle Bin, or anywhere else that takes files.
 * Right-click on the wallpaper (the root window) offers New Folder and
 * Change Desktop Background…, which is the Mac's item: it opens Settings
 * on the Wallpaper page. This program paints the picture too, because it
 * stays running and the root pixmap has to outlive Settings.
 *
 * If ~/Desktop is missing, it is created. Started from /etc/icewm/startup.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <cairo-xlib.h>
#include <librsvg/rsvg.h>
#include <poppler.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "scale.h"
#include "wallpaper.h"

#define ICONS "/usr/share/myos/icons/"

/* The same grid myos-trash uses. Slot 0 is the Recycle Bin. */
#define COL_X 26
#define COL_Y0 18
#define STEP 82
#define ICON_S 48
#define CELL_W 96
#define CELL_H 82
#define TRASH_SLOT 0

static const char *const TEXT_EXT[] = {"txt", "md", "c", "h", "cc", "cpp", "hpp", "py", "sh", "s", "asm", "conf",
                                       "cfg", "ini", "log", "csv", "json", "xml", "css", "js", "mk", NULL};
static const char *const VIEW_EXT[] = {"pdf", "png", "jpg", "jpeg", "jpe", "gif", "bmp", "tif", "tiff", "heic",
                                       "heif", "webp", "svg", "ico", "avif", NULL};
static const char *const DOC_EXT[] = {"doc", "docx", "rtf", "odt", "abw", NULL};
static const char *const WP_EXT[] = {"wpd", "wp", "wp5", "wp6", NULL};
static const char *const WEB_EXT[] = {"html", "htm", "xhtml", NULL};
static const char *const SKIP_THUMB[] = {"pdf", "svg", NULL};   /* not gdk-pixbuf's: see thumb_of */

struct icon {
    char *name, *path;
    int dir;
    int x, y;
    GdkPixbuf *thumb;
    const char *svg;
    GtkWidget *win, *da;
};

static struct icon *items;
static int nitems, selected = -1;
static char desktop[PATH_MAX];
static GdkPixbuf *wall_pb;
static Pixmap wall_pm;
static int screen_w, screen_h, scale_pct;
static double K;
static Atom a_scale, a_auto, a_wall;
static int menu_x, menu_y;

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

static long root_cardinal(Display *d, Atom a, long dflt)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long v = dflt;
    if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success &&
        data && n == 1 && fmt == 32)
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
    a_wall = XInternAtom(d, "_MYOS_WALLPAPER", False);
    screen_w = DisplayWidth(d, DefaultScreen(d));
    screen_h = DisplayHeight(d, DefaultScreen(d));
    int auto_pct = (int)root_cardinal(d, a_auto, auto_scale(screen_w));
    long v = root_cardinal(d, a_scale, -1);
    int chosen = v < 0 ? scale_read_conf() : v ? clamp_scale((int)v) : 0;
    scale_pct = chosen ? chosen : auto_pct;
    /* Same factor myos-trash uses, so the column lines up with the can. */
    K = scale_pct / 100.0;
}

/* --- wallpaper ----------------------------------------------------------------------- */

/* IceWM's background helper (icewmbg) takes the wallpaper from IceWM's
   preferences. IceWM uses the FIRST preferences file it finds, ~/.icewm
   before /etc/icewm, not a merge of them. So the user's file has to be the
   whole system file (/etc/icewm/preferences) with our three wallpaper
   lines in place of its own. It is rebuilt every time (at each login), so
   a change to the system file reaches every user. A user file holding only
   the wallpaper lines once hid every MyOS setting: IceWM's defaults came
   back, workspace buttons on the taskbar among them (2026-10-09). */
#define ICEWM_SYSTEM_PREFS "/etc/icewm/preferences"

static int wallpaper_key(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    return !strncmp(line, "DesktopBackgroundImage", 22) ||
           !strncmp(line, "DesktopBackgroundScaled", 23) ||
           !strncmp(line, "DesktopBackgroundCenter", 23);
}

static void write_icewm(const char *png)
{
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.icewm", wall_home());
    wall_mkdir(dir);
    snprintf(path, sizeof path, "%s/preferences", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(ICEWM_SYSTEM_PREFS, "r");
    if (!in)
        return;                         /* without the system file, write nothing:
                                           a partial one would hide its settings */
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return;
    }
    fprintf(out, "# Made by myos-desktop from " ICEWM_SYSTEM_PREFS " at every login:\n"
                 "# edit that file, not this one.\n");
    char line[PATH_MAX];
    while (fgets(line, sizeof line, in))
        if (!wallpaper_key(line))
            fputs(line, out);
    fclose(in);
    fprintf(out, "DesktopBackgroundImage=\"%s\"\nDesktopBackgroundScaled=0\nDesktopBackgroundCenter=0\n", png);
    if (!fclose(out))
        rename(tmp, path);
}

static void install_pixmap(GdkPixbuf *pb)
{
    Display *dpy = gdk_x11_get_default_xdisplay();
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    Pixmap pm = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, scr));
    cairo_surface_t *s = cairo_xlib_surface_create(dpy, pm, DefaultVisual(dpy, scr), w, h);
    cairo_t *cr = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    XSetWindowBackgroundPixmap(dpy, root, pm);
    XClearWindow(dpy, root);
    Atom id = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    Atom setr = XInternAtom(dpy, "_XSETROOT_ID", False);
    XChangeProperty(dpy, root, id, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pm, 1);
    XChangeProperty(dpy, root, setr, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pm, 1);
    XFlush(dpy);
    if (wall_pm)
        XFreePixmap(dpy, wall_pm);
    wall_pm = pm;
}

/* The Recycle can copies the root pixmap when it draws. Clearing it
   makes it draw again, so a new picture shows through the can too. */
static void redraw_named(Window w, const char *name, int levels)
{
    Display *dpy = gdk_x11_get_default_xdisplay();
    char *n = NULL;
    if (XFetchName(dpy, w, &n) && n) {
        if (!strcmp(n, name))
            XClearWindow(dpy, w);
        XFree(n);
    }
    if (levels <= 0)
        return;
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &nk))
        return;
    for (unsigned i = 0; i < nk; i++)
        redraw_named(kids[i], name, levels - 1);
    if (kids)
        XFree(kids);
}

static void apply_wall(void)
{
    struct wall c;
    wall_read(&c);
    GdkPixbuf *pb = wall_render(screen_w, screen_h, &c);
    if (!pb)
        return;
    install_pixmap(pb);
    char png[PATH_MAX];
    snprintf(png, sizeof png, "%s/.cache/myos", wall_home());
    wall_mkdir(png);
    snprintf(png, sizeof png, "%s/.cache/myos/wallpaper.png", wall_home());
    gdk_pixbuf_save(pb, png, "png", NULL, NULL);
    write_icewm(png);
    if (wall_pb)
        g_object_unref(wall_pb);
    wall_pb = pb;
    for (int i = 0; i < nitems; i++)
        if (items[i].da)
            gtk_widget_queue_draw(items[i].da);
    redraw_named(DefaultRootWindow(gdk_x11_get_default_xdisplay()), "Recycle Bin", 6);
}

/* --- files --------------------------------------------------------------------------- */

static const char *svg_for(const char *name, int dir, const struct stat *st)
{
    if (dir)
        return "folder";
    if (has_ext(name, VIEW_EXT))
        return "preview";
    if (has_ext(name, WP_EXT))
        return "writeperfect";
    if (has_ext(name, DOC_EXT))
        return "text-editor";
    if (has_ext(name, WEB_EXT))
        return "web-browser";
    if (has_ext(name, TEXT_EXT) || (S_ISREG(st->st_mode) && st->st_size < 256 * 1024 && !strchr(name, '.')))
        return "text-file";
    return "binary-file";
}

static int icon_cmp(const void *a, const void *b)
{
    const struct icon *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static void clear_items(void)
{
    for (int i = 0; i < nitems; i++) {
        g_free(items[i].name);
        g_free(items[i].path);
        if (items[i].thumb)
            g_object_unref(items[i].thumb);
        if (items[i].win)
            gtk_widget_destroy(items[i].win);
    }
    g_free(items);
    items = NULL;
    nitems = 0;
    selected = -1;
}

static GdkPixbuf *thumb_of(const char *name, const char *path);

static void load_items(void)
{
    /* The folder is part of the desktop. Put it back if it was never there
       or the user removed it. */
    wall_mkdir(desktop);
    char *keep = selected >= 0 && selected < nitems ? g_strdup(items[selected].name) : NULL;
    clear_items();
    DIR *d = opendir(desktop);
    if (!d) {
        g_free(keep);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", desktop, e->d_name);
        if (stat(path, &st))
            continue;
        items = g_renew(struct icon, items, nitems + 1);
        int dir = S_ISDIR(st.st_mode);
        items[nitems++] = (struct icon){
            .name = g_strdup(e->d_name),
            .path = g_strdup(path),
            .dir = dir,
            .x = -1,
            .thumb = dir ? NULL : thumb_of(e->d_name, path),
            .svg = svg_for(e->d_name, dir, &st)};
    }
    closedir(d);
    qsort(items, (size_t)nitems, sizeof *items, icon_cmp);
    if (keep) {
        for (int i = 0; i < nitems; i++)
            if (!strcmp(items[i].name, keep))
                selected = i;
        g_free(keep);
    }
}

/* A page as a picture: white, with a thin grey edge so a white page shows
   on a light wallpaper (the Mac draws a document's thumbnail so). */
static GdkPixbuf *page_frame(cairo_surface_t *page, int w, int h)
{
    cairo_t *cr = cairo_create(page);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return gdk_pixbuf_get_from_surface(page, 0, 0, w, h);
}

/* Page 1 of a PDF, fitted into px x px (as My Files and Preview draw it). */
static GdkPixbuf *thumb_from_pdf(const char *path, int px)
{
    char *uri = g_filename_to_uri(path, NULL, NULL);
    PopplerDocument *doc = uri ? poppler_document_new_from_file(uri, NULL, NULL) : NULL;
    g_free(uri);
    PopplerPage *page = doc ? poppler_document_get_page(doc, 0) : NULL;
    GdkPixbuf *pb = NULL;
    double pw = 0, ph = 0;
    if (page)
        poppler_page_get_size(page, &pw, &ph);
    if (pw >= 1 && ph >= 1) {
        double sc = fmin(px / pw, px / ph);
        int w = (int)ceil(pw * sc), h = (int)ceil(ph * sc);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, sc, sc);
        poppler_page_render(page, cr);
        cairo_destroy(cr);
        pb = page_frame(surf, w, h);
        cairo_surface_destroy(surf);
    }
    if (page)
        g_object_unref(page);
    if (doc)
        g_object_unref(doc);
    return pb;
}

/* An SVG drawn at px x px (gdk-pixbuf has no SVG loader here). */
static GdkPixbuf *thumb_from_svg(const char *path, int px)
{
    RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
    if (!h)
        return NULL;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    RsvgRectangle vp = { 0, 0, px, px };
    rsvg_handle_render_document(h, cr, &vp, NULL);
    cairo_destroy(cr);
    g_object_unref(h);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, px, px);
    cairo_surface_destroy(surf);
    return pb;
}

/* The picture an icon shows instead of its kind's, or NULL: a picture
   file itself, a PDF's first page, an SVG drawn. */
static GdkPixbuf *thumb_of(const char *name, const char *path)
{
    int px = (int)lround(ICON_S * K);
    if (has_ext(name, (const char *const[]){"pdf", NULL}))
        return thumb_from_pdf(path, px);
    if (has_ext(name, (const char *const[]){"svg", NULL}))
        return thumb_from_svg(path, px);
    if (has_ext(name, VIEW_EXT) && !has_ext(name, SKIP_THUMB))
        return gdk_pixbuf_new_from_file_at_size(path, px, px, NULL);
    return NULL;
}

/* Slot 0 belongs to the Recycle Bin. Files go in the places after it,
   down the column and then into the next one, stopping above the taskbar. */
static void place_of(int index, int *x, int *y)
{
    int slot = index >= TRASH_SLOT ? index + 1 : index;
    int cell_h = (int)lround(STEP * K);
    int y0 = (int)lround(COL_Y0 * K);
    int bar = (int)lround(40 * K);
    int rows = (screen_h - bar - y0) / (cell_h > 0 ? cell_h : 1);
    if (rows < 1)
        rows = 1;
    int col = slot / rows, row = slot % rows;
    *x = (int)lround(COL_X * K - (CELL_W * K - ICON_S * K) / 2) + col * (int)lround(CELL_W * K);
    *y = y0 + row * cell_h;
}

static cairo_surface_t *svg_surface(const char *name, int size)
{
    static struct {
        char name[32];
        int size;
        cairo_surface_t *s;
    } cache[16];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].size == size && !strcmp(cache[i].name, name))
            return cache[i].s;
    if (n == (int)(sizeof cache / sizeof cache[0])) {
        cairo_surface_destroy(cache[0].s);
        memmove(cache, cache + 1, sizeof cache - sizeof cache[0]);
        n--;
    }
    char path[256];
    snprintf(path, sizeof path, ICONS "%s.svg", name);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
    if (h) {
        cairo_t *c = cairo_create(s);
        RsvgRectangle vp = {0, 0, size, size};
        rsvg_handle_render_document(h, c, &vp, NULL);
        cairo_destroy(c);
        g_object_unref(h);
    }
    snprintf(cache[n].name, sizeof cache[0].name, "%s", name);
    cache[n].size = size;
    cache[n].s = s;
    return cache[n++].s;
}

static void open_icon(struct icon *it)
{
    const char *prog = NULL;
    if (it->dir)
        prog = "myos-files";
    else if (has_ext(it->name, VIEW_EXT))
        prog = "myos-preview";
    else if (has_ext(it->name, WP_EXT))
        prog = "writeperfect";
    else if (has_ext(it->name, DOC_EXT))
        prog = "myos-office";     /* LibreOffice, or AbiWord without it */
    else if (has_ext(it->name, WEB_EXT))
        prog = "myos-browser";      /* Chromium or Firefox, whichever the disk has */
    else if (!strcmp(it->svg, "text-file"))
        prog = "geany";
    char *argv[3];
    if (prog) {
        argv[0] = (char *)prog;
        argv[1] = it->path;
        argv[2] = NULL;
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }
    struct stat st;
    if (!stat(it->path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
        argv[0] = it->path;
        argv[1] = NULL;
        g_spawn_async(NULL, argv, NULL, 0, NULL, NULL, NULL, NULL);
    }
}

static void new_folder(void)
{
    char path[PATH_MAX];
    for (int n = 1; n < 1000; n++) {
        if (n == 1)
            snprintf(path, sizeof path, "%s/New Folder", desktop);
        else
            snprintf(path, sizeof path, "%s/New Folder %d", desktop, n);
        if (!mkdir(path, 0755) || errno != EEXIST)
            break;
    }
}

static void open_wallpaper_settings(void)
{
    char *argv[] = {"myos-settings", "wallpaper", NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void menu_open(GtkMenuItem *m, gpointer u)
{
    (void)u;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(m), "item"));
    if (i >= 0 && i < nitems)
        open_icon(&items[i]);
}

static void menu_folder(GtkMenuItem *m, gpointer u)
{
    (void)m, (void)u;
    new_folder();
}

static void menu_background(GtkMenuItem *m, gpointer u)
{
    (void)m, (void)u;
    open_wallpaper_settings();
}

static void menu_pos(GtkMenu *menu, gint *x, gint *y, gboolean *push, gpointer data)
{
    (void)menu, (void)data;
    *x = menu_x;
    *y = menu_y;
    *push = TRUE;
}

static void popup_menu(int item, guint button, guint32 time, int at_pointer)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *first;
    if (item >= 0) {
        first = gtk_menu_item_new_with_label("Open");
        g_object_set_data(G_OBJECT(first), "item", GINT_TO_POINTER(item));
        g_signal_connect(first, "activate", G_CALLBACK(menu_open), NULL);
    } else {
        first = gtk_menu_item_new_with_label("New Folder");
        g_signal_connect(first, "activate", G_CALLBACK(menu_folder), NULL);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), first);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *bg = gtk_menu_item_new_with_label("Change Desktop Background\u2026");
    g_signal_connect(bg, "activate", G_CALLBACK(menu_background), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), bg);
    gtk_widget_show_all(menu);
    if (at_pointer)
        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, button, time);
    else
        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, menu_pos, NULL, button, time);
}

static int index_of(GtkWidget *w)
{
    for (int i = 0; i < nitems; i++)
        if (items[i].da == w || items[i].win == w)
            return i;
    return -1;
}

static gboolean draw_icon(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    int i = index_of(w);
    if (i < 0)
        return TRUE;
    struct icon *it = &items[i];
    cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    cairo_clip(cr);
    int W = gtk_widget_get_allocated_width(w);
    if (wall_pb) {
        int wx = 0, wy = 0;
        gtk_window_get_position(GTK_WINDOW(it->win), &wx, &wy);
        gdk_cairo_set_source_pixbuf(cr, wall_pb, -wx, -wy);
        cairo_paint(cr);
    }
    int isz = (int)lround(ICON_S * K);
    if (it->thumb) {
        int tw = gdk_pixbuf_get_width(it->thumb), th = gdk_pixbuf_get_height(it->thumb);
        gdk_cairo_set_source_pixbuf(cr, it->thumb, (W - tw) / 2.0, 4 * K + (isz - th) / 2.0);
        cairo_paint(cr);
    } else {
        cairo_set_source_surface(cr, svg_surface(it->svg, isz), (W - isz) / 2.0, 4 * K);
        cairo_paint(cr);
    }
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12 * K);
    cairo_text_extents_t te;
    cairo_text_extents(cr, it->name, &te);
    double tx = (W - te.width) / 2 - te.x_bearing;
    if (tx < 2)
        tx = 2;
    double ty = 4 * K + isz + 14 * K;
    if (i == selected) {
        /* The Mac's selection: a blue rounded band behind the name only. */
        double pad = 3 * K, r = 4 * K;
        double bx = tx + te.x_bearing - pad, by = ty + te.y_bearing - pad;
        double bw = te.width + 2 * pad, bh = te.height + 2 * pad;
        if (bx < 1)
            bx = 1;
        if (bx + bw > W - 1)
            bw = W - 1 - bx;
        cairo_new_sub_path(cr);
        cairo_arc(cr, bx + bw - r, by + r, r, -M_PI / 2, 0);
        cairo_arc(cr, bx + bw - r, by + bh - r, r, 0, M_PI / 2);
        cairo_arc(cr, bx + r, by + bh - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, bx + r, by + r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.16, 0.38, 0.85);
        cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_move_to(cr, tx + 1, ty + 1);
    cairo_show_text(cr, it->name);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, it->name);
    return TRUE;
}

static gboolean icon_button(GtkWidget *w, GdkEventButton *e, gpointer unused)
{
    (void)unused;
    int i = index_of(w);
    if (i < 0)
        return FALSE;
    if (e->button == 3) {
        selected = i;
        gtk_widget_queue_draw(w);
        menu_x = (int)e->x_root;
        menu_y = (int)e->y_root;
        popup_menu(i, e->button, e->time, 1);
        return TRUE;
    }
    if (e->button != 1)
        return FALSE;
    selected = i;
    if (e->type == GDK_2BUTTON_PRESS)
        open_icon(&items[i]);
    for (int j = 0; j < nitems; j++)
        if (items[j].da)
            gtk_widget_queue_draw(items[j].da);
    return TRUE;
}

/* Dragging an icon: it carries its file's URI, which the Recycle Bin
   (myos-trash) and My Files take. */
static void drag_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *data, guint info,
                     guint time, gpointer unused)
{
    (void)ctx, (void)info, (void)time, (void)unused;
    int i = index_of(w);
    if (i < 0)
        return;
    gchar *uri = g_filename_to_uri(items[i].path, NULL, NULL);
    gchar *uris[] = { uri, NULL };
    if (uri)
        gtk_selection_data_set_uris(data, uris);
    g_free(uri);
}

static void drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer unused)
{
    (void)unused;
    int i = index_of(w);
    if (i < 0)
        return;
    int isz = (int)lround(ICON_S * K);
    if (items[i].thumb)
        gtk_drag_set_icon_pixbuf(ctx, items[i].thumb, gdk_pixbuf_get_width(items[i].thumb) / 2,
                                 gdk_pixbuf_get_height(items[i].thumb) / 2);
    else {
        /* A copy: the cached surface must keep its own (zero) offset. */
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, isz, isz);
        cairo_t *c = cairo_create(sf);
        cairo_set_source_surface(c, svg_surface(items[i].svg, isz), 0, 0);
        cairo_paint(c);
        cairo_destroy(c);
        cairo_surface_set_device_offset(sf, -isz / 2.0, -isz / 2.0);
        gtk_drag_set_icon_surface(ctx, sf);
        cairo_surface_destroy(sf);
    }
}

static void desk_type(GtkWidget *w)
{
    GdkWindow *gw = gtk_widget_get_window(w);
    if (!gw)
        return;
    Display *dpy = gdk_x11_get_default_xdisplay();
    Window xw = gdk_x11_window_get_xid(gw);
    Atom wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desk = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, xw, wtype, XA_ATOM, 32, PropModeReplace, (unsigned char *)&desk, 1);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom skip[2] = {XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False)};
    XChangeProperty(dpy, xw, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)skip, 2);
}

static void build_windows(void)
{
    int cell_w = (int)lround(CELL_W * K), cell_h = (int)lround(CELL_H * K);
    int limit_x = screen_w - cell_w;
    for (int i = 0; i < nitems; i++) {
        int x, y;
        place_of(i, &x, &y);
        int bar = (int)lround(40 * K), corner = (int)lround(48 * K);
        /* Keep the taskbar and the hot corner clear. */
        if (x > limit_x || y + cell_h > screen_h - bar || (x + cell_w > screen_w - corner && y < corner)) {
            items[i].x = -1;
            continue;
        }
        items[i].x = x;
        items[i].y = y;
        GtkWidget *w = items[i].win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(w), items[i].name);
        gtk_window_set_wmclass(GTK_WINDOW(w), "myos-desktop", "Myos-desktop");
        gtk_window_set_decorated(GTK_WINDOW(w), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
        gtk_window_set_accept_focus(GTK_WINDOW(w), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(w), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(w), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(w), GDK_WINDOW_TYPE_HINT_DESKTOP);
        gtk_widget_set_app_paintable(w, TRUE);
        GtkWidget *da = items[i].da = gtk_drawing_area_new();
        /* Exactly one cell. Without a size of its own the window took GTK's
           default, 200 x 200 — taller than the 82 px step — so a clicked
           icon, raised, covered the top of the next one: its picture
           vanished (the name below stayed) and clicks went to the wrong
           file. */
        gtk_widget_set_size_request(da, cell_w, cell_h);
        gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
        gtk_container_add(GTK_CONTAINER(w), da);
        /* Before our click handler: the drag source watches the press too. */
        static GtkTargetEntry uri_target = { "text/uri-list", 0, 0 };
        gtk_drag_source_set(da, GDK_BUTTON1_MASK, &uri_target, 1, GDK_ACTION_MOVE | GDK_ACTION_COPY);
        g_signal_connect(da, "drag-data-get", G_CALLBACK(drag_get), NULL);
        g_signal_connect(da, "drag-begin", G_CALLBACK(drag_begin), NULL);
        g_signal_connect(da, "draw", G_CALLBACK(draw_icon), NULL);
        g_signal_connect(da, "button-press-event", G_CALLBACK(icon_button), NULL);
        gtk_window_move(GTK_WINDOW(w), x, y);
        gtk_window_resize(GTK_WINDOW(w), cell_w, cell_h);
        gtk_widget_show_all(w);
        desk_type(w);
    }
}

static GFileMonitor *desk_mon;

static void dir_notify(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer u);

static void watch_desktop(void)
{
    /* Deleting the folder cancels the watch that was on it. */
    if (desk_mon && !g_file_monitor_is_cancelled(desk_mon))
        return;
    if (desk_mon)
        g_object_unref(desk_mon);
    GFile *df = g_file_new_for_path(desktop);
    desk_mon = g_file_monitor_directory(df, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(df);
    if (desk_mon)
        g_signal_connect(desk_mon, "changed", G_CALLBACK(dir_notify), NULL);
}

/* Whether ~/Desktop still holds exactly the names shown (in any order). */
static int same_names(void)
{
    DIR *d = opendir(desktop);
    if (!d)
        return 0;
    int n = 0, same = 1;
    struct dirent *e;
    while (same && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        n++;
        int found = 0;
        for (int i = 0; i < nitems && !found; i++)
            found = !strcmp(items[i].name, e->d_name);
        same = found;
    }
    closedir(d);
    return same && n == nitems;
}

/* Rebuilding destroys every icon's window, so it happens only when a file
   came or went: a program writing the file that is on the desktop (an
   editor saving it again and again) must not pull the icon out from under
   a click or a drag — it did, and the icon vanished. */
static void rebuild(void)
{
    load_items();
    build_windows();
    watch_desktop();
}

static gboolean reload(gpointer unused)
{
    (void)unused;
    if (same_names())
        watch_desktop();
    else
        rebuild();
    return G_SOURCE_REMOVE;
}

static void dir_notify(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer u)
{
    (void)m, (void)f, (void)other, (void)u;
    if (ev == G_FILE_MONITOR_EVENT_CHANGED || ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;                     /* contents or times: the icons stay as they are */
    static guint pending;
    if (pending)
        g_source_remove(pending);
    pending = g_timeout_add(200, reload, NULL);
}

static GdkFilterReturn root_filter(GdkXEvent *xev, GdkEvent *event, gpointer data)
{
    (void)event, (void)data;
    XEvent *e = (XEvent *)xev;
    /* Every right-click on the screen comes here first (the grab below is
       on the root window, and a grab there wins over any window's). Only
       one on the bare wallpaper is ours. Any other — on the taskbar, on a
       program, on one of our icons — is replayed: it goes on to that
       window as if there had been no grab at all. */
    if (e->type == ButtonPress && e->xbutton.button == 3) {
        if (e->xbutton.subwindow != None) {
            XAllowEvents(e->xbutton.display, ReplayPointer, e->xbutton.time);
            return GDK_FILTER_REMOVE;
        }
        XAllowEvents(e->xbutton.display, AsyncPointer, e->xbutton.time);
        menu_x = e->xbutton.x_root;
        menu_y = e->xbutton.y_root;
        popup_menu(-1, 3, e->xbutton.time, 0);
        return GDK_FILTER_REMOVE;
    }
    if (e->type != PropertyNotify)
        return GDK_FILTER_CONTINUE;
    if (e->xproperty.atom == a_wall)
        apply_wall();
    else if (e->xproperty.atom == a_scale || e->xproperty.atom == a_auto) {
        read_scale();
        rebuild();                      /* every icon changes size */
    }
    return GDK_FILTER_CONTINUE;
}

int main(int argc, char **argv)
{
    (void)argv;
    g_set_prgname("myos-desktop");
    gtk_init(&argc, &argv);
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default()))
        return 1;
    read_scale();
    snprintf(desktop, sizeof desktop, "%s/Desktop", wall_home());
    wall_mkdir(desktop);

    apply_wall();
    load_items();
    build_windows();

    watch_desktop();

    Display *dpy = gdk_x11_get_default_xdisplay();
    Window root = DefaultRootWindow(dpy);
    /* The wallpaper's right-click. IceWM listens for root clicks itself, so
       only a grab gets them to us. Synchronous: the pointer waits for
       root_filter to say whose click it is (ReplayPointer hands it on), so
       right-clicks on the taskbar and in programs still reach them — an
       asynchronous grab here took every right-click on the screen.
       owner_events False: all of them are reported here, on the root. */
    XGrabButton(dpy, Button3, AnyModifier, root, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    GdkWindow *groot = gdk_get_default_root_window();
    gdk_window_set_events(groot, gdk_window_get_events(groot) | GDK_PROPERTY_CHANGE_MASK | GDK_BUTTON_PRESS_MASK);
    gdk_window_add_filter(groot, root_filter, NULL);

    gtk_main();
    return 0;
}
