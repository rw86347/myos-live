/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * The tray menus. The same shape as the battery card: a rounded rectangle
 * with a point at the bottom, under the icon that opened it, so it sits
 * above the taskbar. The fill is the Mac menu's light gray. Outside the
 * bubble is clear when a compositor is running, and cut away when it isn't
 * (X has no see-through windows without one).
 *
 * One program includes this. The caret's place is menu_caret_x, measured
 * from the window's left edge, set by menu_place.
 */
#ifndef MYOS_MENU_BUBBLE_H
#define MYOS_MENU_BUBBLE_H

#include <gtk/gtk.h>

#define MENU_RADIUS 14
#define MENU_CARET  12

static int menu_caret_x;
static int menu_composited;

static void menu_bubble_path(cairo_t *cr, double w, double h, double inset)
{
    double l = inset, t = inset, r = w - inset, b = h - MENU_CARET - inset;
    double rad = MENU_RADIUS, cx = menu_caret_x;
    if (rad > (b - t) / 2)
        rad = (b - t) / 2;
    if (cx < l + rad + MENU_CARET)
        cx = l + rad + MENU_CARET;
    if (cx > r - rad - MENU_CARET)
        cx = r - rad - MENU_CARET;
    cairo_new_path(cr);
    cairo_arc(cr, l + rad, t + rad, rad, G_PI, 1.5 * G_PI);
    cairo_arc(cr, r - rad, t + rad, rad, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, r - rad, b - rad, rad, 0, 0.5 * G_PI);
    cairo_line_to(cr, cx + MENU_CARET, b);
    cairo_line_to(cr, cx, h - inset);
    cairo_line_to(cr, cx - MENU_CARET, b);
    cairo_arc(cr, l + rad, b - rad, rad, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

/* Drawn behind the menu's widgets (return FALSE). */
static gboolean menu_draw_bubble(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double ww = gtk_widget_get_allocated_width(w), wh = gtk_widget_get_allocated_height(w);
    if (menu_composited) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_save(cr);
        cairo_translate(cr, 0, 2);
        menu_bubble_path(cr, ww, wh, 2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
        cairo_fill(cr);
        cairo_restore(cr);
    } else {
        /* The smoothed edge blends into the border, not into black. */
        cairo_set_source_rgb(cr, 0xd0 / 255.0, 0xd0 / 255.0, 0xd4 / 255.0);
        cairo_paint(cr);
    }
    menu_bubble_path(cr, ww, wh, 0.5);
    cairo_set_source_rgb(cr, 0xe8 / 255.0, 0xe8 / 255.0, 0xed / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return FALSE;
}

static void menu_cut_shape(GtkWidget *w, GdkRectangle *a, gpointer unused)
{
    (void)unused;
    if (menu_composited)
        return;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A1, a->width, a->height);
    cairo_t *cr = cairo_create(s);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    menu_bubble_path(cr, a->width, a->height, 0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_region_t *r = gdk_cairo_region_create_from_surface(s);
    gtk_widget_shape_combine_region(w, r);
    cairo_region_destroy(r);
    cairo_surface_destroy(s);
}

static void menu_bubble_prepare(GtkWidget *win)
{
    GdkScreen *scr = gtk_widget_get_screen(win);
    menu_composited = 0;
    if (gdk_screen_is_composited(scr) && gdk_screen_get_rgba_visual(scr)) {
        gtk_widget_set_visual(win, gdk_screen_get_rgba_visual(scr));
        menu_composited = 1;
    }
    gtk_widget_set_app_paintable(win, TRUE);
    g_signal_connect(win, "draw", G_CALLBACK(menu_draw_bubble), NULL);
    g_signal_connect(win, "size-allocate", G_CALLBACK(menu_cut_shape), NULL);
}

/* The point just above (ax, ay): the icon's middle, the tray's top.
   The card stays on the screen; the point stays on the icon. */
static void menu_place(GtkWidget *win, GtkWidget *card, int ax, int ay)
{
    GtkRequisition req;
    gtk_widget_get_preferred_size(card, NULL, &req);
    if (req.width < 2)
        req.width = 2;
    if (req.height < 2)
        req.height = 2;
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (!m)
        m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (m)
        gdk_monitor_get_geometry(m, &mon);
    int x = ax - req.width / 2, y = ay - req.height - 2;
    if (x + req.width > mon.x + mon.width - 4)
        x = mon.x + mon.width - 4 - req.width;
    if (x < mon.x + 4)
        x = mon.x + 4;
    if (y < mon.y + 4)
        y = mon.y + 4;
    menu_caret_x = ax - x;
    gtk_window_resize(GTK_WINDOW(win), req.width, req.height);
    gtk_window_move(GTK_WINDOW(win), x, y);
    gtk_widget_queue_draw(win);
}

#endif
