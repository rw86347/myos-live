/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * A tray pop-up stays on the tray. Under the mouse the tray grows upward
 * (myos-menu), and each step of that it writes its top edge, in screen
 * pixels, to _MYOS_TRAY_TOP on the root window. A pop-up that calls
 * tray_follow moves with it, so its point stays on the tray's top: up as
 * the tray grows, down as it folds away.
 *
 * Cost: one small property read and one window move per step of the
 * tray's move (about 30 in half a second), none while it is still.
 */
#ifndef MYOS_TRAY_FOLLOW_H
#define MYOS_TRAY_FOLLOW_H

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

static GtkWidget *tray_follow_win;
static int tray_follow_x;               /* the pop-up's left, in GTK's pixels */

static void tray_follow_now(void)
{
    GdkWindow *root = gdk_get_default_root_window();
    GdkAtom type;
    gint fmt, len;
    guchar *data = NULL;
    if (!gdk_property_get(root, gdk_atom_intern_static_string("_MYOS_TRAY_TOP"),
                          gdk_atom_intern_static_string("CARDINAL"), 0, 1, FALSE,
                          &type, &fmt, &len, &data))
        return;
    long top = fmt == 32 && len >= (gint)sizeof(long) ? *(long *)data : -1;
    g_free(data);
    if (top < 0)
        return;
    /* GTK's pixels are two screen pixels each at 200%. */
    int sf = gdk_window_get_scale_factor(root);
    int y = (int)top / sf - gtk_widget_get_allocated_height(tray_follow_win) - 2;
    gtk_window_move(GTK_WINDOW(tray_follow_win), tray_follow_x, y < 4 ? 4 : y);
}

static GdkFilterReturn tray_follow_filter(GdkXEvent *xev, GdkEvent *ev, gpointer unused)
{
    (void)ev, (void)unused;
    XEvent *e = xev;
    if (e->type == PropertyNotify &&
        e->xproperty.atom == gdk_x11_get_xatom_by_name("_MYOS_TRAY_TOP"))
        tray_follow_now();
    return GDK_FILTER_CONTINUE;
}

/* Call once the window is placed: x is where it was put. */
static void tray_follow(GtkWidget *win, int x)
{
    GdkWindow *root = gdk_get_default_root_window();
    tray_follow_win = win;
    tray_follow_x = x;
    gdk_window_set_events(root, gdk_window_get_events(root) | GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(root, tray_follow_filter, NULL);
}

#endif
