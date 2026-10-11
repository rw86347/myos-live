/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-player — Player, in the manner of the Mac's QuickTime Player: a dark
 * window that is mostly the picture, and under it the controls — volume,
 * back 15 s, play/pause, forward 15 s, full screen; the time played, a
 * bar to drag through the film, the time left.
 *
 *   myos-player [FILE]        no file: the Open panel
 *
 * mplayer does the playing. It runs in slave mode, drawing into our video
 * area (-wid, the area's X window) and taking its orders on stdin: pause,
 * seek, volume, and "get_time_pos" four times a second for the bar. Every
 * order goes with pausing_keep_force, so asking the time doesn't restart a
 * paused film. -identify at the start says how long it is and how big the
 * picture is; the window is fitted to it. A file with no picture (music)
 * shows a note and its name instead.
 *
 * Keys: Space play/pause, Left/Right 15 s, F full screen, Esc leaves it,
 * Ctrl+O another file. A file dropped on the window plays.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#define SKIP     15                 /* seconds, the Mac's ±15 */
#define POLL_MS  250

static GtkWidget *win, *video, *bar, *play_btn, *seek, *t_done, *t_left, *vol;
static char *file;                  /* what is playing (or last played) */
static GPid mp;                     /* 0: mplayer not running */
static GIOChannel *mp_in;
static guint out_watch, poll_id;
static double length, pos;
static int paused, has_video, vw, vh, dragging, full;
static gboolean setting;            /* we move the bar ourselves */

/* --- mplayer ----------------------------------------------------------------- */

static void say(const char *cmd)
{
    if (!mp_in)
        return;
    char line[512];
    snprintf(line, sizeof line, "pausing_keep_force %s\n", cmd);
    gsize n;
    g_io_channel_write_chars(mp_in, line, -1, &n, NULL);
    g_io_channel_flush(mp_in, NULL);
}

static void fmt_time(double t, char *out, size_t n)
{
    if (t < 0 || !isfinite(t))
        t = 0;
    int s = (int)t, h = s / 3600, m = s / 60 % 60;
    if (h)
        snprintf(out, n, "%d:%02d:%02d", h, m, s % 60);
    else
        snprintf(out, n, "%d:%02d", m, s % 60);
}

static void show_time(void)
{
    char a[16], b[24];
    fmt_time(pos, a, sizeof a);
    if (length > 0) {
        b[0] = '-';
        fmt_time(length - pos, b + 1, sizeof b - 1);
    } else
        snprintf(b, sizeof b, "--:--");
    gtk_label_set_text(GTK_LABEL(t_done), a);
    gtk_label_set_text(GTK_LABEL(t_left), b);
    if (!dragging && length > 0) {
        setting = TRUE;
        gtk_range_set_value(GTK_RANGE(seek), pos / length * 1000.0);
        setting = FALSE;
    }
}

static void set_play_icon(void)
{
    gtk_button_set_image(GTK_BUTTON(play_btn),
                         gtk_image_new_from_icon_name(paused || !mp ? "media-playback-start-symbolic"
                                                                    : "media-playback-pause-symbolic",
                                                      GTK_ICON_SIZE_DND));
}

/* The window fitted to the picture: at most most of the screen. */
static void fit_window(void)
{
    if (full || vw <= 0 || vh <= 0)
        return;
    GdkRectangle mon = { 0, 0, 1280, 800 };
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (m)
        gdk_monitor_get_workarea(m, &mon);
    double s = fmin(1.0, fmin(mon.width * 0.8 / vw, mon.height * 0.75 / vh));
    int bh = gtk_widget_get_allocated_height(bar);
    gtk_window_resize(GTK_WINDOW(win), MAX(480, (int)(vw * s)), (int)(vh * s) + bh);
}

static gboolean mp_output(GIOChannel *ch, GIOCondition cond, gpointer unused)
{
    (void)unused;
    if (cond & (G_IO_HUP | G_IO_ERR))
        return G_SOURCE_REMOVE;
    gchar *line = NULL;
    gsize len = 0;
    while (g_io_channel_read_line(ch, &line, &len, NULL, NULL) == G_IO_STATUS_NORMAL && line) {
        if (!strncmp(line, "ANS_TIME_POSITION=", 18))
            pos = atof(line + 18);
        else if (!strncmp(line, "ID_LENGTH=", 10))
            length = atof(line + 10);
        else if (!strncmp(line, "ANS_LENGTH=", 11))
            length = atof(line + 11);
        else if (!strncmp(line, "ID_VIDEO_WIDTH=", 15))
            vw = atoi(line + 15), has_video = 1;
        else if (!strncmp(line, "ID_VIDEO_HEIGHT=", 16)) {
            vh = atoi(line + 16);
            fit_window();
            gtk_widget_queue_draw(video);
        }
        g_free(line);
        line = NULL;
    }
    show_time();
    return G_SOURCE_CONTINUE;
}

static gboolean poll_time(gpointer unused)
{
    (void)unused;
    if (mp && !paused)
        say("get_time_pos");
    return G_SOURCE_CONTINUE;
}

static void mp_gone(GPid pid, gint status, gpointer unused)
{
    (void)status, (void)unused;
    g_spawn_close_pid(pid);
    if (pid != mp)
        return;
    mp = 0;
    if (mp_in) {
        g_io_channel_unref(mp_in);
        mp_in = NULL;
    }
    if (out_watch) {
        g_source_remove(out_watch);
        out_watch = 0;
    }
    /* The end, as QuickTime shows it: stopped, at the start, ready to play. */
    paused = 1;
    pos = 0;
    show_time();
    set_play_icon();
}

static void stop_mp(void)
{
    if (!mp)
        return;
    say("quit");
    kill(mp, SIGTERM);
    mp = 0;                         /* mp_gone ignores the old one */
    if (mp_in) {
        g_io_channel_unref(mp_in);
        mp_in = NULL;
    }
    if (out_watch) {
        g_source_remove(out_watch);
        out_watch = 0;
    }
}

static void start_mp(double from)
{
    if (!file)
        return;
    stop_mp();
    gtk_widget_realize(video);
    GdkWindow *gw = gtk_widget_get_window(video);
    gdk_window_ensure_native(gw);
    char wid[32], start[32], volume[16];
    snprintf(wid, sizeof wid, "%lu", (unsigned long)GDK_WINDOW_XID(gw));
    snprintf(start, sizeof start, "%.1f", from);
    snprintf(volume, sizeof volume, "%d", (int)gtk_range_get_value(GTK_RANGE(vol)));
    char *argv[] = {
        "mplayer", "-slave", "-quiet", "-identify", "-noconsolecontrols", "-nomouseinput",
        "-input", "nodefault-bindings:conf=/dev/null", "-wid", wid, "-vo", "xv,x11,",
        "-zoom", "-softvol", "-softvol-max", "100", "-volume", volume, "-ss", start,
        "--", file, NULL
    };
    gint in_fd, out_fd;
    GError *err = NULL;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &mp, &in_fd, &out_fd, NULL, &err)) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_CLOSE, "Can't play the file: %s", err->message);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_error_free(err);
        mp = 0;
        return;
    }
    mp_in = g_io_channel_unix_new(in_fd);
    g_io_channel_set_close_on_unref(mp_in, TRUE);
    GIOChannel *out = g_io_channel_unix_new(out_fd);
    g_io_channel_set_close_on_unref(out, TRUE);
    g_io_channel_set_flags(out, G_IO_FLAG_NONBLOCK, NULL);
    out_watch = g_io_add_watch(out, G_IO_IN | G_IO_HUP | G_IO_ERR, mp_output, NULL);
    g_io_channel_unref(out);
    g_child_watch_add(mp, mp_gone, NULL);
    paused = 0;
    set_play_icon();
}

/* --- what the controls do --------------------------------------------------------- */

static void open_file(const char *path)
{
    g_free(file);
    file = g_strdup(path);
    char *base = g_path_get_basename(path);
    gtk_window_set_title(GTK_WINDOW(win), base);
    g_free(base);
    length = pos = 0;
    has_video = vw = vh = 0;
    start_mp(0);
    gtk_widget_queue_draw(video);
}

static void play_pause(void)
{
    if (!mp) {                      /* stopped (or the end): from where the bar is */
        start_mp(pos);
        return;
    }
    if (mp_in) {
        gsize n;
        g_io_channel_write_chars(mp_in, "pause\n", -1, &n, NULL);
        g_io_channel_flush(mp_in, NULL);
    }
    paused = !paused;
    set_play_icon();
}

static void skip(int by)
{
    if (!mp) {
        pos = fmax(0, fmin(pos + by, length > 0 ? length : pos + by));
        show_time();
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "seek %d 0", by);
    say(cmd);
    say("get_time_pos");
}

static void toggle_full(void)
{
    if (full)
        gtk_window_unfullscreen(GTK_WINDOW(win));
    else
        gtk_window_fullscreen(GTK_WINDOW(win));
    full = !full;
}

static void choose(void)
{
    GtkWidget *d = gtk_file_chooser_dialog_new("Open", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Open",
                                               GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Movies and music");
    static const char *const pats[] = { "*.mp4", "*.m4v", "*.mkv", "*.webm", "*.avi", "*.mov",
                                        "*.mpg", "*.mpeg", "*.wmv", "*.flv", "*.ogv", "*.ts",
                                        "*.mp3", "*.m4a", "*.aac", "*.ogg", "*.oga", "*.opus",
                                        "*.flac", "*.wav", "*.wma", NULL };
    for (int i = 0; pats[i]; i++)
        gtk_file_filter_add_pattern(f, pats[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
    char *movies = g_build_filename(g_get_home_dir(), "Videos", NULL);
    if (g_file_test(movies, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), movies);
    g_free(movies);
    char *path = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT
                 ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)) : NULL;
    gtk_widget_destroy(d);
    if (path)
        open_file(path);
    g_free(path);
}

static void on_play(GtkButton *b, gpointer u) { (void)b, (void)u; play_pause(); }
static void on_back(GtkButton *b, gpointer u) { (void)b, (void)u; skip(-SKIP); }
static void on_fwd(GtkButton *b, gpointer u) { (void)b, (void)u; skip(SKIP); }
static void on_full(GtkButton *b, gpointer u) { (void)b, (void)u; toggle_full(); }

/* Dragging the bar: the time follows the finger; the jump happens on release. */
static gboolean seek_press(GtkWidget *w, GdkEvent *e, gpointer u)
{
    (void)w, (void)e, (void)u;
    dragging = 1;
    return FALSE;
}

static gboolean seek_release(GtkWidget *w, GdkEvent *e, gpointer u)
{
    (void)e, (void)u;
    dragging = 0;
    if (length <= 0)
        return FALSE;
    pos = gtk_range_get_value(GTK_RANGE(w)) / 1000.0 * length;
    if (mp) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "seek %.1f 2", pos);
        say(cmd);
    }
    show_time();
    return FALSE;
}

static void seek_moved(GtkRange *r, gpointer u)
{
    (void)u;
    if (setting || !dragging || length <= 0)
        return;
    char a[16];
    fmt_time(gtk_range_get_value(r) / 1000.0 * length, a, sizeof a);
    gtk_label_set_text(GTK_LABEL(t_done), a);
}

static void vol_moved(GtkRange *r, gpointer u)
{
    (void)u;
    char cmd[32];
    snprintf(cmd, sizeof cmd, "volume %d 1", (int)gtk_range_get_value(r));
    say(cmd);
}

static gboolean key(GtkWidget *w, GdkEventKey *e, gpointer u)
{
    (void)w, (void)u;
    switch (e->keyval) {
    case GDK_KEY_space:
        play_pause();
        return TRUE;
    case GDK_KEY_Left:
        skip(-SKIP);
        return TRUE;
    case GDK_KEY_Right:
        skip(SKIP);
        return TRUE;
    case GDK_KEY_f:
    case GDK_KEY_F:
        toggle_full();
        return TRUE;
    case GDK_KEY_Escape:
        if (full)
            toggle_full();
        return TRUE;
    case GDK_KEY_o:
        if (e->state & GDK_CONTROL_MASK) {
            choose();
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* No picture (music, or nothing open): a note and the name on black. */
static gboolean draw_video(GtkWidget *w, cairo_t *cr, gpointer u)
{
    (void)u;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (has_video && mp)
        return TRUE;                /* mplayer paints the picture */
    double s = fmin(W, H) / 3.0;
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    double cx = W / 2.0, cy = H / 2.0 - s * 0.15;
    /* ♪: a filled oval head and a stem with a flag */
    cairo_save(cr);
    cairo_translate(cr, cx - s * 0.18, cy + s * 0.32);
    cairo_scale(cr, s * 0.2, s * 0.14);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_fill(cr);
    cairo_rectangle(cr, cx - s * 0.02, cy - s * 0.42, s * 0.05, s * 0.75);
    cairo_fill(cr);
    cairo_move_to(cr, cx + s * 0.03, cy - s * 0.42);
    cairo_curve_to(cr, cx + s * 0.3, cy - s * 0.3, cx + s * 0.32, cy - s * 0.1, cx + s * 0.2, cy);
    cairo_curve_to(cr, cx + s * 0.25, cy - s * 0.15, cx + s * 0.15, cy - s * 0.25, cx + s * 0.03, cy - s * 0.28);
    cairo_close_path(cr);
    cairo_fill(cr);
    if (file) {
        char *base = g_path_get_basename(file);
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fmax(13, s * 0.14));
        cairo_text_extents_t te;
        cairo_text_extents(cr, base, &te);
        cairo_move_to(cr, (W - te.width) / 2 - te.x_bearing, cy + s * 0.75);
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.87);
        cairo_show_text(cr, base);
        g_free(base);
    }
    return TRUE;
}

static void dropped(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *data,
                    guint info, guint time, gpointer u)
{
    (void)w, (void)x, (void)y, (void)info, (void)u;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        char *path = g_filename_from_uri(uris[0], NULL, NULL);
        if (path)
            open_file(path);
        g_free(path);
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb, GtkIconSize size)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, size);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_can_focus(b, FALSE);     /* keys stay the window's (Space, arrows) */
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void quit(GtkWidget *w, gpointer u)
{
    (void)w, (void)u;
    stop_mp();
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    g_set_prgname("myos-player");
    g_set_application_name("Player");
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window.player { background: #000; }"
        ".bar { background: #2a2a2c; border-top: 1px solid #3a3a3c; padding: 4px 12px 8px 12px; }"
        ".bar label { color: #c8c8cc; font-feature-settings: 'tnum'; }"
        ".bar button { color: #e8e8ea; min-width: 36px; min-height: 36px; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "player");
    gtk_window_set_title(GTK_WINDOW(win), "Player");
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 420);
    gtk_window_set_icon_name(GTK_WINDOW(win), "multimedia-video-player");

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    video = gtk_drawing_area_new();
    gtk_widget_set_size_request(video, 320, 180);
    gtk_widget_set_vexpand(video, TRUE);
    g_signal_connect(video, "draw", G_CALLBACK(draw_video), NULL);
    gtk_box_pack_start(GTK_BOX(v), video, TRUE, TRUE, 0);

    /* The controls, QuickTime's two rows: volume | back, play, forward |
       full screen; then the time played, the bar, the time left. */
    bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "bar");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(left), gtk_image_new_from_icon_name("audio-volume-high-symbolic",
                                                                   GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
    vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(vol), FALSE);
    gtk_range_set_value(GTK_RANGE(vol), 80);
    gtk_widget_set_size_request(vol, 110, -1);
    gtk_widget_set_can_focus(vol, FALSE);
    g_signal_connect(vol, "value-changed", G_CALLBACK(vol_moved), NULL);
    gtk_box_pack_start(GTK_BOX(left), vol, FALSE, FALSE, 0);
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(mid), icon_button("media-seek-backward-symbolic", "Back 15 seconds (Left)",
                                                 G_CALLBACK(on_back), GTK_ICON_SIZE_LARGE_TOOLBAR),
                       FALSE, FALSE, 0);
    play_btn = icon_button("media-playback-start-symbolic", "Play / Pause (Space)", G_CALLBACK(on_play),
                           GTK_ICON_SIZE_DND);
    gtk_box_pack_start(GTK_BOX(mid), play_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), icon_button("media-seek-forward-symbolic", "Forward 15 seconds (Right)",
                                                 G_CALLBACK(on_fwd), GTK_ICON_SIZE_LARGE_TOOLBAR),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), left, FALSE, FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(top), mid);
    gtk_box_pack_end(GTK_BOX(top), icon_button("view-fullscreen-symbolic", "Full screen (F)",
                                               G_CALLBACK(on_full), GTK_ICON_SIZE_LARGE_TOOLBAR),
                     FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    t_done = gtk_label_new("0:00");
    t_left = gtk_label_new("--:--");
    gtk_label_set_width_chars(GTK_LABEL(t_done), 7);
    gtk_label_set_width_chars(GTK_LABEL(t_left), 8);
    seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1000, 1);
    gtk_scale_set_draw_value(GTK_SCALE(seek), FALSE);
    gtk_widget_set_can_focus(seek, FALSE);
    g_signal_connect(seek, "button-press-event", G_CALLBACK(seek_press), NULL);
    g_signal_connect(seek, "button-release-event", G_CALLBACK(seek_release), NULL);
    g_signal_connect(seek, "value-changed", G_CALLBACK(seek_moved), NULL);
    gtk_box_pack_start(GTK_BOX(row), t_done, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), seek, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), t_left, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), v);

    static GtkTargetEntry uri = { "text/uri-list", 0, 0 };
    gtk_drag_dest_set(win, GTK_DEST_DEFAULT_ALL, &uri, 1, GDK_ACTION_COPY);
    g_signal_connect(win, "drag-data-received", G_CALLBACK(dropped), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(key), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(quit), NULL);

    gtk_widget_show_all(win);
    show_time();
    poll_id = g_timeout_add(POLL_MS, poll_time, NULL);
    if (argc > 1)
        open_file(argv[1]);
    else
        choose();
    gtk_main();
    return 0;
}
