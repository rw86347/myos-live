/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-volume — the speaker in the tray.
 *
 *   myos-volume --at X Y    the Sound menu, its point centred above X Y
 *                           (myos-menu passes the speaker icon's place)
 *   myos-volume --up        one step louder   } the mouse wheel over the
 *   myos-volume --down      one step quieter  } speaker; no window
 *   myos-volume --mute      mute, or unmute   (middle click)
 *
 * The menu is the Mac's Sound menu, drawn as the same bubble as the
 * battery card: "Sound", a slider with a quiet speaker and a loud one,
 * then Output (the playback devices on the cards) and Sound Settings.
 * Choosing an output writes ~/.asoundrc, which is what new programs open.
 * A program already playing keeps the device it opened.
 *
 * It talks to the mixer the way alsa-lib does underneath: the control
 * device /dev/snd/controlC* and its ioctls, looking the controls up by
 * name. The structures are Linux's (include/sound/asound.h). No alsa-lib:
 * the live disk has the library but not its headers, and these ioctls
 * don't need it.
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

#include "asound.h"        /* include/sound/asound.h: ours, not linux-headers' */
#include "menu_bubble.h"
#include "tray_follow.h"

#define STEP     5                  /* percent per wheel notch */
#define OUT_MAX  8

static int ctl = -1, ctl_card;
static long vmin, vmax;
static unsigned vcount;             /* channels */
static char vol_name[44];
static char sw_name[44];
static GtkWidget *win, *card, *scale, *quiet, *loud;
static int busy;                    /* we are setting the widgets ourselves */
static int want_card, want_dev;

struct output {
    int card, device, kind;         /* 0 speakers, 1 display, 2 headphones */
    char name[80];
};

static struct output outs[OUT_MAX];
static int n_outs, out_sel = -1;
static GtkWidget *out_icon[OUT_MAX];

static void elem_name(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof *id);
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)id->name, sizeof id->name, "%s", name);
}

/* The loudness control on this card: Master, or the names a card uses
   when it has no Master. 0, or -1. */
static int bind_volume(void)
{
    static const char *const names[] = {
        "Master Playback Volume", "PCM Playback Volume",
        "Speaker Playback Volume", "Headphone Playback Volume",
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof info);
        elem_name(&info.id, names[i]);
        if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || info.count == 0 ||
            info.value.integer.max <= info.value.integer.min)
            continue;
        vmin = info.value.integer.min;
        vmax = info.value.integer.max;
        vcount = info.count > 128 ? 128 : info.count;
        snprintf(vol_name, sizeof vol_name, "%s", names[i]);
        snprintf(sw_name, sizeof sw_name, "%s", names[i]);
        char *word = strstr(sw_name, "Volume");     /* both words are 6 letters */
        if (word)
            memcpy(word, "Switch", 6);
        return 0;
    }
    return -1;
}

/* Open one card's mixer. 0, or -1 with a message. */
static int mixer_open_card(int card)
{
    if (ctl >= 0)
        close(ctl);
    char path[32];
    snprintf(path, sizeof path, "/dev/snd/controlC%d", card);
    ctl = open(path, O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        fprintf(stderr, "myos-volume: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (bind_volume() < 0) {
        fprintf(stderr, "myos-volume: no playback volume on card %d\n", card);
        close(ctl);
        ctl = -1;
        return -1;
    }
    ctl_card = card;
    return 0;
}

static int get_percent(void)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, vol_name);
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
    elem_name(&v.id, vol_name);
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
    elem_name(&v.id, sw_name);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        return 1;
    return v.value.integer[0] != 0;
}

static void set_on(int on)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    elem_name(&v.id, sw_name);
    for (unsigned k = 0; k < vcount; k++)
        v.value.integer[k] = on ? 1 : 0;
    ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

/* What new programs play: ~/.asoundrc, or card 0 device 0. */
static void read_default_out(int *card, int *dev)
{
    *card = 0;
    *dev = 0;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return;
    char path[256], line[160];
    snprintf(path, sizeof path, "%s/.asoundrc", home);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int c, d;
        if (sscanf(line, " slave.pcm \"hw:%d,%d\"", &c, &d) == 2 ||
            sscanf(line, "slave.pcm \"hw:%d,%d\"", &c, &d) == 2) {
            *card = c;
            *dev = d;
        }
    }
    fclose(f);
}

static void write_default_out(int card, int dev)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return;
    char path[256], tmp[280];
    snprintf(path, sizeof path, "%s/.asoundrc", home);
    snprintf(tmp, sizeof tmp, "%s/.asoundrc.tmp", home);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f,
            "# Written by myos-volume when an output is chosen.\n"
            "pcm.!default {\n"
            "    type plug\n"
            "    slave.pcm \"hw:%d,%d\"\n"
            "}\n"
            "ctl.!default {\n"
            "    type hw\n"
            "    card %d\n"
            "}\n",
            card, dev, card);
    if (fclose(f) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}

static int kind_of(const char *s)
{
    if (strstr(s, "HDMI") || strstr(s, "DisplayPort") || strstr(s, " DP"))
        return 1;
    if (strstr(s, "Head") || strstr(s, "phone") || strstr(s, "Bluetooth"))
        return 2;
    return 0;
}

/* "ALC897 Analog" is the speakers. HDMI keeps its own name so two ports
   stay distinct. */
static void friendly(const char *pcm, int kind, char *out, size_t n)
{
    if (kind == 1)
        snprintf(out, n, "%s", pcm[0] ? pcm : "Display");
    else if (kind == 2)
        snprintf(out, n, "%s", pcm[0] ? pcm : "Headphones");
    else if (!pcm[0] || strstr(pcm, "Analog"))
        snprintf(out, n, "Speakers");
    else
        snprintf(out, n, "%s", pcm);
}

static void list_outputs(void)
{
    n_outs = 0;
    for (int card = 0; card < 8 && n_outs < OUT_MAX; card++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/snd/controlC%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        int dev = -1;
        while (n_outs < OUT_MAX &&
               ioctl(fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &dev) >= 0 && dev >= 0) {
            struct snd_pcm_info info;
            memset(&info, 0, sizeof info);
            info.device = (unsigned)dev;
            info.stream = SNDRV_PCM_STREAM_PLAYBACK;
            if (ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &info) < 0)
                continue;
            struct output *o = &outs[n_outs++];
            o->card = card;
            o->device = dev;
            o->kind = kind_of((const char *)info.name);
            friendly((const char *)info.name, o->kind, o->name, sizeof o->name);
        }
        close(fd);
    }
    for (int i = 0; i < n_outs; i++)
        for (int j = 0; j < i; j++)
            if (!strcmp(outs[i].name, outs[j].name)) {
                char tmp[80];
                snprintf(tmp, sizeof tmp, "%s %d", outs[i].name, i + 1);
                snprintf(outs[i].name, sizeof outs[i].name, "%s", tmp);
            }
    if (!n_outs) {
        snprintf(outs[0].name, sizeof outs[0].name, "Speakers");
        outs[0].card = ctl_card;
        outs[0].device = 0;
        outs[0].kind = 0;
        n_outs = 1;
    }
}

/* --- the pop-up ------------------------------------------------------------ */

static const char css[] =
    "window { background-color: transparent; }"
    ".title { font-size: 13px; font-weight: bold; color: #1d1d1f; }"
    ".section { font-size: 11px; color: #8e8e93; }"
    ".name { font-size: 13px; color: #1d1d1f; }"
    ".menu-row { background-color: transparent; border-radius: 6px; }"
    ".menu-row:hover { background-color: rgba(0,0,0,0.08); }"
    "separator { background-color: rgba(0,0,0,0.12); min-height: 1px; margin: 6px 10px; }"
    "scrolledwindow, viewport { background-color: #e8e8ed; }"
    "scale trough { min-height: 4px; background-color: #d1d1d6; border: none; border-radius: 2px; }"
    "scale highlight { background-color: #007aff; border-radius: 2px; }"
    "scale slider {"
    "  background-color: #ffffff; border: 1px solid #c7c7cc; border-radius: 50%;"
    "  min-width: 16px; min-height: 16px; margin: -6px;"
    "}";

static void icon_color(cairo_t *cr, int on)
{
    if (on)
        cairo_set_source_rgb(cr, 0.0, 0.48, 1.0);
    else
        cairo_set_source_rgb(cr, 0.20, 0.20, 0.22);
}

static void round_stroke(cairo_t *cr)
{
    cairo_set_line_width(cr, 1.4);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
}

/* A speaker facing right. `waves` is how many sound arcs (0 is the quiet
   one, 2 is the loud one). A slash means the switch is off. */
static gboolean draw_speaker(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int waves = GPOINTER_TO_INT(data);
    double s = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_move_to(cr, s * 0.08, s * 0.38);
    cairo_line_to(cr, s * 0.32, s * 0.38);
    cairo_line_to(cr, s * 0.55, s * 0.16);
    cairo_line_to(cr, s * 0.55, s * 0.84);
    cairo_line_to(cr, s * 0.32, s * 0.62);
    cairo_line_to(cr, s * 0.08, s * 0.62);
    cairo_close_path(cr);
    cairo_fill(cr);
    round_stroke(cr);
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    for (int i = 0; i < waves; i++) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, s * 0.5, s * 0.5, s * (0.22 + i * 0.2), -0.8, 0.8);
        cairo_stroke(cr);
    }
    if (!waves && !get_on()) {          /* the quiet speaker carries the mute mark */
        cairo_set_source_rgb(cr, 0.55, 0.12, 0.12);
        cairo_move_to(cr, s * 0.15, s * 0.2);
        cairo_line_to(cr, s * 0.85, s * 0.85);
        cairo_stroke(cr);
    }
    return TRUE;
}

static gboolean draw_device(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int i = GPOINTER_TO_INT(data);
    double s = gtk_widget_get_allocated_height(w);
    icon_color(cr, i == out_sel);
    round_stroke(cr);
    if (outs[i].kind == 1) {            /* a monitor */
        cairo_rectangle(cr, s * 0.12, s * 0.1, s * 0.76, s * 0.5);
        cairo_stroke(cr);
        cairo_move_to(cr, s * 0.5, s * 0.62);
        cairo_line_to(cr, s * 0.5, s * 0.76);
        cairo_move_to(cr, s * 0.28, s * 0.82);
        cairo_line_to(cr, s * 0.72, s * 0.82);
        cairo_stroke(cr);
    } else if (outs[i].kind == 2) {     /* headphones */
        cairo_new_sub_path(cr);
        cairo_arc(cr, s * 0.5, s * 0.46, s * 0.26, G_PI, 2 * G_PI);
        cairo_stroke(cr);
        cairo_arc(cr, s * 0.24, s * 0.58, s * 0.11, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_arc(cr, s * 0.76, s * 0.58, s * 0.11, 0, 2 * G_PI);
        cairo_stroke(cr);
    } else {                            /* a laptop */
        cairo_rectangle(cr, s * 0.22, s * 0.1, s * 0.56, s * 0.4);
        cairo_stroke(cr);
        cairo_move_to(cr, s * 0.08, s * 0.72);
        cairo_line_to(cr, s * 0.22, s * 0.52);
        cairo_line_to(cr, s * 0.78, s * 0.52);
        cairo_line_to(cr, s * 0.92, s * 0.72);
        cairo_close_path(cr);
        cairo_stroke(cr);
    }
    return TRUE;
}

static void redraw_icons(void)
{
    if (quiet)
        gtk_widget_queue_draw(quiet);
    if (loud)
        gtk_widget_queue_draw(loud);
    for (int i = 0; i < n_outs; i++)
        if (out_icon[i])
            gtk_widget_queue_draw(out_icon[i]);
}

static void choose_out(int i)
{
    if (i < 0 || i >= n_outs || i == out_sel)
        return;
    out_sel = i;
    want_card = outs[i].card;
    want_dev = outs[i].device;
    write_default_out(want_card, want_dev);
    if (want_card != ctl_card && mixer_open_card(want_card) == 0 && scale) {
        busy = 1;
        gtk_range_set_value(GTK_RANGE(scale), get_percent());
        busy = 0;
    }
    redraw_icons();
}

static gboolean out_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    if (e->button != 1)
        return FALSE;
    choose_out(GPOINTER_TO_INT(data));
    return TRUE;
}

static void scale_moved(GtkRange *r, gpointer unused)
{
    (void)unused;
    if (busy)
        return;
    set_percent((int)gtk_range_get_value(r));
    if (!get_on())                  /* moving the slider turns the sound back on */
        set_on(1);
    redraw_icons();
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

static gboolean quit_press(GtkWidget *w, GdkEventButton *e, gpointer unused)
{
    (void)w, (void)e, (void)unused;
    gtk_main_quit();
    return TRUE;
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

static GtkWidget *menu_label(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void popup(int ax, int ay)
{
    GtkCssProvider *cssp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cssp, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cssp),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    list_outputs();
    out_sel = 0;
    for (int i = 0; i < n_outs; i++)
        if (outs[i].card == want_card && outs[i].device == want_dev)
            out_sel = i;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Sound");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    menu_bubble_prepare(win);

    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(card, 280, -1);
    gtk_widget_set_margin_top(card, 10);
    gtk_widget_set_margin_bottom(card, MENU_CARET);

    GtkWidget *title = menu_label("Sound", "title");
    gtk_widget_set_margin_start(title, 14);
    gtk_widget_set_margin_end(title, 14);
    gtk_widget_set_margin_bottom(title, 6);
    gtk_box_pack_start(GTK_BOX(card), title, FALSE, FALSE, 0);

    GtkWidget *slider_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(slider_row, 12);
    gtk_widget_set_margin_end(slider_row, 12);
    gtk_widget_set_margin_bottom(slider_row, 4);
    quiet = gtk_drawing_area_new();
    loud = gtk_drawing_area_new();
    gtk_widget_set_size_request(quiet, 16, 16);
    gtk_widget_set_size_request(loud, 22, 16);
    gtk_widget_set_valign(quiet, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(loud, GTK_ALIGN_CENTER);
    g_signal_connect(quiet, "draw", G_CALLBACK(draw_speaker), GINT_TO_POINTER(0));
    g_signal_connect(loud, "draw", G_CALLBACK(draw_speaker), GINT_TO_POINTER(2));
    scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(slider_row), quiet, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(slider_row), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(slider_row), loud, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), slider_row, FALSE, FALSE, 0);

    GtkWidget *output = menu_label("Output", "section");
    gtk_widget_set_margin_start(output, 14);
    gtk_widget_set_margin_top(output, 8);
    gtk_widget_set_margin_bottom(output, 2);
    gtk_box_pack_start(GTK_BOX(card), output, FALSE, FALSE, 0);

    for (int i = 0; i < n_outs; i++) {
        GtkWidget *row = gtk_event_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "menu-row");
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 6);
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(h), 4);
        out_icon[i] = gtk_drawing_area_new();
        gtk_widget_set_size_request(out_icon[i], 18, 18);
        gtk_widget_set_valign(out_icon[i], GTK_ALIGN_CENTER);
        g_signal_connect(out_icon[i], "draw", G_CALLBACK(draw_device), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(h), out_icon[i], FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(h), menu_label(outs[i].name, "name"), TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), h);
        g_signal_connect(row, "button-press-event", G_CALLBACK(out_press), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(card), row, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *settings_row = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(settings_row), "menu-row");
    gtk_widget_set_margin_start(settings_row, 6);
    gtk_widget_set_margin_end(settings_row, 6);
    gtk_widget_set_margin_bottom(settings_row, 4);
    GtkWidget *settings = menu_label("Sound Settings\u2026", "name");
    gtk_widget_set_margin_start(settings, 8);
    gtk_widget_set_margin_top(settings, 4);
    gtk_widget_set_margin_bottom(settings, 4);
    gtk_container_add(GTK_CONTAINER(settings_row), settings);
    /* Settings has no Sound pane yet. The line is the Mac's; a click
       closes the menu, where the slider and the outputs already are. */
    g_signal_connect(settings_row, "button-press-event", G_CALLBACK(quit_press), NULL);
    gtk_box_pack_start(GTK_BOX(card), settings_row, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(win), card);

    busy = 1;
    gtk_range_set_value(GTK_RANGE(scale), get_percent());
    busy = 0;
    g_signal_connect(scale, "value-changed", G_CALLBACK(scale_moved), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(button), NULL);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(card);
    menu_place(win, card, ax, ay);
    gtk_widget_show(win);
    gtk_window_present(GTK_WINDOW(win));
    tray_follow(win, ax - menu_caret_x);
    gtk_widget_grab_focus(scale);
    g_timeout_add(50, grab_it, NULL);
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
    read_default_out(&want_card, &want_dev);
    if (mixer_open_card(want_card) < 0 && (want_card == 0 || mixer_open_card(0) < 0))
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
    } else {
        /* --at is in screen pixels; GTK places windows in its own, which
           at 200% are two screen pixels each. */
        int sf = gdk_window_get_scale_factor(gdk_get_default_root_window());
        ax /= sf, ay /= sf;
    }
    popup(ax, ay);
    return 0;
}
