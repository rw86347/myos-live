/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-volume — the speaker in the tray: how loud, and mute.
 *
 *   myos-volume --at X Y    the pop-up: a slider and Mute, its bottom
 *                           centred above X Y (myos-menu passes the
 *                           speaker icon's place when it is clicked)
 *   myos-volume --up        one step louder   } the mouse wheel over the
 *   myos-volume --down      one step quieter  } speaker; no window
 *   myos-volume --mute      mute, or unmute   (middle click)
 *
 * It talks to the sound card's mixer the way alsa-lib does underneath:
 * the control device /dev/snd/controlC0 and its ioctls, looking the
 * controls up by name — "Master Playback Volume" (one value per channel)
 * and "Master Playback Switch" (1 = sound on). The structures are Linux's
 * (include/sound/asound.h, the same header the kernel and sndtest use), so
 * this works on any Linux too. No alsa-lib: the live disk has the library
 * but not its headers, and two ioctls don't need it.
 *
 * The pop-up closes on a click anywhere else, on Escape, or when it loses
 * the keyboard. The slider moves in the card's own steps (HD Audio: about
 * 0.75 dB each, so equal steps sound equally loud) shown as 0-100 %.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "asound.h"        /* include/sound/asound.h: ours, not linux-headers' (other field names) */

#define CTL_DEV  "/dev/snd/controlC0"
#define VOL_NAME "Master Playback Volume"
#define SW_NAME  "Master Playback Switch"
#define STEP     5                  /* percent per wheel notch */

static int ctl = -1;
static long vmin, vmax;
static unsigned vcount;             /* channels */
static GtkWidget *win, *scale, *mute;
static int busy;                    /* we are setting the widgets ourselves */

static void elem_name(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof *id);
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)id->name, sizeof id->name, "%s", name);
}

/* Open the card and learn the volume's range. 0, or -1 with a message. */
static int mixer_open(void)
{
    ctl = open(CTL_DEV, O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        fprintf(stderr, "myos-volume: %s: %s\n", CTL_DEV, strerror(errno));
        return -1;
    }
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof info);
    elem_name(&info.id, VOL_NAME);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || info.count == 0 ||
        info.value.integer.max <= info.value.integer.min) {
        fprintf(stderr, "myos-volume: no \"%s\" on this card\n", VOL_NAME);
        return -1;
    }
    vmin = info.value.integer.min;
    vmax = info.value.integer.max;
    vcount = info.count > 128 ? 128 : info.count;
    return 0;
}

static int get_percent(void)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, VOL_NAME);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        return 0;
    long best = v.value.integer[0];                 /* the loudest channel */
    for (unsigned k = 1; k < vcount; k++)
        if (v.value.integer[k] > best)
            best = v.value.integer[k];
    return (int)((best - vmin) * 100 + (vmax - vmin) / 2) / (int)(vmax - vmin);
}

static void set_percent(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, VOL_NAME);
    long raw = vmin + ((long)pct * (vmax - vmin) + 50) / 100;
    for (unsigned k = 0; k < vcount; k++)
        v.value.integer[k] = raw;
    ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

/* 1 if the sound is on. A card without the switch is never muted. */
static int get_on(void)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, SW_NAME);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        return 1;
    return v.value.integer[0] != 0;
}

static void set_on(int on)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, SW_NAME);
    for (unsigned k = 0; k < vcount; k++)
        v.value.integer[k] = on ? 1 : 0;
    ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

/* --- the pop-up ------------------------------------------------------------ */

static void scale_moved(GtkRange *r, gpointer unused)
{
    (void)unused;
    if (busy)
        return;
    set_percent((int)gtk_range_get_value(r));
    if (!get_on()) {                /* moving the slider turns the sound back on */
        set_on(1);
        busy = 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute), FALSE);
        busy = 0;
    }
}

static void mute_toggled(GtkToggleButton *b, gpointer unused)
{
    (void)unused;
    if (!busy)
        set_on(!gtk_toggle_button_get_active(b));
}

static gboolean key(GtkWidget *w, GdkEventKey *e, gpointer unused)
{
    (void)w, (void)unused;
    if (e->keyval == GDK_KEY_Escape) {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

/* A click outside the window ends the pop-up (we hold the pointer). */
static gboolean button(GtkWidget *w, GdkEventButton *e, gpointer unused)
{
    (void)unused;
    int ww = gtk_widget_get_allocated_width(w), wh = gtk_widget_get_allocated_height(w);
    if (e->x < 0 || e->y < 0 || e->x >= ww || e->y >= wh) {
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static gboolean grab_it(gpointer unused)
{
    (void)unused;
    GdkWindow *gw = gtk_widget_get_window(win);
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    if (gw)
        gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void popup(int ax, int ay)
{
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Volume");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Volume</b>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_size_request(scale, 240, -1);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    mute = gtk_check_button_new_with_label("Mute");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), mute, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_container_add(GTK_CONTAINER(win), frame);

    busy = 1;
    gtk_range_set_value(GTK_RANGE(scale), get_percent());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute), !get_on());
    busy = 0;
    g_signal_connect(scale, "value-changed", G_CALLBACK(scale_moved), NULL);
    g_signal_connect(mute, "toggled", G_CALLBACK(mute_toggled), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(button), NULL);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Above the speaker: bottom centre at (ax, ay), inside the screen. */
    GtkRequisition req;
    gtk_widget_show_all(box);
    gtk_widget_get_preferred_size(frame, NULL, &req);
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (m)
        gdk_monitor_get_geometry(m, &mon);
    int x = ax - req.width / 2, y = ay - req.height - 4;
    if (x + req.width > mon.x + mon.width - 4)
        x = mon.x + mon.width - 4 - req.width;
    if (x < mon.x + 4)
        x = mon.x + 4;
    if (y < mon.y + 4)
        y = mon.y + 4;
    gtk_window_move(GTK_WINDOW(win), x, y);
    gtk_widget_show_all(win);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(scale);
    g_timeout_add(50, grab_it, NULL);   /* once it is mapped */
    gtk_main();
}

int main(int argc, char **argv)
{
    int ax = -1, ay = -1, step = 0, toggle = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--at") && i + 2 < argc) {
            ax = atoi(argv[++i]);
            ay = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--up"))
            step = STEP;
        else if (!strcmp(argv[i], "--down"))
            step = -STEP;
        else if (!strcmp(argv[i], "--mute"))
            toggle = 1;
        else {
            fprintf(stderr, "usage: myos-volume [--at X Y | --up | --down | --mute]\n");
            return 2;
        }
    }
    if (mixer_open() < 0)
        return 1;
    if (step) {
        set_percent(get_percent() + step);
        if (step > 0 && !get_on())
            set_on(1);
        return 0;
    }
    if (toggle) {
        set_on(!get_on());
        return 0;
    }
    gtk_init(&argc, &argv);
    if (ax < 0 || ay < 0) {             /* no place given: near the pointer */
        GdkDevice *p = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
        gdk_device_get_position(p, NULL, &ax, &ay);
    }
    popup(ax, ay);
    return 0;
}
