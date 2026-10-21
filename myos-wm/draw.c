/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * draw.c — the pictures myos-wm draws itself: the hot corner's curl, title
 * bars, Mission Control's labels and buttons, the wallpaper, and the small
 * copies of windows Mission Control zooms from.
 *
 * Every picture is a struct pixbuf: plain memory, ARGB premultiplied (what
 * cairo calls ARGB32 and DRM calls ARGB8888, on a little-endian CPU). The
 * pixman renderer reads it in place, so redrawing a picture and calling
 * pixbuf_damage() copies nothing. Colors and sizes are the Golden Hour
 * theme's (docs/ui-theme.md, made by live/assets/themes/golden-hour/make-theme.py).
 */
#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "wm.h"

#define S(x) ((int)((x) * srv.scale + 0.5))
#define FONT_TITLE "Trebuchet MS, Verdana, sans-serif"
#define FONT_UI    "Verdana, DejaVu Sans, sans-serif"

/* --- our buffers ------------------------------------------------------------------ */

struct pixbuf_priv {
    struct wlr_buffer base;
    struct pixbuf pub;
};

static void pb_destroy(struct wlr_buffer *b)
{
    struct pixbuf_priv *p = wl_container_of(b, p, base);
    free(p->pub.data);
    free(p);
}

static bool pb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format,
                     size_t *stride)
{
    (void)flags;
    struct pixbuf_priv *p = wl_container_of(b, p, base);
    *data = p->pub.data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)p->pub.stride;
    return true;
}

static void pb_end(struct wlr_buffer *b)
{
    (void)b;
}

static const struct wlr_buffer_impl pb_impl = {
    .destroy = pb_destroy,
    .begin_data_ptr_access = pb_begin,
    .end_data_ptr_access = pb_end,
};

struct pixbuf *pixbuf_new(int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    struct pixbuf_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->pub.data = calloc((size_t)w * h, 4);
    if (!p->pub.data) {
        free(p);
        return NULL;
    }
    p->pub.w = w;
    p->pub.h = h;
    p->pub.stride = w * 4;
    p->pub.base = &p->base;
    wlr_buffer_init(&p->base, &pb_impl, w, h);
    return &p->pub;
}

/* Let go of it: it is freed once no scene node shows it any more. */
void pixbuf_drop(struct pixbuf *p)
{
    if (p)
        wlr_buffer_drop(p->base);
}

/* The pixels changed: have the scene draw them again. */
void pixbuf_damage(struct wlr_scene_buffer *sb, struct pixbuf *p)
{
    wlr_scene_buffer_set_buffer(sb, p ? p->base : NULL);
}

static cairo_t *begin(struct pixbuf *p)
{
    memset(p->data, 0, (size_t)p->stride * p->h);
    cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)p->data,
                                                             CAIRO_FORMAT_ARGB32, p->w, p->h,
                                                             p->stride);
    cairo_t *cr = cairo_create(s);
    cairo_surface_destroy(s);       /* cr holds it */
    cairo_scale(cr, srv.scale, srv.scale);
    return cr;
}

static void end(cairo_t *cr)
{
    cairo_surface_flush(cairo_get_target(cr));
    cairo_destroy(cr);
}

static void rgb(cairo_t *cr, uint32_t c, double a)
{
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
                          (c & 255) / 255.0, a);
}

static void stop(cairo_pattern_t *g, double at, uint32_t c, double a)
{
    cairo_pattern_add_color_stop_rgba(g, at, ((c >> 16) & 255) / 255.0,
                                      ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

static void round_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* SVG's quadratic curve, as cairo's cubic one. */
static void quad_to(cairo_t *cr, double qx, double qy, double x, double y)
{
    double x0, y0;
    cairo_get_current_point(cr, &x0, &y0);
    cairo_curve_to(cr, x0 + 2.0 / 3 * (qx - x0), y0 + 2.0 / 3 * (qy - y0),
                   x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y), x, y);
}

/* --- text ------------------------------------------------------------------------- */

static PangoLayout *text_layout(cairo_t *cr, const char *font, double px, bool bold,
                                const char *s, int max_w)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *d = pango_font_description_from_string(font);
    pango_font_description_set_absolute_size(d, px * PANGO_SCALE);
    pango_font_description_set_weight(d, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    if (max_w > 0) {
        pango_layout_set_width(l, max_w * PANGO_SCALE);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    pango_layout_set_single_paragraph_mode(l, TRUE);
    pango_layout_set_text(l, s, -1);
    return l;
}

/* The text's size in logical pixels, measured on a scratch surface. */
static void text_size(const char *font, double px, bool bold, const char *s, int *w, int *h)
{
    static cairo_surface_t *scratch;
    if (!scratch)
        scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);
    PangoLayout *l = text_layout(cr, font, px, bold, s, 0);
    pango_layout_get_pixel_size(l, w, h);
    g_object_unref(l);
    cairo_destroy(cr);
}

/* --- icons ------------------------------------------------------------------------ */
/* A program's icon, found the way a taskbar finds it: its .desktop file's
   Icon=, then the theme's pictures, hicolor and pixmaps. Kept in a small
   table, so each is read from disk once. */

#define ICONS 32
static struct {
    char app[64];
    int size;
    cairo_surface_t *s;             /* NULL: looked for, not found */
} icons[ICONS];
static int icon_next;

static cairo_surface_t *surface_from_gdk(GdkPixbuf *pb)
{
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
    const guchar *px = gdk_pixbuf_read_pixels(pb);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    uint32_t *out = (uint32_t *)cairo_image_surface_get_data(s);
    int ostride = cairo_image_surface_get_stride(s) / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const guchar *p = px + y * rs + x * n;
            uint32_t a = n == 4 ? p[3] : 255;
            out[y * ostride + x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 |
                                   (p[2] * a / 255);
        }
    cairo_surface_mark_dirty(s);
    return s;
}

static GdkPixbuf *try_load(const char *path, int size)
{
    if (access(path, R_OK) < 0)
        return NULL;
    return gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
}

static void icon_name_for(const char *app, char *name, size_t len)
{
    snprintf(name, len, "%s", app);
    char path[256], line[512];
    snprintf(path, sizeof path, "/usr/share/applications/%s.desktop", app);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "Icon=", 5) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(name, len, "%.200s", line + 5);
            break;
        }
    fclose(f);
}

static cairo_surface_t *icon(const char *app, int size)
{
    if (!app || !*app)
        return NULL;
    for (int i = 0; i < ICONS; i++)
        if (icons[i].size == size && strcmp(icons[i].app, app) == 0)
            return icons[i].s;
    char name[256], path[512];
    icon_name_for(app, name, sizeof name);
    GdkPixbuf *pb = NULL;
    if (name[0] == '/') {
        pb = try_load(name, size);
    } else {
        static const char *const dirs[] = {
            "/usr/share/myos/theme/icons/png/48/%s.png",
            "/usr/share/icons/hicolor/48x48/apps/%s.png",
            "/usr/share/icons/hicolor/scalable/apps/%s.svg",
            "/usr/share/icons/hicolor/64x64/apps/%s.png",
            "/usr/share/icons/hicolor/32x32/apps/%s.png",
            "/usr/share/pixmaps/%s.png",
            "/usr/share/pixmaps/%s.svg",
            "/usr/share/pixmaps/%s.xpm",
        };
        for (size_t i = 0; !pb && i < sizeof dirs / sizeof *dirs; i++) {
            snprintf(path, sizeof path, dirs[i], name);
            pb = try_load(path, size);
        }
    }
    int slot = icon_next++ % ICONS;
    if (icons[slot].s)
        cairo_surface_destroy(icons[slot].s);
    snprintf(icons[slot].app, sizeof icons[slot].app, "%s", app);
    icons[slot].size = size;
    icons[slot].s = pb ? surface_from_gdk(pb) : NULL;
    if (pb)
        g_object_unref(pb);
    return icons[slot].s;
}

/* The icon, or a plain window shape when the program has none. */
static void draw_icon(cairo_t *cr, const char *app, double x, double y, double size, double alpha)
{
    cairo_surface_t *s = icon(app, (int)(size * srv.scale + 0.5));
    if (s) {
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, 1 / srv.scale, 1 / srv.scale);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint_with_alpha(cr, alpha);
        cairo_restore(cr);
        return;
    }
    round_rect(cr, x + 1, y + 2, size - 2, size - 4, 2);
    rgb(cr, 0xfbfbf9, alpha);
    cairo_fill_preserve(cr);
    rgb(cr, 0x1f6d72, alpha);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + 1, y + 2, size - 2, size / 4);
    rgb(cr, 0x1f7f86, alpha);
    cairo_fill(cr);
}

/* --- the hot corner --------------------------------------------------------------- */
/* The wallpaper's top-right corner, folded back over the line from (x-s, 0)
   to (x, s) with a rounded tip; under it shows the fill. The picture is
   CURL_MAX + 4 square, the corner at its top right, so the flap's shadow
   fits. One picture per size from 24 to 40 px (the 120 ms growth is a
   pick from these, not a redraw), and the resting one. */

static void draw_curl(struct pixbuf *p, int s, bool hover)
{
    cairo_t *cr = begin(p);
    double x = CURL_MAX + 4;
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, s);
    stop(g, 0, hover ? 0x1f7f86 : 0x0f5560, 1);
    stop(g, 1, 0x0e2c52, 1);
    cairo_move_to(cr, x - s, 0);
    cairo_line_to(cr, x, 0);
    cairo_line_to(cr, x, s);
    cairo_close_path(cr);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    /* The flap's shadow: -1/1.5 px, blur 1.5, black 35% — three soft copies. */
    static const double off[3][2] = {{-1, 1.5}, {-0.4, 1}, {-1.6, 2}};
    for (int k = 0; k < 4; k++) {
        cairo_save(cr);
        if (k < 3)
            cairo_translate(cr, off[k][0], off[k][1]);
        cairo_move_to(cr, x - s, 0);
        cairo_line_to(cr, x, s);
        quad_to(cr, x - s * 0.62, s * 1.02, x - s * 0.9, s * 0.9);
        quad_to(cr, x - s * 1.02, s * 0.62, x - s, 0);
        cairo_close_path(cr);
        if (k < 3) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.13);
            cairo_fill(cr);
        } else {
            g = cairo_pattern_create_linear(0, 0, 0, s);
            stop(g, 0, hover ? 0xffffff : 0xfffaf0, 1);
            stop(g, 1, hover ? 0xe3d3b0 : 0xd8c7a4, 1);
            cairo_set_source(cr, g);
            cairo_fill_preserve(cr);
            cairo_pattern_destroy(g);
            rgb(cr, 0xb9a37a, 1);
            cairo_set_line_width(cr, 0.8);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }
    /* Lit along the fold. */
    cairo_move_to(cr, x - s + 1, 0.5);
    cairo_line_to(cr, x - 0.5, s - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    end(cr);
}

void draw_curl_pics(void)
{
    int box = S(CURL_MAX + 4);
    for (int s = CURL_MIN; s <= CURL_MAX + 1; s++) {
        int i = s - CURL_MIN;
        pixbuf_drop(srv.curl_pics[i]);
        srv.curl_pics[i] = pixbuf_new(box, box);
        if (srv.curl_pics[i])
            draw_curl(srv.curl_pics[i], s <= CURL_MAX ? s : CURL_MIN, s <= CURL_MAX);
    }
}

/* --- title bars ------------------------------------------------------------------- */

struct ball {
    uint32_t c[3];                  /* top, middle (at mid), bottom */
    double mid;
    uint32_t rim, glyph;
};

/* [button: 0 min/max, 1 close][state: normal, hover, pressed, inactive] */
static const struct ball balls[2][4] = {
    {{{0xd6f6f7, 0, 0x5cc3cb}, 0, 0x2b8a92, 0xffffff},
     {{0xeffdfd, 0, 0x7fd8de}, 0, 0x2b8a92, 0xffffff},
     {{0x5cc3cb, 0, 0x3c9fa7}, 0, 0x1f6d72, 0xe8fbfb},
     {{0xe6eff0, 0, 0xa9c6c9}, 0, 0x8aa9ac, 0xf7fafa}},
    {{{0xffc56b, 0xf08a12, 0xc96500}, 0.55, 0xa24f00, 0xffffff},
     {{0xffd994, 0xff9e30, 0xdb7600}, 0.55, 0xa24f00, 0xffffff},
     {{0xf08a12, 0xc96500, 0x934800}, 0.55, 0x7a3a00, 0xffe9cc},
     {{0xf6e0c6, 0xe6b98a, 0xcf9c6a}, 0.55, 0xb8916a, 0xfffaf3}},
};

static void shine(cairo_t *cr, double x, double y, double w, double h, double alpha, bool ellipse)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
    stop(g, 0, 0xffffff, 0.75);
    stop(g, 1, 0xffffff, 0);
    if (ellipse) {
        cairo_save(cr);
        cairo_translate(cr, x + w / 2, y + h / 2);
        cairo_scale(cr, w / 2, h / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
    } else {
        round_rect(cr, x, y, w, h, h / 2);
    }
    cairo_set_source(cr, g);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_pattern_destroy(g);
}

/* One round button, 48 units drawn at size (the theme's draw_ball). */
static void draw_ball(cairo_t *cr, int which, double x, double y, double size, int state)
{
    const struct ball *b = &balls[which == H_CLOSE][state];
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / 48, size / 48);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 4, 0, 44);
    stop(g, 0, b->c[0], 1);
    if (b->mid > 0)
        stop(g, b->mid, b->c[1], 1);
    stop(g, 1, b->c[2], 1);
    cairo_arc(cr, 24, 24, 20, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    rgb(cr, b->rim, 1);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
    shine(cr, 10, 6, 28, 18, 0.8, true);
    rgb(cr, b->glyph, 1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (which == H_MIN) {
        cairo_set_line_width(cr, 5);
        cairo_move_to(cr, 15, 31);
        cairo_line_to(cr, 33, 31);
        cairo_stroke(cr);
    } else if (which == H_MAX) {
        cairo_set_line_width(cr, 4);
        round_rect(cr, 15, 15, 18, 18, 2);
        cairo_stroke(cr);
    } else {
        cairo_set_line_width(cr, 5);
        cairo_move_to(cr, 16, 16);
        cairo_line_to(cr, 32, 32);
        cairo_move_to(cr, 32, 16);
        cairo_line_to(cr, 16, 32);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* The title bar: TITLE_H high, as wide as the window plus its frame. Drawn
   again when the title, the focus, the width or a button's state changes. */
void draw_title(struct view *v)
{
    int lw = v->w + 2 * FRAME_W;
    if (!v->title_pic || v->title_pic->w != S(lw) || v->title_pic->h != S(TITLE_H)) {
        pixbuf_drop(v->title_pic);
        v->title_pic = pixbuf_new(S(lw), S(TITLE_H));
        if (!v->title_pic)
            return;
    }
    bool active = v == srv.focused;
    cairo_t *cr = begin(v->title_pic);
    double w = lw;
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, TITLE_H);
    stop(g, 0, active ? 0x2f9aa1 : 0xa9cacd, 1);
    stop(g, 0.5, active ? 0x1f7f86 : 0x8fb5b9, 1);
    stop(g, 1, active ? 0x0f5560 : 0x769ea3, 1);
    cairo_move_to(cr, 0.75, TITLE_H);
    cairo_line_to(cr, 0.75, 8.75);
    cairo_arc(cr, 8.75, 8.75, 8, M_PI, 3 * M_PI / 2);
    cairo_arc(cr, w - 8.75, 8.75, 8, 3 * M_PI / 2, 2 * M_PI);
    cairo_line_to(cr, w - 0.75, TITLE_H);
    cairo_close_path(cr);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    rgb(cr, active ? 0x0a4048 : 0x628a8f, 1);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    shine(cr, 6, 2, w - 12, 12, active ? 0.35 : 0.25, false);

    draw_icon(cr, view_app(v), 8, 6, 18, active ? 1 : 0.75);
    int text_w = (int)w - 32 - 84;
    if (text_w > 8) {
        PangoLayout *l = text_layout(cr, FONT_TITLE, 13, true, view_title(v), text_w);
        int tw, th;
        pango_layout_get_pixel_size(l, &tw, &th);
        double ty = (TITLE_H - th) / 2.0;
        if (active) {
            cairo_move_to(cr, 33, ty + 1);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
            pango_cairo_show_layout(cr, l);
        }
        cairo_move_to(cr, 32, ty);
        rgb(cr, 0xffffff, 1);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
    }
    static const int which[3] = {H_MIN, H_MAX, H_CLOSE};
    for (int k = 0; k < 3; k++) {
        int state = !active ? 3 : v->pressed_button == which[k] ? 2 : v->hover_button == which[k] ? 1 : 0;
        if (active && v->pressed_button == which[k] && v->hover_button != which[k])
            state = 0;
        draw_ball(cr, which[k], w - 78 + k * 24, 5, 21, state);
    }
    end(cr);
}

/* --- Mission Control's labels and buttons ----------------------------------------- */

/* The hovered window's name under it: the program's icon and the title,
   white on a dark rounded pill (the Mac shows the same). */
struct pixbuf *draw_label(const char *text, struct wlr_surface *icon_from, const char *app,
                          int max_w)
{
    (void)icon_from;
    int tw, th;
    text_size(FONT_UI, 13, false, text, &tw, &th);
    int icon_sz = 24, pad = 8;
    if (tw > max_w - icon_sz - 3 * pad)
        tw = max_w - icon_sz - 3 * pad;
    if (tw < 20)
        tw = 20;
    int w = pad + icon_sz + pad + tw + pad, h = 34;
    struct pixbuf *p = pixbuf_new(S(w), S(h));
    if (!p)
        return NULL;
    cairo_t *cr = begin(p);
    round_rect(cr, 0, 0, w, h, 9);
    cairo_set_source_rgba(cr, 0.05, 0.08, 0.12, 0.72);
    cairo_fill(cr);
    draw_icon(cr, app, pad, (h - icon_sz) / 2.0, icon_sz, 1);
    PangoLayout *l = text_layout(cr, FONT_UI, 13, false, text, tw);
    cairo_move_to(cr, pad + icon_sz + pad, (h - th) / 2.0);
    rgb(cr, 0xffffff, 1);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    end(cr);
    return p;
}

/* A desktop's name in the Spaces bar: a pill, lighter for the current one. */
struct pixbuf *draw_space_name(const char *name, bool current, int w, int h)
{
    struct pixbuf *p = pixbuf_new(S(w), S(h));
    if (!p)
        return NULL;
    cairo_t *cr = begin(p);
    round_rect(cr, 0.5, 0.5, w - 1, h - 1, h / 2.0);
    cairo_set_source_rgba(cr, 1, 1, 1, current ? 0.32 : 0.14);
    cairo_fill(cr);
    int tw, th;
    text_size(FONT_UI, 12, current, name, &tw, &th);
    PangoLayout *l = text_layout(cr, FONT_UI, 12, current, name, w - 12);
    pango_layout_get_pixel_size(l, &tw, &th);
    cairo_move_to(cr, (w - tw) / 2.0, (h - th) / 2.0);
    rgb(cr, 0xffffff, 1);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    end(cr);
    return p;
}

/* The Spaces bar's + (plus) and a desktop's x (not plus). */
struct pixbuf *draw_round_button(int size, bool plus)
{
    struct pixbuf *p = pixbuf_new(S(size), S(size));
    if (!p)
        return NULL;
    cairo_t *cr = begin(p);
    double c = size / 2.0;
    if (plus) {
        round_rect(cr, 0.5, 0.5, size - 1, size - 1, size / 5.0);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
        cairo_fill(cr);
    } else {
        cairo_arc(cr, c, c, c - 0.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.25, 0.25, 0.27, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    rgb(cr, 0xffffff, 1);
    cairo_set_line_width(cr, plus ? size / 10.0 : size / 8.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double r = plus ? size * 0.28 : size * 0.22;
    if (plus) {
        cairo_move_to(cr, c - r, c);
        cairo_line_to(cr, c + r, c);
        cairo_move_to(cr, c, c - r);
        cairo_line_to(cr, c, c + r);
    } else {
        cairo_move_to(cr, c - r, c - r);
        cairo_line_to(cr, c + r, c + r);
        cairo_move_to(cr, c + r, c - r);
        cairo_line_to(cr, c - r, c + r);
    }
    cairo_stroke(cr);
    end(cr);
    return p;
}

/* --- the wallpaper ---------------------------------------------------------------- */
/* The same choice the X11 desktop uses (live/myos-settings/wallpaper.h):
   ~/.config/myos/desktop.conf, else the theme's picture, filled to the
   screen the way the Mac's "Fill Screen" does. */

void draw_wallpaper(int w, int h)
{
    char image[512] = "/usr/share/myos/theme/wallpaper.jpg", color[16] = "", mode[16] = "fill";
    const char *home = getenv("HOME");
    char conf[512], line[600];
    snprintf(conf, sizeof conf, "%s/.config/myos/desktop.conf", home ? home : "/");
    FILE *f = fopen(conf, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strncmp(line, "image=", 6) == 0)
                snprintf(image, sizeof image, "%.500s", line + 6);
            else if (strncmp(line, "color=", 6) == 0)
                snprintf(color, sizeof color, "%.15s", line + 6);
            else if (strncmp(line, "mode=", 5) == 0)
                snprintf(mode, sizeof mode, "%.15s", line + 5);
        }
        fclose(f);
    }
    pixbuf_drop(srv.wall_pic);
    srv.wall_pic = pixbuf_new(S(w), S(h));
    if (!srv.wall_pic)
        return;
    cairo_t *cr = begin(srv.wall_pic);
    unsigned c = 0x0e2c52;
    if (color[0] == '#')
        sscanf(color + 1, "%x", &c);
    rgb(cr, c, 1);
    cairo_paint(cr);
    GdkPixbuf *pb = image[0] ? gdk_pixbuf_new_from_file(image, NULL) : NULL;
    if (pb) {
        cairo_surface_t *s = surface_from_gdk(pb);
        double iw = gdk_pixbuf_get_width(pb), ih = gdk_pixbuf_get_height(pb);
        g_object_unref(pb);
        cairo_save(cr);
        if (strcmp(mode, "tile") == 0) {
            cairo_pattern_t *pat = cairo_pattern_create_for_surface(s);
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
            cairo_set_source(cr, pat);
            cairo_paint(cr);
            cairo_pattern_destroy(pat);
        } else {
            double sx = w / iw, sy = h / ih;
            if (strcmp(mode, "fill") == 0)
                sx = sy = fmax(sx, sy);
            else if (strcmp(mode, "fit") == 0)
                sx = sy = fmin(sx, sy);
            else if (strcmp(mode, "center") == 0)
                sx = sy = 1;
            cairo_translate(cr, (w - iw * sx) / 2, (h - ih * sy) / 2);
            cairo_scale(cr, sx, sy);
            cairo_set_source_surface(cr, s, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
        }
        cairo_restore(cr);
        cairo_surface_destroy(s);
    }
    end(cr);
}

/* --- Mission Control's small copies ----------------------------------------------- */
/* A copy of a window at half or a quarter of its size, averaged over boxes
   of 2x2 or 4x4 pixels in one pass. Zooming from it touches a quarter (or
   a sixteenth) of the memory a full-size picture would — on an old PC with
   a small cache, that is the difference between a smooth zoom and not —
   and it doesn't shimmer the way sampling every 4th pixel would. */

void draw_scaled_copy(struct pixbuf *dst, struct wlr_buffer *src)
{
    pixman_image_t *in = NULL;
    void *data = NULL;
    bool mapped = false;
    struct wlr_client_buffer *cb = wlr_client_buffer_get(src);
    if (cb && cb->texture && wlr_texture_is_pixman(cb->texture)) {
        in = wlr_pixman_texture_get_image(cb->texture);
        if (in)
            pixman_image_ref(in);
    } else {
        uint32_t fmt;
        size_t stride;
        if (!wlr_buffer_begin_data_ptr_access(src, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt,
                                              &stride))
            return;
        mapped = true;
        if (fmt != DRM_FORMAT_ARGB8888 && fmt != DRM_FORMAT_XRGB8888) {
            wlr_buffer_end_data_ptr_access(src);
            return;
        }
        in = pixman_image_create_bits_no_clear(
            fmt == DRM_FORMAT_ARGB8888 ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8, src->width,
            src->height, data, (int)stride);
    }
    if (!in) {
        if (mapped)
            wlr_buffer_end_data_ptr_access(src);
        return;
    }
    pixman_image_t *out = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, dst->w, dst->h,
                                                            dst->data, dst->stride);
    double fx = (double)src->width / dst->w, fy = (double)src->height / dst->h;
    pixman_transform_t t;
    pixman_transform_init_scale(&t, pixman_double_to_fixed(fx), pixman_double_to_fixed(fy));
    pixman_image_set_transform(in, &t);
    int n;
    pixman_fixed_t *params = pixman_filter_create_separable_convolution(
        &n, pixman_double_to_fixed(fx), pixman_double_to_fixed(fy), PIXMAN_KERNEL_BOX,
        PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, 1, 1);
    pixman_image_set_filter(in, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n);
    free(params);
    pixman_image_composite32(PIXMAN_OP_SRC, in, NULL, out, 0, 0, 0, 0, 0, 0, dst->w, dst->h);
    pixman_image_set_transform(in, NULL);
    pixman_image_set_filter(in, PIXMAN_FILTER_NEAREST, NULL, 0);
    pixman_image_unref(out);
    pixman_image_unref(in);
    if (mapped)
        wlr_buffer_end_data_ptr_access(src);
}
