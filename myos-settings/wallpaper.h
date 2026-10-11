/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * wallpaper.h — the desktop picture, shared by Settings and myos-desktop.
 *
 * The choice is one file per user, ~/.config/myos/desktop.conf:
 *   image=/path/to/picture     (empty when a flat colour is chosen)
 *   color=#rrggbb              (empty when a picture is chosen)
 *   mode=fill|fit|stretch|center|tile
 * With no file, the picture is the theme's wallpaper, filled to the screen
 * the way the Mac's "Fill Screen" does: scaled until it covers, then cropped.
 *
 * Include this after gtk/gtk.h.
 */
#ifndef MYOS_WALLPAPER_H
#define MYOS_WALLPAPER_H

#include <ctype.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>

#define WALL_THEME "/usr/share/myos/theme/wallpaper.jpg"
/* Extra pictures shipped with the system, listed under Desktop Pictures. */
#define WALL_SHARE "/usr/share/myos/wallpapers"

enum { WALL_FILL, WALL_FIT, WALL_STRETCH, WALL_CENTER, WALL_TILE, WALL_NMODE };

static const char *const wall_modes[WALL_NMODE] = {"fill", "fit", "stretch", "center", "tile"};
/* The names on the Mac's Wallpaper pane, in the same order. */
static const char *const wall_labels[WALL_NMODE] = {
    "Fill Screen", "Fit to Screen", "Stretch to Fill Screen", "Center", "Tile"};

struct wall {
    char image[PATH_MAX];
    char color[8]; /* "#rrggbb", or empty */
    int mode;
};

static const char *wall_home(void)
{
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}

/* Make every directory along path (a directory, not a file). */
static void wall_mkdir(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void wall_conf_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.config/myos/desktop.conf", wall_home());
}

static int wall_mode_of(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    for (int i = 0; i < WALL_NMODE; i++)
        if (!strncmp(s, wall_modes[i], strlen(wall_modes[i])))
            return i;
    return WALL_FILL;
}

static void wall_read(struct wall *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->image, sizeof c->image, "%s", WALL_THEME);
    char path[PATH_MAX], line[PATH_MAX + 32];
    wall_conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    int saw_image = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "image=", 6)) {
            saw_image = 1;
            snprintf(c->image, sizeof c->image, "%s", line + 6);
            c->image[strcspn(c->image, "\r\n")] = 0;
        } else if (!strncmp(line, "color=", 6)) {
            snprintf(c->color, sizeof c->color, "%s", line + 6);
            c->color[strcspn(c->color, "\r\n")] = 0;
        } else if (!strncmp(line, "mode=", 5))
            c->mode = wall_mode_of(line + 5);
    }
    fclose(f);
    if (saw_image && !c->image[0])
        c->image[0] = 0; /* a colour was chosen; don't put the theme back */
    if (!saw_image && !c->color[0])
        snprintf(c->image, sizeof c->image, "%s", WALL_THEME);
}

static int wall_write(const struct wall *c) __attribute__((unused));
static int wall_write(const struct wall *c)
{
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.config/myos", wall_home());
    wall_mkdir(dir);
    wall_conf_path(path, sizeof path);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "image=%s\ncolor=%s\nmode=%s\n", c->image, c->color,
            wall_modes[c->mode >= 0 && c->mode < WALL_NMODE ? c->mode : 0]);
    if (fclose(f))
        return -1;
    return rename(tmp, path);
}

/* Paint the choice into the rectangle, as it will look on a screen of
   that size. Black shows through where Fit or Center doesn't cover. */
static void wall_paint(cairo_t *cr, double x, double y, double w, double h, const struct wall *c)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (c->color[0] == '#') {
        unsigned rgb = (unsigned)strtoul(c->color + 1, NULL, 16);
        cairo_set_source_rgb(cr, (rgb >> 16 & 255) / 255.0, (rgb >> 8 & 255) / 255.0, (rgb & 255) / 255.0);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    const char *path = c->image[0] ? c->image : WALL_THEME;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
    if (!pb) {
        cairo_restore(cr);
        return;
    }
    double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
    if (iw < 1 || ih < 1) {
        g_object_unref(pb);
        cairo_restore(cr);
        return;
    }
    double sx = w / iw, sy = h / ih;
    if (c->mode == WALL_TILE) {
        cairo_translate(cr, x, y);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_paint(cr);
    } else if (c->mode == WALL_STRETCH) {
        cairo_translate(cr, x, y);
        cairo_scale(cr, sx, sy);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
    } else {
        double s = c->mode == WALL_FIT ? (sx < sy ? sx : sy) : c->mode == WALL_CENTER ? 1 : (sx > sy ? sx : sy);
        cairo_translate(cr, x + (w - iw * s) / 2, y + (h - ih * s) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
    }
    g_object_unref(pb);
    cairo_restore(cr);
}

/* The choice rendered at sw x sh pixels, for the root window. */
static GdkPixbuf *wall_render(int sw, int sh, const struct wall *c) __attribute__((unused));
static GdkPixbuf *wall_render(int sw, int sh, const struct wall *c)
{
    if (sw < 1 || sh < 1)
        return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
    cairo_t *cr = cairo_create(s);
    wall_paint(cr, 0, 0, sw, sh, c);
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, sw, sh);
    cairo_surface_destroy(s);
    return pb;
}

#endif
