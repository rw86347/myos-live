/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-choose-file — an Open panel for shell scripts: prints the chosen
 * file's path and exits 0, or exits 1 if it was cancelled.
 *
 *   myos-choose-file [--title TITLE] [--media] [FOLDER]
 *
 * --media offers video and audio files first (myos-mplayer uses it).
 * FOLDER is where it starts; the home folder if FOLDER is missing.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

static const char *const MEDIA[] = {
    "*.mp4", "*.m4v", "*.mkv", "*.webm", "*.avi", "*.mov", "*.mpg", "*.mpeg", "*.wmv", "*.flv",
    "*.ogv", "*.ts", "*.mp3", "*.m4a", "*.aac", "*.ogg", "*.oga", "*.opus", "*.flac", "*.wav",
    "*.wma", NULL
};

int main(int argc, char **argv)
{
    const char *title = "Open", *start = NULL;
    int media = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc)
            title = argv[++i];
        else if (!strcmp(argv[i], "--media"))
            media = 1;
        else
            start = argv[i];
    }
    gtk_init(&argc, &argv);
    GtkWidget *d = gtk_file_chooser_dialog_new(title, NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (media) {
        GtkFileFilter *f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, "Video and audio");
        for (int i = 0; MEDIA[i]; i++)
            gtk_file_filter_add_pattern(f, MEDIA[i]);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
        GtkFileFilter *all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All files");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
    }
    const char *home = g_get_home_dir();
    if (start && g_file_test(start, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), start);
    else if (home)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), home);
    int ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    char *path = ok ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d)) : NULL;
    gtk_widget_destroy(d);
    if (!path)
        return 1;
    printf("%s\n", path);
    g_free(path);
    return 0;
}
