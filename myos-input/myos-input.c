/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-input — Ctrl+Space between the keyboards, as on the Mac.
 *
 * The keyboards are IBus's engines (English, and pinyin for Chinese).
 * IBus has its own Ctrl+Space and its own pop-up, a plain GTK window that
 * doesn't look like the rest of the desktop, so MyOS turns IBus's key off
 * (90_myos-ibus.gschema.override) and this program takes it instead:
 *
 *   - it grabs Ctrl+Space on the root window, the way IBus did, so the key
 *     works whatever window has the keyboard;
 *   - each press moves to the next keyboard in IBus's list (preload-engines)
 *     and shows the list in the same bubble as the Sound and battery menus
 *     (common/menu_bubble.h), its point on the tray, the chosen keyboard
 *     drawn in blue;
 *   - the bubble goes a moment after the last press. Pressing again while
 *     it shows moves the blue row: there is only ever one bubble.
 *
 * The bubble is a pop-up window (override-redirect): the window manager
 * doesn't place it and it never takes the keyboard from what you type in.
 *
 * Started once per desktop from /etc/icewm/startup. A second copy can't
 * grab the key and quits.
 *
 * Cost: a press asks IBus nothing (the keyboards, and which one is on, are
 * kept from IBus's signals); it sends the switch without waiting for it,
 * moves the blue row and shows the window. Nothing runs between presses.
 */
#include <stdio.h>
#include <string.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "menu_bubble.h"

/* libibus. Alpine's ibus package has the library; its headers are in
   ibus-dev, which the sysroot doesn't carry. These are the calls we use,
   as ibus 1.5 declares them. */
typedef struct _IBusBus IBusBus;
typedef struct _IBusEngineDesc IBusEngineDesc;
void ibus_init(void);
IBusBus *ibus_bus_new(void);
gboolean ibus_bus_is_connected(IBusBus *bus);
IBusEngineDesc *ibus_bus_get_global_engine(IBusBus *bus);
void ibus_bus_set_global_engine_async(IBusBus *bus, const gchar *name, gint timeout_msec,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data);
gboolean ibus_bus_set_global_engine_async_finish(IBusBus *bus, GAsyncResult *res, GError **error);
IBusEngineDesc **ibus_bus_get_engines_by_names(IBusBus *bus, const gchar *const *names);
const gchar *ibus_engine_desc_get_name(IBusEngineDesc *d);
const gchar *ibus_engine_desc_get_longname(IBusEngineDesc *d);
const gchar *ibus_engine_desc_get_symbol(IBusEngineDesc *d);
const gchar *ibus_engine_desc_get_language(IBusEngineDesc *d);
void ibus_bus_set_watch_ibus_signal(IBusBus *bus, gboolean watch);

#define SHOW_MS  1200               /* the bubble stays this long after a press */
#define MAX_KB   8

static IBusBus *bus;
static GSettings *general;          /* org.freedesktop.ibus.general */
static GtkWidget *win, *card, *rows[MAX_KB], *badges[MAX_KB];
static guint hide_id;
static int held;                    /* Ctrl+Space is down (the key repeats) */
static KeyCode space;

struct kb {
    char name[64], label[64], badge[8];
};
static struct kb kbs[MAX_KB];
static int n_kbs, chosen;

static const char css[] =
    "window { background-color: transparent; }"
    ".title { font-size: 13px; font-weight: bold; color: #1d1d1f; }"
    ".name { font-size: 13px; color: #1d1d1f; }"
    ".menu-row { background-color: transparent; border-radius: 6px; }"
    ".chosen { background-color: #007aff; }"
    ".chosen .name { color: #ffffff; }";

/* The names IBus gives its engines are its own ("Intelligent Pinyin");
   these are the Mac's for the two the live disk has. */
static const char *friendly(const char *engine)
{
    if (!strcmp(engine, "xkb:us::eng"))
        return "English (US)";
    if (!strcmp(engine, "libpinyin"))
        return "Pinyin \u2013 Simplified";
    return NULL;
}

static void engine_changed(IBusBus *b, const gchar *name, gpointer unused);
static void keyboards_changed(void);

/* The link to ibus-daemon, made again if the daemon went. */
static int bus_up(void)
{
    if (bus && ibus_bus_is_connected(bus))
        return 0;
    if (bus)
        g_object_unref(bus);
    bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus))
        return -1;
    ibus_bus_set_watch_ibus_signal(bus, TRUE);
    g_signal_connect(bus, "global-engine-changed", G_CALLBACK(engine_changed), NULL);
    g_signal_connect_swapped(bus, "connected", G_CALLBACK(keyboards_changed), NULL);
    return 0;
}

/* The keyboards, in IBus's order, and which one is on. Asked of IBus
   here, when the desktop starts and when the list changes, never on a
   press: each call waits for the daemon. 0, or -1 when IBus isn't there. */
static int read_keyboards(void)
{
    if (bus_up() < 0)
        return -1;
    gchar **names = g_settings_get_strv(general, "preload-engines");
    IBusEngineDesc **descs = ibus_bus_get_engines_by_names(bus, (const gchar *const *)names);
    g_strfreev(names);
    n_kbs = 0;
    for (int i = 0; descs && descs[i]; i++) {
        if (n_kbs < MAX_KB) {
            struct kb *k = &kbs[n_kbs++];
            const char *name = ibus_engine_desc_get_name(descs[i]);
            const char *f = friendly(name);
            const char *sym = ibus_engine_desc_get_symbol(descs[i]);
            snprintf(k->name, sizeof k->name, "%s", name);
            snprintf(k->label, sizeof k->label, "%s", f ? f : ibus_engine_desc_get_longname(descs[i]));
            if (sym && sym[0])
                snprintf(k->badge, sizeof k->badge, "%s", sym);
            else {
                const char *lang = ibus_engine_desc_get_language(descs[i]);
                snprintf(k->badge, sizeof k->badge, "%.2s", lang ? lang : "?");
            }
            for (char *c = k->badge; *c; c++)
                if (*c >= 'a' && *c <= 'z')
                    *c -= 'a' - 'A';
        }
        g_object_unref(descs[i]);
    }
    g_free(descs);
    if (!n_kbs)
        return -1;
    chosen = 0;
    IBusEngineDesc *now = ibus_bus_get_global_engine(bus);
    if (now) {
        for (int i = 0; i < n_kbs; i++)
            if (!strcmp(kbs[i].name, ibus_engine_desc_get_name(now)))
                chosen = i;
        g_object_unref(now);
    }
    return 0;
}

/* The keyboard's badge: its letters in a rounded box, the way the Mac's
   menu bar shows an input source. White on the blue row. */
static gboolean draw_badge(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int i = GPOINTER_TO_INT(data);
    double bw = gtk_widget_get_allocated_width(w), bh = gtk_widget_get_allocated_height(w);
    double r = 4;
    cairo_new_sub_path(cr);
    cairo_arc(cr, bw - r - 0.5, r + 0.5, r, -G_PI / 2, 0);
    cairo_arc(cr, bw - r - 0.5, bh - r - 0.5, r, 0, G_PI / 2);
    cairo_arc(cr, r + 0.5, bh - r - 0.5, r, G_PI / 2, G_PI);
    cairo_arc(cr, r + 0.5, r + 0.5, r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
    if (i == chosen)
        cairo_set_source_rgb(cr, 1, 1, 1);
    else
        cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_fill(cr);

    PangoLayout *pl = gtk_widget_create_pango_layout(w, kbs[i].badge);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 8");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    if (i == chosen)
        cairo_set_source_rgb(cr, 0.0, 0.48, 1.0);
    else
        cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (bw - tw) / 2, (bh - th) / 2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return TRUE;
}

/* Where the point goes: on IBus's icon when it shows, else on the middle
   of the tray (the icon waits in the panel that grows out of it).
   The tray's windows are myos-menu's override-redirect children of the
   root; the icon is IBus's, inside one of them. IBus's own pop-ups are
   override-redirect children of the root too, so a match at the top
   doesn't count. Screen pixels. 0, or -1. */
static int is_ibus(Display *d, Window w)
{
    XClassHint ch;
    if (!XGetClassHint(d, w, &ch))
        return 0;
    int ours = ch.res_name && !strcmp(ch.res_name, "ibus-ui-gtk");     /* its icon's name, gtk3 or not */
    XFree(ch.res_name);
    XFree(ch.res_class);
    return ours;
}

static int icon_window(Display *d, Window w, int depth, Window *found)
{
    if (is_ibus(d, w)) {
        *found = w;
        return 0;
    }
    if (depth == 0)
        return -1;
    Window r, p, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(d, w, &r, &p, &kids, &n))
        return -1;
    int rc = -1;
    for (unsigned i = 0; i < n && rc; i++)
        rc = icon_window(d, kids[i], depth - 1, found);
    if (kids)
        XFree(kids);
    return rc;
}

/* 1 if one of w's children is a docked icon (it has _XEMBED_INFO). */
static int holds_icon(Display *d, Window w)
{
    Atom info = XInternAtom(d, "_XEMBED_INFO", False), type;
    Window r, p, *kids = NULL;
    unsigned n = 0;
    int found = 0;
    if (!XQueryTree(d, w, &r, &p, &kids, &n))
        return 0;
    for (unsigned i = 0; i < n && !found; i++) {
        int fmt;
        unsigned long len, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d, kids[i], info, 0, 2, False, AnyPropertyType, &type, &fmt,
                               &len, &after, &data) == Success && type != None)
            found = 1;
        if (data)
            XFree(data);
    }
    if (kids)
        XFree(kids);
    return found;
}

/* The tray and IBus's icon, found once: looking means asking the X
   server about every window at the top, too slow for each press. Looked
   for again when one of them goes (myos-menu starts over at a new scale). */
static Window tray_w, icon_w;

static void find_tray(Display *d, Window root)
{
    Window r, p, *kids = NULL;
    unsigned n = 0;
    tray_w = icon_w = 0;
    if (!XQueryTree(d, root, &r, &p, &kids, &n))
        return;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(d, kids[i], &a) || !a.override_redirect || is_ibus(d, kids[i]))
            continue;
        if (!tray_w && a.map_state == IsViewable && holds_icon(d, kids[i]))
            tray_w = kids[i];
        if (!icon_w)
            icon_window(d, kids[i], 3, &icon_w);
    }
    if (kids)
        XFree(kids);
}

/* 0, and the point, if the icon shows (in the tray or the open panel), or
   at least the tray does. */
static int point_at(Display *d, Window root, int *ax, int *ay)
{
    XWindowAttributes a;
    Window child;
    int x, y;
    if (icon_w && XGetWindowAttributes(d, icon_w, &a) && a.map_state == IsViewable &&
        XTranslateCoordinates(d, icon_w, root, 0, 0, &x, &y, &child)) {
        *ax = x + a.width / 2;
        *ay = y;
        return 0;
    }
    if (tray_w && XGetWindowAttributes(d, tray_w, &a) && a.map_state == IsViewable) {
        *ax = a.x + a.width / 2;        /* the icon is in the closed panel */
        *ay = a.y;
        return 0;
    }
    return -1;
}

static int tray_point(int *ax, int *ay)
{
    Display *d = gdk_x11_get_default_xdisplay();
    Window root = DefaultRootWindow(d);
    gdk_x11_display_error_trap_push(gdk_display_get_default());
    int rc = point_at(d, root, ax, ay);
    if (rc) {
        find_tray(d, root);
        rc = point_at(d, root, ax, ay);
    }
    gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
    if (rc)
        return -1;

    /* The tray's top, which myos-menu keeps up to date (tray_follow.h). */
    GdkAtom type;
    gint fmt, len;
    guchar *data = NULL;
    if (gdk_property_get(gdk_get_default_root_window(),
                         gdk_atom_intern_static_string("_MYOS_TRAY_TOP"),
                         gdk_atom_intern_static_string("CARDINAL"), 0, 1, FALSE,
                         &type, &fmt, &len, &data)) {
        if (fmt == 32 && len >= (gint)sizeof(long) && *(long *)data > 0)
            *ay = (int)*(long *)data;
        g_free(data);
    }
    return 0;
}

static gboolean hide_now(gpointer unused)
{
    (void)unused;
    gtk_widget_hide(win);
    hide_id = 0;
    return G_SOURCE_REMOVE;
}

static GtkWidget *row_for(int i)
{
    GtkWidget *row = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "menu-row");
    gtk_widget_set_margin_start(row, 6);
    gtk_widget_set_margin_end(row, 6);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(h), 4);
    GtkWidget *badge = gtk_drawing_area_new();
    gtk_widget_set_size_request(badge, 24, 16);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    g_signal_connect(badge, "draw", G_CALLBACK(draw_badge), GINT_TO_POINTER(i));
    gtk_box_pack_start(GTK_BOX(h), badge, FALSE, FALSE, 4);
    GtkWidget *l = gtk_label_new(kbs[i].label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "name");
    gtk_box_pack_start(GTK_BOX(h), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), h);
    rows[i] = row, badges[i] = badge;
    return row;
}

/* The blue row: the chosen keyboard. */
static void mark_chosen(void)
{
    for (int i = 0; card && i < n_kbs; i++) {
        GtkStyleContext *sc = gtk_widget_get_style_context(rows[i]);
        if (i == chosen)
            gtk_style_context_add_class(sc, "chosen");
        else
            gtk_style_context_remove_class(sc, "chosen");
        gtk_widget_queue_draw(badges[i]);
    }
}

/* The bubble's rows, made when the keyboards are read; a press only
   moves the blue row. */
static void build_card(void)
{
    if (card)
        gtk_widget_destroy(card);
    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(card, 240, -1);
    gtk_widget_set_margin_top(card, 10);
    gtk_widget_set_margin_bottom(card, MENU_CARET + 6);
    GtkWidget *title = gtk_label_new("Keyboard");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "title");
    gtk_widget_set_margin_start(title, 14);
    gtk_widget_set_margin_bottom(title, 4);
    gtk_box_pack_start(GTK_BOX(card), title, FALSE, FALSE, 0);
    for (int i = 0; i < n_kbs; i++)
        gtk_box_pack_start(GTK_BOX(card), row_for(i), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), card);
    gtk_widget_show_all(card);
    mark_chosen();
}

static void show_bubble(void)
{
    mark_chosen();
    int ax, ay;
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (!m)
        m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    int sf = gdk_window_get_scale_factor(gdk_get_default_root_window());
    if (tray_point(&ax, &ay) == 0) {
        ax /= sf, ay /= sf;         /* GTK's pixels are two screen pixels at 200% */
    } else {                        /* no tray: the bottom right of the screen */
        GdkRectangle wa = { 0, 0, 1024, 768 };
        if (m)
            gdk_monitor_get_workarea(m, &wa);
        ax = wa.x + wa.width - 140;
        ay = wa.y + wa.height;
    }
    menu_place(win, card, ax, ay);
    gtk_widget_show(win);
    gdk_window_raise(gtk_widget_get_window(win));

    if (hide_id)
        g_source_remove(hide_id);
    hide_id = g_timeout_add(SHOW_MS, hide_now, NULL);
}

/* The badges drawn once off the screen: 拼 is in the Chinese font, which
   is big, and is only read when something is first drawn with it. */
static void load_fonts(void)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 20);
    cairo_t *cr = cairo_create(s);
    for (int i = 0; i < n_kbs; i++) {
        PangoLayout *pl = gtk_widget_create_pango_layout(win, kbs[i].badge);
        PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 8");
        pango_layout_set_font_description(pl, fd);
        pango_font_description_free(fd);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

/* IBus came (again), or its list of keyboards changed in Settings. */
static void keyboards_changed(void)
{
    if (read_keyboards() < 0)
        return;
    build_card();
    GtkRequisition req;
    gtk_widget_get_preferred_size(card, NULL, &req);
    load_fonts();
}

static void preload_changed(GSettings *g, const gchar *key, gpointer unused)
{
    (void)g, (void)key, (void)unused;
    keyboards_changed();
}

/* IBus is usually up before us, but not always: tried every 2 s for half
   a minute. */
static gboolean warm_up(gpointer unused)
{
    static int tries;
    (void)unused;
    keyboards_changed();
    if (!n_kbs)
        return ++tries < 15 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    return G_SOURCE_REMOVE;
}

/* The switch is asynchronous: the first switch to pinyin starts its
   engine, which loads its dictionary, and the bubble mustn't wait for
   that. While IBus hasn't answered, the keyboard asked for is the one
   that counts, not the one IBus still reports. */
static int asking;                  /* switches IBus hasn't answered yet */

static void switched(GObject *src, GAsyncResult *res, gpointer unused)
{
    (void)unused;
    ibus_bus_set_global_engine_async_finish((IBusBus *)src, res, NULL);
    asking--;
}

/* Someone else changed the keyboard (IBus's own menu in the tray). */
static void engine_changed(IBusBus *b, const gchar *name, gpointer unused)
{
    (void)b, (void)unused;
    if (asking)
        return;
    for (int i = 0; i < n_kbs; i++)
        if (!strcmp(kbs[i].name, name))
            chosen = i;
    mark_chosen();
}

/* The switch goes once the bubble is on the screen: the first switch to
   pinyin starts its engine, which takes the processor while it reads its
   dictionary, and on one core the X server would draw nothing until
   then, the bubble included. */
static guint switch_id;

static gboolean send_switch(gpointer unused)
{
    (void)unused;
    switch_id = 0;
    gdk_display_sync(gdk_display_get_default());    /* the bubble is drawn by now */
    asking++;
    ibus_bus_set_global_engine_async(bus, kbs[chosen].name, -1, NULL, switched, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean drawn(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)w, (void)cr, (void)unused;
    if (switch_id) {
        g_source_remove(switch_id);
        switch_id = g_idle_add(send_switch, NULL);
    }
    return FALSE;
}

/* A press: no questions to IBus, so the bubble shows at once. */
static void next_keyboard(void)
{
    if (!n_kbs)
        keyboards_changed();
    if (!n_kbs || bus_up() < 0)
        return;
    if (n_kbs > 1) {
        chosen = (chosen + 1) % n_kbs;
        if (switch_id)
            g_source_remove(switch_id);
        switch_id = g_timeout_add(200, send_switch, NULL);  /* if no draw comes */
    }
    show_bubble();
}

static GdkFilterReturn key_filter(GdkXEvent *xev, GdkEvent *ev, gpointer unused)
{
    (void)ev, (void)unused;
    XEvent *e = xev;
    if ((e->type != KeyPress && e->type != KeyRelease) || e->xkey.keycode != space)
        return GDK_FILTER_CONTINUE;
    if (e->type == KeyRelease) {
        held = 0;
    } else if (!held) {
        held = 1;
        next_keyboard();
    }
    return GDK_FILTER_REMOVE;
}

/* Ctrl+Space, whatever Caps Lock and Num Lock are doing. */
static int grab_key(void)
{
    Display *d = gdk_x11_get_default_xdisplay();
    Window root = DefaultRootWindow(d);
    static const unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    space = XKeysymToKeycode(d, XK_space);
    /* Holding the key sends presses with no releases between them. */
    XkbSetDetectableAutoRepeat(d, True, NULL);
    gdk_x11_display_error_trap_push(gdk_display_get_default());
    for (unsigned i = 0; i < sizeof locks / sizeof locks[0]; i++)
        XGrabKey(d, space, ControlMask | locks[i], root, False, GrabModeAsync, GrabModeAsync);
    if (gdk_x11_display_error_trap_pop(gdk_display_get_default()))
        return -1;                  /* someone else has it: another copy, or IBus */
    gdk_window_add_filter(gdk_get_default_root_window(), key_filter, NULL);
    return 0;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GSettingsSchema *s = g_settings_schema_source_lookup(g_settings_schema_source_get_default(),
                                                         "org.freedesktop.ibus.general", TRUE);
    if (!s) {
        fprintf(stderr, "myos-input: IBus isn't installed\n");
        return 1;
    }
    g_settings_schema_unref(s);
    general = g_settings_new("org.freedesktop.ibus.general");
    g_signal_connect(general, "changed::preload-engines", G_CALLBACK(preload_changed), NULL);
    ibus_init();
    if (grab_key() < 0) {
        fprintf(stderr, "myos-input: Ctrl+Space is taken\n");
        return 1;
    }

    GtkCssProvider *cssp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cssp, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cssp),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(win), "Keyboard");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    menu_bubble_prepare(win);
    g_signal_connect_after(win, "draw", G_CALLBACK(drawn), NULL);
    if (warm_up(NULL) != G_SOURCE_REMOVE)
        g_timeout_add(2000, warm_up, NULL);
    gtk_main();
    return 0;
}
