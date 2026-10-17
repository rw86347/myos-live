/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-trash — the Recycle Bin on the desktop: the first place in the
 * left column, a wire wastebasket (empty, or with paper in it while
 * ~/Trash holds something: wastebasket-{empty,full}.png, from Rodger's
 * pictures by live/tools/make-wastebasket-icons.py) and a white label.
 *
 * It is a desktop-type window (IceWM keeps those under every other
 * window), so it sits on the wallpaper. Drops land here: any file URI
 * is handed to `rm`, which moves it to ~/Trash. Right-click Empty Trash
 * runs `rm --emptyTrash`, which is the real delete (`rm --reallyDelete`
 * of what is in the can). Double-click opens ~/Trash in My Files.
 *
 * There is no compositor, so the window can't be see-through: it paints
 * the wallpaper behind itself, copied from the root window's picture
 * (_XROOTPMAP_ID, which myos-desktop sets). It watches that property and
 * paints again whenever the picture appears or changes — at login the
 * can may start before the wallpaper is up, and then it showed a plain
 * box until something else made it redraw.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <librsvg/rsvg.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "scale.h"

#define ICONS "/usr/share/myos/icons/"
#define ICON_X  26          /* mockup: left column */
#define ICON_Y  18          /* the first place (myos-desktop's TRASH_SLOT 0) */
#define ICON_S  48
#define CELL_W  96
#define CELL_H  82

static double K = 1.0;
static GtkWidget *win, *da;
static int full;                    /* something is in the Trash */

static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    return h && h[0] ? h : NULL;
}

static void trash_path(char *out, size_t n)
{
    snprintf(out, n, "%s/Trash", home_dir() ? home_dir() : "");
}

static int trash_count(void)
{
    char dir[PATH_MAX];
    trash_path(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && strncmp(e->d_name, "._", 2))
            n++;
    closedir(d);
    return n;
}

static void spawn_rm(char **argv)
{
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, NULL, &e)) {
        fprintf(stderr, "myos-trash: %s\n", e->message);
        g_error_free(e);
    }
}

static void open_trash(void)
{
    char dir[PATH_MAX];
    trash_path(dir, sizeof dir);
    g_mkdir_with_parents(dir, 0700);
    char *argv[] = { "myos-files", dir, NULL };
    spawn_rm(argv);
}

static void empty_trash(void)
{
    if (!full)
        return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Empty the Recycle Bin?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "Items in it will be deleted for good.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL,
                           "Empty Trash", GTK_RESPONSE_ACCEPT, NULL);
    int ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    if (!ok)
        return;
    char *argv[] = { "rm", "--emptyTrash", NULL };
    spawn_rm(argv);
}

static void place(void)
{
    GdkScreen *sc = gtk_widget_get_screen(win);
    int sw = gdk_screen_get_width(sc);
    int s = scale_read_conf();
    K = (s > 0 ? s : auto_scale(sw)) / 100.0;
    int x = (int)lround(ICON_X * K - (CELL_W * K - ICON_S * K) / 2);
    int y = (int)lround(ICON_Y * K);
    gtk_window_move(GTK_WINDOW(win), x < 4 ? 4 : x, y);
    /* Exactly one cell (as myos-desktop's icons): left to GTK, the window
       was 200 x 200 and covered the top of the first file below it. */
    gtk_widget_set_size_request(da, (int)lround(CELL_W * K), (int)lround(CELL_H * K));
    gtk_window_resize(GTK_WINDOW(win), (int)lround(CELL_W * K), (int)lround(CELL_H * K));
}

/* Wallpaper behind the icon, from icewmbg's root pixmap — not from the
   screen, or a window sitting over the can would be baked into it. */
static void paint_wallpaper(cairo_t *cr, int w, int h)
{
    Display *dpy = gdk_x11_get_default_xdisplay();
    Window root = DefaultRootWindow(dpy);
    Atom a = XInternAtom(dpy, "_XROOTPMAP_ID", False), type;
    int fmt, wx = 0, wy = 0;
    unsigned long n, after;
    unsigned char *data = NULL;
    gtk_window_get_position(GTK_WINDOW(win), &wx, &wy);
    Pixmap pm = None;
    if (XGetWindowProperty(dpy, root, a, 0, 1, False, AnyPropertyType, &type, &fmt,
                           &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32)
            pm = *(Pixmap *)data;
        XFree(data);
    }
    if (pm == None)
        return;
    XImage *im = XGetImage(dpy, pm, wx, wy, w, h, AllPlanes, ZPixmap);
    if (!im)
        return;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < w; x++) {
            unsigned long p = XGetPixel(im, x, y);
            row[x] = 0xff000000u | (uint32_t)p;
        }
    }
    cairo_surface_mark_dirty(s);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(s);
    XDestroyImage(im);
}

/* The wastebasket at `px` pixels, empty or full; cached per state. NULL
   if the picture is missing (then the old recycle.svg is drawn). */
static GdkPixbuf *basket(int px, int is_full)
{
    static GdkPixbuf *pb[2];
    static int pb_px[2];
    if (pb[is_full] && pb_px[is_full] == px)
        return pb[is_full];
    if (pb[is_full])
        g_object_unref(pb[is_full]);
    pb[is_full] = gdk_pixbuf_new_from_file_at_scale(
        is_full ? ICONS "wastebasket-full.png" : ICONS "wastebasket-empty.png", px, px, TRUE, NULL);
    pb_px[is_full] = px;
    return pb[is_full];
}

static void draw_icon(cairo_t *cr, double x, double y, double size)
{
    int px = (int)lround(size);
    if (px < 1)
        return;
    GdkPixbuf *pb = basket(px, full);
    if (pb) {
        gdk_cairo_set_source_pixbuf(cr, pb, x + (px - gdk_pixbuf_get_width(pb)) / 2.0,
                                    y + (px - gdk_pixbuf_get_height(pb)) / 2.0);
        cairo_paint(cr);
        return;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    RsvgHandle *h = rsvg_handle_new_from_file(ICONS "recycle.svg", NULL);
    if (h) {
        cairo_t *c = cairo_create(s);
        RsvgRectangle vp = { 0, 0, px, px };
        rsvg_handle_render_document(h, c, &vp, NULL);
        cairo_destroy(c);
        g_object_unref(h);
    }
    cairo_set_source_surface(cr, s, x, y);
    cairo_paint(cr);
    cairo_surface_destroy(s);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    paint_wallpaper(cr, W, H);
    double s = ICON_S * K, ix = (W - s) / 2, iy = 4 * K;
    draw_icon(cr, ix, iy, s);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12 * K);
    cairo_text_extents_t te;
    const char *label = "Recycle Bin";
    cairo_text_extents(cr, label, &te);
    double tx = (W - te.width) / 2 - te.x_bearing, ty = iy + s + 14 * K;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_move_to(cr, tx + 1, ty + 1);
    cairo_show_text(cr, label);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, label);
    return FALSE;
}

static void trash_uri(const char *uri)
{
    GError *e = NULL;
    char *path = g_filename_from_uri(uri, NULL, &e);
    if (!path) {
        if (e)
            g_error_free(e);
        return;
    }
    char *argv[] = { "rm", "-r", "--", path, NULL };
    gint st = 0;
    GError *se = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, &se) ||
        !g_spawn_check_exit_status(st, NULL)) {
        fprintf(stderr, "myos-trash: can't trash %s\n", path);
        if (se)
            g_error_free(se);
    }
    g_free(path);
}

static void on_drag(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                    GtkSelectionData *data, guint info, guint time, gpointer d)
{
    (void)w;
    (void)x;
    (void)y;
    (void)info;
    (void)d;
    gchar **uris = gtk_selection_data_get_uris(data);
    int ok = 0;
    if (uris) {
        for (int i = 0; uris[i]; i++)
            trash_uri(uris[i]);
        ok = 1;
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
    full = trash_count() > 0;
    gtk_widget_queue_draw(da);
}

static void popup_empty(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    empty_trash();
}

static void popup_open(GtkMenuItem *m, gpointer d)
{
    (void)m;
    (void)d;
    open_trash();
}

static void show_menu(GdkEventButton *ev)
{
    GtkWidget *m = gtk_menu_new();
    GtkWidget *o = gtk_menu_item_new_with_label("Open");
    GtkWidget *e = gtk_menu_item_new_with_label("Empty Trash");
    gtk_widget_set_sensitive(e, full);
    g_signal_connect(o, "activate", G_CALLBACK(popup_open), NULL);
    g_signal_connect(e, "activate", G_CALLBACK(popup_empty), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), o);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), e);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w;
    (void)d;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        open_trash();
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        show_menu(ev);
        return TRUE;
    }
    return FALSE;
}

static gboolean poll_full(gpointer d)
{
    (void)d;
    int n = trash_count() > 0;
    if (n != full) {
        full = n;
        gtk_widget_queue_draw(da);
    }
    return G_SOURCE_CONTINUE;
}

/* Something went into or out of the trash: the kernel says so (inotify,
   through GIO's file monitor), so nothing has to look every few seconds. */
static void on_trash_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev,
                             gpointer d)
{
    (void)m, (void)f, (void)other, (void)ev;
    poll_full(d);
}

/* The root window's picture appeared or changed: paint again. */
static GdkFilterReturn root_filter(GdkXEvent *xev, GdkEvent *event, gpointer data)
{
    XEvent *e = (XEvent *)xev;
    (void)event, (void)data;
    if (e->type == PropertyNotify &&
        e->xproperty.atom == XInternAtom(e->xproperty.display, "_XROOTPMAP_ID", False))
        gtk_widget_queue_draw(da);
    return GDK_FILTER_CONTINUE;
}

static void desk_type(void)
{
    GdkWindow *gw = gtk_widget_get_window(win);
    Display *dpy = gdk_x11_get_default_xdisplay();
    Window xw = gdk_x11_window_get_xid(gw);
    Atom wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desk = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, xw, wtype, XA_ATOM, 32, PropModeReplace, (unsigned char *)&desk, 1);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom skip[2] = {
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False)
    };
    XChangeProperty(dpy, xw, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)skip, 2);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    g_set_prgname("myos-trash");
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), "Recycle Bin");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    da = gtk_drawing_area_new();
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(win), da);

    GtkTargetEntry te = { "text/uri-list", 0, 0 };
    gtk_drag_dest_set(da, GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect(da, "drag-data-received", G_CALLBACK(on_drag), NULL);
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(da, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_realize(win);
    desk_type();
    place();
    full = trash_count() > 0;
    char dir[PATH_MAX];
    trash_path(dir, sizeof dir);
    mkdir(dir, 0700);                   /* (GIO polls a folder that isn't there yet) */
    GFile *tf = g_file_new_for_path(dir);
    GFileMonitor *mon = g_file_monitor_directory(tf, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(tf);
    if (mon)
        g_signal_connect(mon, "changed", G_CALLBACK(on_trash_changed), NULL);
    else
        g_timeout_add_seconds(2, poll_full, NULL);      /* no monitor: look every 2 s */
    GdkWindow *groot = gdk_get_default_root_window();
    gdk_window_set_events(groot, gdk_window_get_events(groot) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(groot, root_filter, NULL);
    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
