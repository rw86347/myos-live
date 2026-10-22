/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-battery — the battery in the tray: how much is left, how long it
 * lasts, how worn it is, and the power-saving slider.
 *
 *   myos-battery --at X Y   the pop-up, its bottom centred above X Y
 *                           (myos-menu passes the battery icon's place)
 *   myos-battery --print    the same facts as text, for a terminal
 *
 * The facts come from /sys/class/power_supply (power_supply.h), the
 * slider is /sys/firmware/acpi/platform_profile: Linux's three platform
 * profiles, low-power, balanced and performance. The kernel switches the
 * processor's speed range, turbo and energy preference at once
 * (arch/.../cpupower.c), no restart. At boot /etc/runit/1 lets the
 * administrators (group wheel) write it; for anyone else the slider only
 * shows the setting.
 *
 * The card matches live/assets/themes/golden-hour/battery-popup.png: a
 * large percent, the time to empty or to full beside it, and a line that
 * says whether the machine is plugged in. The pop-up sits above the
 * bottom tray. It reads the files again every 5 seconds while it is open
 * (the kernel's copy changes every 10), and closes like the volume
 * pop-up: a click anywhere else, Escape, or losing the keyboard.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "power_supply.h"
#include "tray_follow.h"

#define PROFILE "/sys/firmware/acpi/platform_profile"

static const char *const profiles[] = { "low-power", "balanced", "performance" };

/* Golden Hour cream card (docs/ui-theme.md, battery-popup.png). The card
   itself, round corners and the point, is drawn by draw_bubble. */
static const char css[] =
    "window { background-color: transparent; }"
    "#battery-card { padding: 18px 20px 14px 20px; }"
    ".percent { font-size: 52px; font-weight: bold; color: #1d2430; }"
    ".pct-mark { font-size: 18px; color: #1d2430; }"
    ".time { font-size: 15px; color: #5b6573; }"
    ".status { font-size: 14px; color: #5b6573; }"
    ".health { font-size: 13px; color: #1d2430; }"
    ".cycles { font-size: 12px; color: #5b6573; }"
    ".note { font-size: 12px; color: #5b6573; }"
    "progressbar trough {"
    "  min-height: 8px; min-width: 88px;"
    "  background-color: #e3e8ee; border: none; border-radius: 4px;"
    "}"
    "progressbar progress { min-height: 8px; border-radius: 4px; background-color: #1c6b21; }"
    "progressbar.fair progress { background-color: #c96500; }"
    "progressbar.worn progress { background-color: #a24f00; }"
    "scale { min-width: 280px; }"
    "scale trough { min-height: 6px; background-color: #e3e8ee; border: none; }"
    "scale highlight { background-color: #1d3554; }"
    "scale mark indicator { background-color: #5b6573; }";

static GtkWidget *win, *percent, *pct_mark, *timeleft, *status;
static GtkWidget *health_bar, *health, *cycles, *scale, *note;
static int busy;

struct facts {
    int have;
    int plugged;
    int percent;
    char time[64];
    char status[80];
    int health;
    char health_word[32];
    int cycles;
};

/* The platform profile: 0..2, or -1 if this kernel has none. */
static int profile_get(void)
{
    char buf[32];
    FILE *f = fopen(PROFILE, "r");
    if (!f)
        return -1;
    int p = -1;
    if (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = 0;
        for (int i = 0; i < 3; i++)
            if (!strcmp(buf, profiles[i]))
                p = i;
    }
    fclose(f);
    return p;
}

/* 0, or errno. */
static int profile_set(int p)
{
    int fd = open(PROFILE, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    int n = (int)strlen(profiles[p]);
    int err = write(fd, profiles[p], (size_t)n) == n ? 0 : errno;
    close(fd);
    return err;
}

/* "2 hr 14 min", "45 min" */
static void say_time(char *out, int size, int min)
{
    if (min >= 60)
        snprintf(out, (size_t)size, "%d hr %d min", min / 60, min % 60);
    else
        snprintf(out, (size_t)size, "%d min", min);
}

static void read_facts(struct facts *f)
{
    struct power_supply p;
    memset(f, 0, sizeof *f);
    f->percent = f->health = f->cycles = -1;
    if (ps_read(PS_DIR, &p) < 0) {
        f->plugged = p.on_ac;
        snprintf(f->status, sizeof f->status, p.on_ac ? "Plugged in, no battery" : "No battery found");
        return;
    }
    f->have = 1;
    f->plugged = p.on_ac || p.charging || p.full;
    f->percent = p.percent;
    f->cycles = p.cycles;
    f->health = ps_health(&p);
    if (f->health >= 80)
        snprintf(f->health_word, sizeof f->health_word, "Good");
    else if (f->health >= 60)
        snprintf(f->health_word, sizeof f->health_word, "Fair");
    else if (f->health >= 0)
        snprintf(f->health_word, sizeof f->health_word, "Worn");

    if (p.full)
        snprintf(f->status, sizeof f->status, "Plugged in · fully charged");
    else if (p.charging)
        snprintf(f->status, sizeof f->status, "Plugged in · charging");
    else if (p.on_ac)
        snprintf(f->status, sizeof f->status, "Plugged in · not charging");
    else
        snprintf(f->status, sizeof f->status, "On battery");

    int min = ps_minutes(&p);
    if (p.full)
        f->time[0] = 0;
    else if (min < 0 && (p.charging || p.discharging))
        snprintf(f->time, sizeof f->time, "Working out the time…");
    else if (p.charging && min >= 0) {
        char span[32];
        say_time(span, sizeof span, min);
        snprintf(f->time, sizeof f->time, "%s to full", span);
    } else if (p.discharging && min >= 0) {
        char span[32];
        say_time(span, sizeof span, min);
        snprintf(f->time, sizeof f->time, "%s left", span);
    }
}

/* --- --print ---------------------------------------------------------------- */

static int print(void)
{
    struct facts f;
    int pr = profile_get();
    read_facts(&f);
    if (!f.have)
        printf("%s\n", f.status);
    else {
        if (f.percent >= 0)
            printf("%d%%\n", f.percent);
        if (f.time[0])
            printf("%s\n", f.time);
        printf("%s\n", f.status);
        if (f.health >= 0) {
            printf("Health: %s, %d%% of new", f.health_word, f.health);
            if (f.cycles > 0)
                printf(" (%d cycles)", f.cycles);
            printf("\n");
        }
    }
    if (pr >= 0)
        printf("Power mode: %s\n", profiles[pr]);
    return 0;
}

/* --- the pop-up ------------------------------------------------------------- */

static void apply_facts(const struct facts *f)
{
    char buf[96];
    if (f->percent >= 0)
        snprintf(buf, sizeof buf, "%d", f->percent);
    else
        snprintf(buf, sizeof buf, "—");
    gtk_label_set_text(GTK_LABEL(percent), buf);
    gtk_widget_set_visible(pct_mark, f->percent >= 0);
    gtk_label_set_text(GTK_LABEL(timeleft), f->time);
    gtk_widget_set_visible(timeleft, f->time[0] != 0);
    gtk_label_set_text(GTK_LABEL(status), f->status);

    if (f->health >= 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(health_bar), f->health / 100.0);
        GtkStyleContext *sc = gtk_widget_get_style_context(health_bar);
        gtk_style_context_remove_class(sc, "fair");
        gtk_style_context_remove_class(sc, "worn");
        if (f->health < 60)
            gtk_style_context_add_class(sc, "worn");
        else if (f->health < 80)
            gtk_style_context_add_class(sc, "fair");
        snprintf(buf, sizeof buf, "Health  %s  %d%%", f->health_word, f->health);
        gtk_label_set_text(GTK_LABEL(health), buf);
        gtk_widget_show(health_bar);
        gtk_widget_show(health);
    } else {
        gtk_widget_hide(health_bar);
        gtk_widget_hide(health);
    }
    if (f->cycles > 0) {
        snprintf(buf, sizeof buf, "%d cycles", f->cycles);
        gtk_label_set_text(GTK_LABEL(cycles), buf);
        gtk_widget_show(cycles);
    } else
        gtk_widget_hide(cycles);
}

static gboolean refresh(gpointer unused)
{
    (void)unused;
    struct facts f;
    read_facts(&f);
    apply_facts(&f);
    return G_SOURCE_CONTINUE;
}

static void scale_moved(GtkRange *r, gpointer unused)
{
    (void)unused;
    if (busy)
        return;
    int p = (int)(gtk_range_get_value(r) + 0.5);
    int err = profile_set(p);
    if (err) {
        gtk_label_set_text(GTK_LABEL(note), err == EACCES || err == EPERM
                           ? "Only an administrator can change the power mode."
                           : "The power mode could not be changed.");
        gtk_widget_show(note);
        busy = 1;
        int now = profile_get();
        gtk_range_set_value(r, now < 0 ? 1 : now);
        busy = 0;
    }
}

/* The slider stops only at its three marks. */
static gboolean scale_snap(GtkRange *r, GtkScrollType s, double v, gpointer unused)
{
    (void)s, (void)unused;
    gtk_range_set_value(r, v < 0.5 ? 0 : v < 1.5 ? 1 : 2);
    return TRUE;
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

static GtkWidget *label_class(const char *cls, GtkAlign align)
{
    GtkWidget *l = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    gtk_widget_set_halign(l, align);
    gtk_widget_set_valign(l, GTK_ALIGN_END);
    return l;
}

/* The card is a speech bubble: a rounded rectangle with a point at the
   bottom, under the battery icon, so it sits above the tray, not over it. */
#define RADIUS 18
#define CARET 12                    /* how far the point reaches below the card */
static int caret_x;                 /* the point, from the window's left */
static int composited;              /* a compositor runs: real transparency */

static void bubble_path(cairo_t *cr, double w, double h, double inset)
{
    double l = inset, t = inset, r = w - inset, b = h - CARET - inset, cx = caret_x;
    if (cx < RADIUS + CARET)
        cx = RADIUS + CARET;
    if (cx > w - RADIUS - CARET)
        cx = w - RADIUS - CARET;
    cairo_new_path(cr);
    cairo_arc(cr, l + RADIUS, t + RADIUS, RADIUS, G_PI, 1.5 * G_PI);
    cairo_arc(cr, r - RADIUS, t + RADIUS, RADIUS, 1.5 * G_PI, 2 * G_PI);
    cairo_arc(cr, r - RADIUS, b - RADIUS, RADIUS, 0, 0.5 * G_PI);
    cairo_line_to(cr, cx + CARET, b);
    cairo_line_to(cr, cx, h - inset);
    cairo_line_to(cr, cx - CARET, b);
    cairo_arc(cr, l + RADIUS, b - RADIUS, RADIUS, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
}

static gboolean draw_bubble(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double ww = gtk_widget_get_allocated_width(w), wh = gtk_widget_get_allocated_height(w);
    if (composited) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
    } else
        cairo_set_source_rgb(cr, 0xc9 / 255.0, 0xc2 / 255.0, 0xb2 / 255.0);
    /* Without a compositor the window is cut to the bubble (cut_shape), and
       the smoothed edge blends into the border's colour instead of black. */
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    bubble_path(cr, ww, wh, 0.5);
    cairo_set_source_rgb(cr, 0xfb / 255.0, 0xfb / 255.0, 0xf9 / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0xc9 / 255.0, 0xc2 / 255.0, 0xb2 / 255.0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return FALSE;                   /* the card's contents draw over it */
}

/* No compositor: X has no see-through windows, so outside the bubble the
   window is cut away (the shape extension) and the desktop shows. */
static void cut_shape(GtkWidget *w, GdkRectangle *a, gpointer unused)
{
    (void)unused;
    if (composited)
        return;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A1, a->width, a->height);
    cairo_t *cr = cairo_create(s);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    bubble_path(cr, a->width, a->height, 0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_region_t *r = gdk_cairo_region_create_from_surface(s);
    gtk_widget_shape_combine_region(w, r);
    cairo_region_destroy(r);
    cairo_surface_destroy(s);
}

static void popup(int ax, int ay)
{
    GtkCssProvider *cssp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cssp, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(cssp),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Battery");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    GdkScreen *scr = gtk_widget_get_screen(win);
    if (gdk_screen_is_composited(scr) && gdk_screen_get_rgba_visual(scr)) {
        gtk_widget_set_visual(win, gdk_screen_get_rgba_visual(scr));
        composited = 1;
    }
    gtk_widget_set_app_paintable(win, TRUE);
    g_signal_connect(win, "draw", G_CALLBACK(draw_bubble), NULL);
    g_signal_connect(win, "size-allocate", G_CALLBACK(cut_shape), NULL);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_name(card, "battery-card");
    gtk_widget_set_margin_bottom(card, CARET);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    percent = label_class("percent", GTK_ALIGN_START);
    pct_mark = label_class("pct-mark", GTK_ALIGN_START);
    gtk_label_set_text(GTK_LABEL(pct_mark), "%");
    timeleft = label_class("time", GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(timeleft, 8);
    gtk_box_pack_start(GTK_BOX(head), percent, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), pct_mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), timeleft, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card), head, FALSE, FALSE, 0);

    status = label_class("status", GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), status, FALSE, FALSE, 0);

    GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    health_bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(health_bar, 88, -1);
    gtk_widget_set_valign(health_bar, GTK_ALIGN_CENTER);
    health = label_class("health", GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hrow), health_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hrow), health, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), hrow, FALSE, FALSE, 0);

    cycles = label_class("cycles", GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), cycles, FALSE, FALSE, 0);

    int pr = profile_get();
    if (pr >= 0) {
        gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
        scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 2, 1);
        gtk_widget_set_size_request(scale, 280, -1);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_scale_add_mark(GTK_SCALE(scale), 0, GTK_POS_BOTTOM, "Lifespan");
        gtk_scale_add_mark(GTK_SCALE(scale), 1, GTK_POS_BOTTOM, "Balanced");
        gtk_scale_add_mark(GTK_SCALE(scale), 2, GTK_POS_BOTTOM, "Speed");
        gtk_range_set_value(GTK_RANGE(scale), pr);
        gtk_box_pack_start(GTK_BOX(card), scale, FALSE, FALSE, 0);
        note = label_class("note", GTK_ALIGN_START);
        if (access(PROFILE, W_OK) != 0)
            gtk_label_set_text(GTK_LABEL(note), "Only an administrator can change the power mode.");
        gtk_box_pack_start(GTK_BOX(card), note, FALSE, FALSE, 0);
        g_signal_connect(scale, "change-value", G_CALLBACK(scale_snap), NULL);
        g_signal_connect(scale, "value-changed", G_CALLBACK(scale_moved), NULL);
    }
    gtk_container_add(GTK_CONTAINER(win), card);

    g_signal_connect(win, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(button), NULL);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(card);
    refresh(NULL);
    if (note && !gtk_label_get_text(GTK_LABEL(note))[0])
        gtk_widget_hide(note);
    g_timeout_add_seconds(5, refresh, NULL);

    /* The point just above the icon, (ax, ay): the icon's middle, the
       tray's top. The card stays on the screen, the point on the icon. */
    GtkRequisition req;
    gtk_widget_get_preferred_size(card, NULL, &req);
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (m)
        gdk_monitor_get_geometry(m, &mon);
    int x = ax - req.width / 2, y = ay - req.height - 2;    /* (req has the point's margin) */
    if (x + req.width > mon.x + mon.width - 4)
        x = mon.x + mon.width - 4 - req.width;
    if (x < mon.x + 4)
        x = mon.x + 4;
    if (y < mon.y + 4)
        y = mon.y + 4;
    caret_x = ax - x;
    gtk_window_move(GTK_WINDOW(win), x, y);
    gtk_widget_show(card);
    gtk_widget_show(win);
    gtk_window_present(GTK_WINDOW(win));
    tray_follow(win, x);
    if (scale)
        gtk_widget_grab_focus(scale);
    g_timeout_add(50, grab_it, NULL);
    gtk_main();
}

int main(int argc, char **argv)
{
    int ax = -1, ay = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--at") && i + 2 < argc) {
            ax = atoi(argv[++i]);
            ay = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--print"))
            return print();
        else {
            fprintf(stderr, "usage: myos-battery [--at X Y | --print]\n");
            return 2;
        }
    }
    gtk_init(&argc, &argv);
    if (ax < 0 || ay < 0) {
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
