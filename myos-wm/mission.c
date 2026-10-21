/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * mission.c — Mission Control, as on the Mac (macOS 15): every window of
 * this desktop side by side, the Spaces bar along the top, App Exposé,
 * Quick Look, the slide between desktops, and the hot corner's curl.
 *
 * How it is drawn. Opening hides the desktop's real windows and shows a
 * copy of each ("thumb", one scene tree: the title bar and the window's
 * picture, both scene buffers sized with set_dest_size, since wlroots 0.17
 * can't scale a tree). The copies start exactly on top of the windows, so
 * nothing jumps; then they move to where layout.c puts them. The real
 * windows never move, so closing puts every window back exactly where it
 * was. The copies show the windows' own buffers, live: a playing video
 * keeps playing. Hidden windows get no frame callbacks from wlroots, so
 * this file sends them, once per frame for this desktop's windows.
 *
 * Every motion is driven by the display's frame events, not a timer: each
 * frame moves the animation by however much time really passed, so a slow
 * frame makes a bigger step instead of slowing the zoom down. Easing is
 * the Mac's: fast start, soft landing (cubic ease-out).
 *
 * Speed on old PCs. Shrinking a window to a quarter means reading pixels
 * spread over all of it, and a 1920x1200 window is 9 MiB: far more than a
 * Pentium 4's cache. So on opening each window that ends up at half size or
 * less gets a half (and maybe quarter) size copy (draw.c, one box-filter
 * pass), and the zoom draws from the nearest one. Copies of windows that
 * keep drawing are refreshed at most 12.5 times a second, and such windows
 * are held to that rate too (they get their frame callback when their copy
 * is refreshed). Opening allocates (thumbs, copies); frames allocate nothing.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "wm.h"

#define MIP_MS     80               /* copies refresh at most 12.5 times a second */
#define MINI_MS    1000             /* other desktops' miniatures: once a second */
#define QUICK_MS   200
#define GAP        24               /* between windows in Mission Control */
#define BAR_SMALL  46               /* the Spaces bar's height with names only */
#define PILL_H     26
#define DRAG_START 6                /* pixels the pointer moves before a click is a drag */

static const float ring_blue[4] = {0.04f, 0.52f, 1.0f, 1.0f};
static const float slot_ring[4] = {1, 1, 1, 0.9f};

static bool built;                  /* thumbs exist (open, opening or closing) */
static struct view *target;         /* the window to focus once closed */
static int switch_to = -1;          /* the desktop to show once closed */
static double re = 1;               /* re-layout after a window left: 0 .. 1 */
static uint32_t press_ms;
static double press_x, press_y;
static struct view *press_view;     /* pressed on this thumb, not yet a drag */
static int press_slot = -1;         /* pressed on this desktop in the bar */
static int hover_slot = -1;
static int bar_n;                   /* slots built in the bar */
static struct wlr_box pill[MAX_SPACES], mini[MAX_SPACES], add_small, add_big;
static int mini_w, mini_h, bar_big_h;
static struct view *label_for;

/* --- easing and small helpers ----------------------------------------------------- */

static double ease(double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double u = 1 - t;
    return 1 - u * u * u;
}

/* p moving toward dir: eased so it lands softly whichever way it goes. */
static double eased(double p, int dir)
{
    return dir < 0 ? 1 - ease(1 - p) : ease(p);
}

static int p_last_dir = 1, bar_last_dir = 1;

static int lerp_i(int a, int b, double t)
{
    return (int)lround(a + (b - a) * t);
}

static struct mc_rect lerp_r(struct mc_rect a, struct mc_rect b, double t)
{
    return (struct mc_rect){lerp_i(a.x, b.x, t), lerp_i(a.y, b.y, t), lerp_i(a.w, b.w, t),
                            lerp_i(a.h, b.h, t)};
}

static struct wlr_box lerp_b(struct wlr_box a, struct wlr_box b, double t)
{
    return (struct wlr_box){lerp_i(a.x, b.x, t), lerp_i(a.y, b.y, t),
                            lerp_i(a.width, b.width, t), lerp_i(a.height, b.height, t)};
}

static bool in_box(struct wlr_box b, double x, double y)
{
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

static void dest(struct wlr_scene_buffer *sb, int w, int h)
{
    wlr_scene_buffer_set_dest_size(sb, w < 1 ? 1 : w, h < 1 ? 1 : h);
}

static struct wlr_buffer *live_buffer(struct view *v)
{
    return v->surface && v->surface->buffer ? &v->surface->buffer->base : NULL;
}

static bool in_mc(struct view *v)
{
    if (v->kind != K_VIEW || !view_on_screen(v))
        return false;
    return !srv.mc.expose || strcmp(view_app(v), srv.mc.app) == 0;
}

bool mc_animating(void)
{
    return srv.mc.p_dir || srv.mc.bar_dir || srv.curl_dir || srv.slide.on ||
           (srv.mc.quick && srv.mc.quick_p < 1) || (built && re < 1);
}

void read_motion_setting(void)
{
    char path[512], line[64];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config/myos/motion.conf", home ? home : "/");
    bool on = srv.reduce_motion_auto;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "reduce=", 7) == 0)
                on = line[7] == '1' || srv.reduce_motion_auto;
        fclose(f);
    }
    srv.reduce_motion = on;
}

/* --- the hot corner --------------------------------------------------------------- */

static int curl_size(void)
{
    return CURL_MIN + (int)lround((CURL_MAX - CURL_MIN) * ease(srv.curl_p));
}

/* Only the curl itself takes clicks, not the transparent rest of its square. */
static bool curl_accepts(struct wlr_scene_buffer *sb, double *sx, double *sy)
{
    (void)sb;
    double box = CURL_MAX + 4, s = curl_size();
    return *sx <= box && *sy >= 0 && (box - *sx) + *sy <= s * 1.35;
}

void curl_hover(bool in)
{
    int dir = in ? 1 : -1;
    if ((dir > 0 && srv.curl_p >= 1) || (dir < 0 && srv.curl_p <= 0 && !srv.curl_dir))
        return;
    if (srv.curl_dir == dir)
        return;
    srv.curl_dir = dir;
    srv.curl_ms = now_ms();
    schedule_frame();
}

void curl_frame(uint32_t ms)
{
    if (!srv.curl_dir)
        return;
    double step = srv.reduce_motion ? 1 : (double)(ms - srv.curl_ms) / CURL_MS;
    srv.curl_ms = ms;
    srv.curl_p += srv.curl_dir * step;
    if (srv.curl_p >= 1 || srv.curl_p <= 0) {
        srv.curl_p = srv.curl_p >= 1 ? 1 : 0;
        srv.curl_dir = 0;
    }
    int i = srv.curl_p <= 0 && srv.curl_dir == 0 ? CURL_MAX - CURL_MIN + 1 : curl_size() - CURL_MIN;
    struct pixbuf *p = srv.curl_pics[i];
    if (p && srv.curl->buffer != p->base)
        wlr_scene_buffer_set_buffer(srv.curl, p->base);
}

/* --- the Spaces bar --------------------------------------------------------------- */

static void bar_geometry(void)
{
    struct output *o = primary_output();
    if (!o)
        return;
    struct wlr_box f = o->full;
    int n = srv.nspaces;
    mini_w = f.width * 11 / 100;
    mini_h = mini_w * f.height / (f.width ? f.width : 1);
    bar_big_h = 12 + mini_h + 6 + PILL_H + 10;
    int pw[MAX_SPACES], total = 0;
    for (int i = 0; i < n; i++) {
        pw[i] = 96;
        total += pw[i] + (i ? 10 : 0);
    }
    int x = f.x + (f.width - total) / 2;
    for (int i = 0; i < n; i++) {
        pill[i] = (struct wlr_box){x, f.y + 10, pw[i], PILL_H};
        x += pw[i] + 10;
    }
    total = n * mini_w + (n - 1) * GAP;
    x = f.x + (f.width - total) / 2;
    for (int i = 0; i < n; i++) {
        mini[i] = (struct wlr_box){x, f.y + 12, mini_w, mini_h};
        x += mini_w + GAP;
    }
    add_small = (struct wlr_box){f.x + f.width - 44, f.y + 9, 28, 28};
    add_big = (struct wlr_box){f.x + f.width - 72, f.y + 12 + (mini_h - 48) / 2, 48, 48};
}

static void bar_destroy(void)
{
    struct mc *m = &srv.mc;
    for (int i = 0; i < bar_n; i++) {
        wlr_scene_node_destroy(&m->slot[i].tree->node);
        pixbuf_drop(m->slot[i].name_pic);
        m->slot[i].tree = NULL;
        m->slot[i].name_pic = NULL;
    }
    bar_n = 0;
    hover_slot = -1;
}

/* A miniature's windows are pictures, not targets: a click there is a
   click on the desktop. */
static bool no_input(struct wlr_scene_buffer *b, double *sx, double *sy)
{
    (void)b;
    (void)sx;
    (void)sy;
    return false;
}

/* One desktop's miniature: its windows' pictures over the wallpaper. */
static void build_minis(int i)
{
    struct mc *m = &srv.mc;
    struct wlr_scene_node *c, *tmp;
    wl_list_for_each_safe(c, tmp, &m->slot[i].minis->children, link)
        wlr_scene_node_destroy(c);
    struct view *v;
    wl_list_for_each_reverse(v, &srv.views, link) {
        if (v->kind != K_VIEW || !v->mapped || v->minimized || v->space != i)
            continue;
        struct wlr_scene_buffer *b = wlr_scene_buffer_create(m->slot[i].minis, live_buffer(v));
        b->node.data = v;
        b->point_accepts_input = no_input;
    }
}

static void bar_build(void)
{
    struct mc *m = &srv.mc;
    bar_destroy();
    bar_geometry();
    if (m->expose)
        return;
    for (int i = 0; i < srv.nspaces; i++) {
        snprintf(srv.spaces[i].name, sizeof srv.spaces[i].name, "Desktop %d", i + 1);
        m->slot[i].tree = wlr_scene_tree_create(m->bar_tree);
        m->slot[i].back = wlr_scene_rect_create(m->slot[i].tree, 1, 1, slot_ring);
        m->slot[i].wall = wlr_scene_buffer_create(m->slot[i].tree, srv.wall_pic ? srv.wall_pic->base : NULL);
        m->slot[i].minis = wlr_scene_tree_create(m->slot[i].tree);
        m->slot[i].name_pic = draw_space_name(srv.spaces[i].name, i == srv.cur, pill[i].width, PILL_H);
        m->slot[i].name = wlr_scene_buffer_create(m->slot[i].tree,
                                                  m->slot[i].name_pic ? m->slot[i].name_pic->base : NULL);
        m->slot[i].close = wlr_scene_buffer_create(m->slot[i].tree, m->close_pic ? m->close_pic->base : NULL);
        m->slot[i].hit = (struct hit){.kind = K_HIT, .what = H_SPACE, .index = i};
        m->slot[i].close_hit = (struct hit){.kind = K_HIT, .what = H_SPACE_CLOSE, .index = i};
        m->slot[i].tree->node.data = &m->slot[i].hit;
        m->slot[i].close->node.data = &m->slot[i].close_hit;
        wlr_scene_node_set_enabled(&m->slot[i].close->node, false);
        build_minis(i);
    }
    bar_n = srv.nspaces;
    wlr_scene_node_raise_to_top(&m->add->node);
}

static void bar_apply(double e, double eb)
{
    struct mc *m = &srv.mc;
    struct output *o = primary_output();
    if (!o || m->expose) {
        wlr_scene_node_set_enabled(&m->bar_tree->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&m->bar_tree->node, true);
    /* The bar drops in from above the screen as Mission Control opens. */
    int drop = lerp_i(-bar_big_h, 0, e);
    wlr_scene_node_set_position(&m->bar_tree->node, 0, drop);
    double k = (double)mini_w / (o->full.width ? o->full.width : 1);
    for (int i = 0; i < bar_n; i++) {
        struct wlr_box mb = lerp_b(pill[i], mini[i], eb);
        bool big = eb > 0.01;
        wlr_scene_node_set_enabled(&m->slot[i].wall->node, big);
        wlr_scene_node_set_enabled(&m->slot[i].minis->node, big);
        wlr_scene_node_set_enabled(&m->slot[i].back->node, big && i == srv.cur);
        wlr_scene_node_set_position(&m->slot[i].back->node, mb.x - 2, mb.y - 2);
        wlr_scene_rect_set_size(m->slot[i].back, mb.width + 4, mb.height + 4);
        wlr_scene_node_set_position(&m->slot[i].wall->node, mb.x, mb.y);
        dest(m->slot[i].wall, mb.width, mb.height);
        wlr_scene_node_set_position(&m->slot[i].minis->node, mb.x, mb.y);
        double kk = k * mb.width / (mini_w ? mini_w : 1);
        struct wlr_scene_node *c;
        wl_list_for_each(c, &m->slot[i].minis->children, link) {
            struct view *v = c->data;
            struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(c);
            wlr_scene_node_set_position(c, (int)((v->x - o->full.x) * kk), (int)((v->y - o->full.y) * kk));
            dest(b, (int)(v->w * kk), (int)(v->h * kk));
        }
        /* The name: in the pill row while small, under the miniature while big. */
        int ny = lerp_i(pill[i].y, mini[i].y + mini[i].height + 6, eb);
        int nx = lerp_i(pill[i].x, mini[i].x + (mini[i].width - pill[i].width) / 2, eb);
        wlr_scene_node_set_position(&m->slot[i].name->node, nx, ny);
        dest(m->slot[i].name, pill[i].width, PILL_H);
        m->slot[i].box = mini[i];
        bool show_x = eb >= 1 && hover_slot == i && srv.nspaces > 1;
        wlr_scene_node_set_enabled(&m->slot[i].close->node, show_x);
        wlr_scene_node_set_position(&m->slot[i].close->node, mini[i].x - 10, mini[i].y - 10);
        dest(m->slot[i].close, 22, 22);
    }
    struct wlr_box ab = lerp_b(add_small, add_big, eb);
    wlr_scene_node_set_position(&m->add->node, ab.x, ab.y);
    dest(m->add, ab.width, ab.height);
}

/* --- thumbs ----------------------------------------------------------------------- */

static int side(struct view *v) { return v->ssd ? FRAME_W : 0; }
static int top(struct view *v) { return v->ssd ? TITLE_H : 0; }

/* Which part of the window's buffer is the window (not its shadow). */
static void set_body_source(struct view *v, int level)
{
    struct thumb *t = &v->mc;
    struct wlr_buffer *b = live_buffer(v);
    if (!b || !v->surface->current.width) {
        wlr_scene_buffer_set_source_box(t->body, NULL);
        return;
    }
    double f = (double)b->width / v->surface->current.width / (1 << level);
    struct wlr_box geo = {0, 0, v->w, v->h};
    if (v->type == VIEW_XDG)
        wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
    struct wlr_fbox src = {geo.x * f, geo.y * f, geo.width * f, geo.height * f};
    wlr_scene_buffer_set_source_box(t->body, &src);
}

static void use_level(struct view *v, int level)
{
    struct thumb *t = &v->mc;
    if (level && !t->mip[level - 1])
        level = 0;
    if (level == t->mip_used && t->body->buffer)
        return;
    t->mip_used = level;
    wlr_scene_buffer_set_buffer(t->body, level ? t->mip[level - 1]->base : live_buffer(v));
    set_body_source(v, level);
}

static void make_mips(struct view *v, double scale)
{
    struct thumb *t = &v->mc;
    struct wlr_buffer *b = live_buffer(v);
    if (!b)
        return;
    for (int k = 0; k < 2; k++) {
        double need = k == 0 ? 0.5 : 0.25;
        if (scale > need || b->width < 64 || b->height < 64)
            break;
        t->mip[k] = pixbuf_new(b->width >> (k + 1), b->height >> (k + 1));
        if (t->mip[k])
            draw_scaled_copy(t->mip[k], b);
    }
}

static void thumb_destroy(struct view *v)
{
    struct thumb *t = &v->mc;
    if (!t->tree)
        return;
    wlr_scene_node_destroy(&t->tree->node);
    pixbuf_drop(t->mip[0]);
    pixbuf_drop(t->mip[1]);
    memset(t, 0, sizeof *t);
}

static void thumb_create(struct view *v)
{
    struct mc *m = &srv.mc;
    struct thumb *t = &v->mc;
    t->tree = wlr_scene_tree_create(m->windows);
    t->ring = wlr_scene_rect_create(t->tree, 1, 1, ring_blue);
    wlr_scene_node_set_enabled(&t->ring->node, false);
    t->deco = wlr_scene_buffer_create(t->tree, v->ssd && v->title_pic ? v->title_pic->base : NULL);
    t->body = wlr_scene_buffer_create(t->tree, NULL);
    wlr_scene_buffer_set_filter_mode(t->body, WLR_SCALE_FILTER_BILINEAR);
    t->hit = (struct hit){.kind = K_HIT, .what = H_THUMB, .view = v};
    t->tree->node.data = &t->hit;
    t->from = (struct mc_rect){v->x - side(v), v->y - top(v), v->w + 2 * side(v),
                               v->h + top(v) + side(v)};
    t->mip_used = -1;
    use_level(v, 0);
}

static struct mc_rect shown_rect(struct view *v);

/* Lay the thumbs out (again): with the bar small and with it big. With
   animate, they glide from where they are now. */
static void relayout(bool animate)
{
    struct mc *m = &srv.mc;
    struct output *o = primary_output();
    if (!o)
        return;
    struct view *list[MC_MAX];
    struct mc_rect in[MC_MAX], out_c[MC_MAX], out_e[MC_MAX];
    int n = 0;
    struct view *v;
    wl_list_for_each_reverse(v, &srv.views, link)
        if (v->mc.tree && n < MC_MAX) {
            if (animate)
                v->mc.from_shown = shown_rect(v);
            list[n] = v;
            in[n++] = v->mc.from;
        }
    struct wlr_box u = o->usable;
    int top_c = m->expose ? 32 : BAR_SMALL + 16, top_e = m->expose ? 32 : bar_big_h + 16;
    struct mc_rect ac = {u.x + 40, u.y + top_c, u.width - 80, u.height - top_c - 32};
    struct mc_rect ae = {u.x + 40, u.y + top_e, u.width - 80, u.height - top_e - 32};
    double s = mc_layout(in, out_c, n, ac, GAP);
    mc_layout(in, out_e, n, ae, GAP);
    for (int i = 0; i < n; i++) {
        list[i]->mc.to_c = out_c[i];
        list[i]->mc.to_e = out_e[i];
        list[i]->mc.relayout = animate;
    }
    (void)s;
    if (animate) {
        re = srv.reduce_motion ? 1 : 0;
        schedule_frame();
    }
}

/* Where a thumb is drawn right now. */
static struct mc_rect shown_rect(struct view *v)
{
    struct mc *m = &srv.mc;
    struct thumb *t = &v->mc;
    double e = eased(m->p, p_last_dir), eb = eased(m->bar, bar_last_dir);
    struct mc_rect to = lerp_r(t->to_c, t->to_e, eb);
    if (t->relayout && re < 1)
        to = lerp_r(t->from_shown, to, ease(re));
    struct mc_rect r = lerp_r(t->from, to, e);
    if (m->quick == v) {
        struct output *o = primary_output();
        double q = ease(m->quick_p);
        int w = t->from.w, h = t->from.h;
        if (o) {
            double k = fmin(1.0, fmin(o->full.width * 0.85 / w, o->full.height * 0.85 / h));
            w = (int)(w * k);
            h = (int)(h * k);
            struct mc_rect big = {o->full.x + (o->full.width - w) / 2,
                                  o->full.y + (o->full.height - h) / 2, w, h};
            r = lerp_r(r, big, q);
        }
    }
    if (m->drag == v) {
        r.x = (int)(srv.cursor->x - m->drag_dx);
        r.y = (int)(srv.cursor->y - m->drag_dy);
    }
    return r;
}

static void thumb_apply(struct view *v)
{
    struct thumb *t = &v->mc;
    struct mc_rect r = shown_rect(v);
    double s = (double)r.w / (t->from.w ? t->from.w : 1);
    wlr_scene_node_set_position(&t->tree->node, r.x, r.y);
    dest(t->deco, (int)lround((v->w + 2 * side(v)) * s), (int)lround(top(v) * s));
    wlr_scene_node_set_enabled(&t->deco->node, v->ssd);
    wlr_scene_node_set_position(&t->body->node, (int)lround(side(v) * s), (int)lround(top(v) * s));
    dest(t->body, (int)lround(v->w * s), (int)lround(v->h * s));
    use_level(v, s <= 0.25 ? 2 : s <= 0.5 ? 1 : 0);
    bool ring = srv.mc.hover == v && srv.mc.p >= 1 && !srv.mc.drag;
    wlr_scene_node_set_enabled(&t->ring->node, ring);
    if (ring) {
        wlr_scene_node_set_position(&t->ring->node, -4, -4);
        wlr_scene_rect_set_size(t->ring, r.w + 8, r.h + 8);
    }
}

static void label_apply(void)
{
    struct mc *m = &srv.mc;
    struct view *v = m->p >= 1 && !m->drag && !m->quick ? m->hover : NULL;
    if (v != label_for) {
        label_for = v;
        pixbuf_drop(m->label_pic);
        m->label_pic = v ? draw_label(view_title(v), v->surface, view_app(v), 420) : NULL;
        pixbuf_damage(m->label, m->label_pic);
    }
    wlr_scene_node_set_enabled(&m->label->node, v && m->label_pic);
    if (v && m->label_pic) {
        struct mc_rect r = shown_rect(v);
        int w = (int)(m->label_pic->w / srv.scale), h = (int)(m->label_pic->h / srv.scale);
        wlr_scene_node_set_position(&m->label->node, r.x + (r.w - w) / 2, r.y + r.h + 8);
        dest(m->label, w, h);
        wlr_scene_node_raise_to_top(&m->label->node);
    }
}

static void mc_apply(void)
{
    struct mc *m = &srv.mc;
    struct output *o = primary_output();
    if (!o)
        return;
    double e = eased(m->p, p_last_dir), eb = eased(m->bar, bar_last_dir);
    float dim[4] = {0, 0, 0, (float)(0.32 * e)};
    wlr_scene_rect_set_color(m->dim, dim);
    wlr_scene_rect_set_size(m->dim, o->full.width, o->full.height);
    wlr_scene_node_set_position(&m->dim->node, o->full.x, o->full.y);
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        if (v->mc.tree)
            thumb_apply(v);
    bar_apply(e, eb);
    label_apply();
}

/* --- opening and closing ---------------------------------------------------------- */

static int mip_tick(void *data);
static int mini_tick(void *data);

static void finish(void)
{
    struct mc *m = &srv.mc;
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        thumb_destroy(v);
    bar_destroy();
    pixbuf_drop(m->label_pic);
    m->label_pic = NULL;
    label_for = NULL;
    pixbuf_damage(m->label, NULL);
    built = false;
    m->open = false;
    m->p = 0;
    m->bar = 0;
    m->p_dir = m->bar_dir = 0;
    m->hover = m->quick = m->drag = NULL;
    m->drag_space = -1;
    press_view = NULL;
    press_slot = -1;
    wl_event_source_timer_update(m->mip_timer, 0);
    wl_event_source_timer_update(m->mini_timer, 0);
    wlr_scene_node_set_enabled(&m->tree->node, false);
    wlr_scene_node_set_enabled(&srv.spaces_root->node, true);
    wlr_scene_node_set_enabled(&srv.unmanaged->node, true);
    wlr_scene_node_place_above(&srv.curl_tree->node, &srv.layer_bg->node);
    struct view *t = target;
    target = NULL;
    int sw = switch_to;
    switch_to = -1;
    if (t && t->mapped)
        focus_view(t);
    if (sw >= 0)
        space_switch(sw);
    pointer_refocus();
}

static void open_mc(bool expose)
{
    struct mc *m = &srv.mc;
    struct output *o = primary_output();
    if (!o)
        return;
    read_motion_setting();
    m->expose = expose;
    snprintf(m->app, sizeof m->app, "%s", srv.focused ? view_app(srv.focused) : "");
    if (!built) {
        struct view *v;
        wl_list_for_each_reverse(v, &srv.views, link)
            if (in_mc(v))
                thumb_create(v);
        bar_build();
        relayout(false);
        /* The small copies, made once, now. */
        wl_list_for_each(v, &srv.views, link)
            if (v->mc.tree) {
                double s = (double)v->mc.to_c.w / (v->mc.from.w ? v->mc.from.w : 1);
                make_mips(v, s);
            }
        m->p = 0;
        m->bar = 0;
        built = true;
    }
    m->open = true;
    m->p_dir = 1;
    p_last_dir = 1;
    /* Every desktop's miniature is up the whole time Mission Control is:
       the bar grows with the zoom (not with App Expose, which has no bar). */
    m->bar_dir = expose ? (m->bar > 0 ? -1 : 0) : 1;
    if (m->bar_dir)
        bar_last_dir = m->bar_dir;
    m->last_ms = now_ms();
    m->frames_over = 0;
    if (srv.reduce_motion) {
        m->p = 1;
        m->p_dir = 0;
        m->bar = expose ? 0 : 1;
        m->bar_dir = 0;
    }
    wlr_scene_node_set_enabled(&m->tree->node, true);
    wlr_scene_node_set_enabled(&srv.spaces_root->node, false);
    wlr_scene_node_set_enabled(&srv.unmanaged->node, false);
    wlr_scene_node_place_above(&srv.curl_tree->node, &m->tree->node);
    wlr_seat_pointer_clear_focus(srv.seat);
    wl_event_source_timer_update(m->mip_timer, MIP_MS);
    wl_event_source_timer_update(m->mini_timer, MINI_MS);
    mc_apply();
    schedule_frame();
}

static void close_mc(struct view *focus)
{
    struct mc *m = &srv.mc;
    if (!built)
        return;
    target = focus;
    m->open = false;
    m->quick = NULL;
    m->hover = NULL;
    m->drag = NULL;
    if (focus && focus->mc.tree)
        wlr_scene_node_raise_to_top(&focus->mc.tree->node);
    m->p_dir = -1;
    p_last_dir = -1;
    m->bar_dir = m->bar > 0 ? -1 : 0;
    if (m->bar_dir)
        bar_last_dir = -1;
    m->last_ms = now_ms();
    m->frames_over = 0;
    if (srv.reduce_motion) {
        finish();
        return;
    }
    schedule_frame();
}

void mc_toggle(bool expose)
{
    if (srv.mc.open && srv.mc.expose == expose)
        close_mc(NULL);
    else if (srv.mc.open)
        close_mc(NULL);
    else
        open_mc(expose);
}

/* --- the frame -------------------------------------------------------------------- */

static void slide_frame(uint32_t ms);

void mc_frame(uint32_t ms)
{
    struct mc *m = &srv.mc;
    slide_frame(ms);
    if (!built)
        return;
    double dt = (double)(ms - m->last_ms);
    m->last_ms = ms;
    if (m->p_dir) {
        m->p += m->p_dir * dt / MC_MS;
        if (m->p >= 1) {
            m->p = 1;
            m->p_dir = 0;
        } else if (m->p <= 0) {
            m->p = 0;
            m->p_dir = 0;
            finish();
            return;
        }
    }
    if (m->bar_dir) {
        m->bar += srv.reduce_motion ? m->bar_dir : m->bar_dir * dt / BAR_MS;
        if (m->bar >= 1 || m->bar <= 0) {
            m->bar = m->bar >= 1 ? 1 : 0;
            m->bar_dir = 0;
        }
    }
    if (m->quick && m->quick_p < 1)
        m->quick_p = srv.reduce_motion ? 1 : fmin(1, m->quick_p + dt / QUICK_MS);
    if (re < 1)
        re = fmin(1, re + dt / MC_MS);
    mc_apply();
    /* This desktop's windows draw on: the ones shown from their own
       buffer every frame, the ones shown from a copy when it is refreshed. */
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        if (v->mc.tree && v->mc.mip_used <= 0)
            view_send_frame(v);
}

/* A copy is out of date: refresh it (at most every MIP_MS). */
static int mip_tick(void *data)
{
    (void)data;
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        struct thumb *t = &v->mc;
        if (!t->tree || t->mip_used <= 0 || !t->mip_stale)
            continue;
        struct wlr_buffer *b = live_buffer(v);
        struct pixbuf *p = t->mip[t->mip_used - 1];
        if (b && p && (b->width >> t->mip_used) == p->w && (b->height >> t->mip_used) == p->h) {
            draw_scaled_copy(p, b);
            pixbuf_damage(t->body, p);
        }
        t->mip_stale = false;
        view_send_frame(v);
    }
    wl_event_source_timer_update(srv.mc.mip_timer, MIP_MS);
    return 0;
}

/* The other desktops' miniatures: a new picture once a second, and only
   while the bar shows them. */
static int mini_tick(void *data)
{
    (void)data;
    struct mc *m = &srv.mc;
    if (m->bar > 0) {
        struct view *v;
        wl_list_for_each(v, &srv.views, link)
            if (v->kind == K_VIEW && v->mapped && v->space != srv.cur)
                view_send_frame(v);
        for (int i = 0; i < bar_n; i++) {
            struct wlr_scene_node *c;
            wl_list_for_each(c, &m->slot[i].minis->children, link) {
                struct view *mv = c->data;
                wlr_scene_buffer_set_buffer(wlr_scene_buffer_from_node(c), live_buffer(mv));
            }
        }
        schedule_frame();
    }
    wl_event_source_timer_update(m->mini_timer, MINI_MS);
    return 0;
}

void mc_view_commit(struct view *v)
{
    struct thumb *t = &v->mc;
    if (t->tree) {
        if (t->mip_used <= 0) {
            wlr_scene_buffer_set_buffer(t->body, live_buffer(v));
            set_body_source(v, 0);
        }
        t->mip_stale = true;
    }
}

void mc_view_gone(struct view *v)
{
    struct mc *m = &srv.mc;
    if (m->hover == v)
        m->hover = NULL;
    if (m->quick == v)
        m->quick = NULL;
    if (m->drag == v)
        m->drag = NULL;
    if (press_view == v)
        press_view = NULL;
    if (target == v)
        target = NULL;
    if (label_for == v) {
        label_for = NULL;
        pixbuf_drop(m->label_pic);
        m->label_pic = NULL;
        pixbuf_damage(m->label, NULL);
    }
    if (!v->mc.tree)
        return;
    thumb_destroy(v);
    for (int i = 0; i < bar_n; i++) {
        struct wlr_scene_node *c, *tmp;
        wl_list_for_each_safe(c, tmp, &m->slot[i].minis->children, link)
            if (c->data == v)
                wlr_scene_node_destroy(c);
    }
    relayout(true);
}

/* --- the pointer and keys in Mission Control -------------------------------------- */

static struct view *thumb_at(double x, double y)
{
    struct wlr_surface *s;
    double sx, sy;
    enum kind *k = owner_at(x, y, &s, &sx, &sy);
    if (k && *k == K_HIT && ((struct hit *)k)->what == H_THUMB)
        return ((struct hit *)k)->view;
    return NULL;
}

static int slot_at(double x, double y)
{
    if (srv.mc.bar < 1)
        return -1;
    int drop = 0;
    for (int i = 0; i < bar_n; i++) {
        struct wlr_box b = mini[i];
        b.y += drop;
        if (in_box(b, x, y))
            return i;
    }
    return -1;
}

static bool on_add(double x, double y)
{
    return !srv.mc.expose && in_box(srv.mc.bar >= 1 ? add_big : add_small, x, y);
}

bool mc_motion(double x, double y)
{
    struct mc *m = &srv.mc;
    if (!built)
        return false;
    if (!m->open)
        return true;
    if (press_view && !m->drag && hypot(x - press_x, y - press_y) > DRAG_START) {
        m->drag = press_view;
        struct mc_rect r = shown_rect(press_view);
        m->drag_dx = (int)(press_x - r.x);
        m->drag_dy = (int)(press_y - r.y);
        wlr_scene_node_raise_to_top(&press_view->mc.tree->node);
    }
    if (press_slot >= 0 && m->drag_space < 0 && hypot(x - press_x, y - press_y) > DRAG_START)
        m->drag_space = press_slot;

    /* Reordering desktops: the dragged one takes the place under the pointer. */
    if (m->drag_space >= 0) {
        int to = -1;
        for (int i = 0; i < bar_n; i++)
            if (x >= mini[i].x - GAP / 2 && x < mini[i].x + mini[i].width + GAP / 2)
                to = i;
        int from = m->drag_space;
        if (to >= 0 && to != from) {
            struct space s = srv.spaces[from];
            int step = to > from ? 1 : -1;
            for (int i = from; i != to; i += step)
                srv.spaces[i] = srv.spaces[i + step];
            srv.spaces[to] = s;
            struct view *v;
            wl_list_for_each(v, &srv.views, link) {
                if (v->space == from)
                    v->space = to;
                else if (step > 0 && v->space > from && v->space <= to)
                    v->space--;
                else if (step < 0 && v->space < from && v->space >= to)
                    v->space++;
            }
            if (srv.cur == from)
                srv.cur = to;
            else if (step > 0 && srv.cur > from && srv.cur <= to)
                srv.cur--;
            else if (step < 0 && srv.cur < from && srv.cur >= to)
                srv.cur++;
            m->drag_space = to;
            press_slot = to;
            bar_build();
        }
        mc_apply();
        schedule_frame();
        return true;
    }

    hover_slot = slot_at(x, y);
    m->hover = m->drag ? NULL : thumb_at(x, y);
    mc_apply();
    schedule_frame();
    return true;
}

bool mc_button(uint32_t button, bool pressed)
{
    struct mc *m = &srv.mc;
    if (!built)
        return false;
    if (!m->open)
        return true;                /* closing: clicks wait until it is done */
    double x = srv.cursor->x, y = srv.cursor->y;
    if (button != BTN_LEFT)
        return true;
    if (pressed) {
        press_x = x;
        press_y = y;
        press_ms = now_ms();
        press_view = NULL;
        press_slot = -1;
        if (m->quick) {
            m->quick = NULL;
            return true;
        }
        struct wlr_surface *s;
        double sx, sy;
        enum kind *k = owner_at(x, y, &s, &sx, &sy);
        struct hit *h = k && *k == K_HIT ? (struct hit *)k : NULL;
        if (h && h->what == H_THUMB) {
            press_view = h->view;
        } else if (h && h->what == H_SPACE_CLOSE) {
            space_remove(h->index);
        } else if (on_add(x, y) || (h && h->what == H_SPACE_ADD)) {
            space_add();
        } else if (h && h->what == H_SPACE) {
            press_slot = h->index;
        } else if (h && h->what == H_CURL) {
            close_mc(NULL);
        } else {
            struct output *o = primary_output();
            bool in_bar = o && y < o->full.y + (m->bar > 0 ? bar_big_h : BAR_SMALL);
            if (!in_bar)
                close_mc(NULL);     /* a click on empty space */
        }
        return true;
    }

    /* Released. */
    if (m->drag) {
        struct view *v = m->drag;
        m->drag = NULL;
        press_view = NULL;
        int slot = slot_at(x, y);
        int dest_space = -1;
        if (slot >= 0 && slot != srv.cur)
            dest_space = slot;
        else if (on_add(x, y))
            dest_space = space_add();
        if (dest_space >= 0) {
            thumb_destroy(v);
            view_move_to_space(v, dest_space);
            for (int i = 0; i < bar_n; i++)
                build_minis(i);
            relayout(true);
        } else {
            /* Dropped on nothing: it glides back to its place. */
            v->mc.from_shown = (struct mc_rect){(int)(x - m->drag_dx), (int)(y - m->drag_dy),
                                                shown_rect(v).w, shown_rect(v).h};
            v->mc.relayout = true;
            re = srv.reduce_motion ? 1 : 0;
        }
        mc_apply();
        schedule_frame();
        return true;
    }
    if (m->drag_space >= 0) {
        m->drag_space = -1;
        press_slot = -1;
        return true;
    }
    if (press_view) {
        struct view *v = press_view;
        press_view = NULL;
        close_mc(v);
        return true;
    }
    if (press_slot >= 0) {
        int i = press_slot;
        press_slot = -1;
        if (i == srv.cur) {
            close_mc(NULL);
        } else {
            /* Another desktop: Mission Control closes onto it. */
            switch_to = i;
            finish();
        }
        return true;
    }
    return true;
}

bool mc_key(uint32_t sym)
{
    struct mc *m = &srv.mc;
    switch (sym) {
    case XKB_KEY_Escape:
        if (m->quick)
            m->quick = NULL;
        else
            close_mc(NULL);
        schedule_frame();
        return true;
    case XKB_KEY_space:
        /* Quick Look: the window under the pointer, big; Space again puts it back. */
        if (m->quick) {
            m->quick = NULL;
        } else if (m->hover) {
            m->quick = m->hover;
            m->quick_p = 0;
            wlr_scene_node_raise_to_top(&m->quick->mc.tree->node);
        }
        mc_apply();
        schedule_frame();
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        close_mc(m->hover);
        return true;
    default:
        return sym != XKB_KEY_F3 && sym != XKB_KEY_XF86LaunchA && sym != XKB_KEY_Up &&
               sym != XKB_KEY_Down && sym != XKB_KEY_Left && sym != XKB_KEY_Right &&
               !(sym >= XKB_KEY_1 && sym <= XKB_KEY_9);
    }
}

/* --- desktops --------------------------------------------------------------------- */

static void slide_frame(uint32_t ms)
{
    struct slide *s = &srv.slide;
    if (!s->on)
        return;
    struct output *o = primary_output();
    int w = o ? o->full.width : 1024;
    s->p = srv.reduce_motion ? 1 : fmin(1, s->p + (double)(ms - s->last_ms) / SLIDE_MS);
    s->last_ms = ms;
    double e = ease(s->p);
    int dir = s->to > s->from ? 1 : -1;
    if (s->from < srv.nspaces)
        wlr_scene_node_set_position(&srv.spaces[s->from].tree->node, lround(-dir * w * e), 0);
    wlr_scene_node_set_position(&srv.spaces[s->to].tree->node, lround(dir * w * (1 - e)), 0);
    if (s->p >= 1) {
        s->on = false;
        if (s->from < srv.nspaces && s->from != srv.cur) {
            wlr_scene_node_set_enabled(&srv.spaces[s->from].tree->node, false);
            wlr_scene_node_set_position(&srv.spaces[s->from].tree->node, 0, 0);
        }
        wlr_scene_node_set_position(&srv.spaces[s->to].tree->node, 0, 0);
        pointer_refocus();
    }
}

void space_switch(int to)
{
    if (to < 0 || to >= srv.nspaces || to == srv.cur)
        return;
    if (built)
        finish();
    struct slide *s = &srv.slide;
    if (s->on) {                    /* a slide already going: end it first */
        s->p = 1;
        slide_frame(s->last_ms);
    }
    int from = srv.cur;
    srv.cur = to;
    wlr_scene_node_set_enabled(&srv.spaces[to].tree->node, true);
    *s = (struct slide){.on = true, .from = from, .to = to, .p = 0, .last_ms = now_ms()};
    slide_frame(s->last_ms);
    struct view *v, *front = NULL;
    wl_list_for_each(v, &srv.views, link)
        if (v->kind == K_VIEW && view_on_screen(v)) {
            front = v;
            break;
        }
    if (front)
        focus_view(front);
    else if (srv.focused && srv.focused->space != to)
        focus_view(NULL);
    schedule_frame();
}

int space_add(void)
{
    if (srv.nspaces >= MAX_SPACES)
        return -1;
    int i = srv.nspaces++;
    srv.spaces[i].tree = wlr_scene_tree_create(srv.spaces_root);
    wlr_scene_node_set_enabled(&srv.spaces[i].tree->node, false);
    snprintf(srv.spaces[i].name, sizeof srv.spaces[i].name, "Desktop %d", i + 1);
    if (built) {
        bar_build();
        relayout(true);
        mc_apply();
        schedule_frame();
    }
    return i;
}

/* Close desktop i: its windows move to the one beside it, as on the Mac. */
void space_remove(int i)
{
    if (srv.nspaces <= 1 || i < 0 || i >= srv.nspaces)
        return;
    int into = i > 0 ? i - 1 : 1;
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        if (v->space == i && v->tree) {
            v->space = into;
            wlr_scene_node_reparent(&v->tree->node, srv.spaces[into].tree);
        }
    bool was_cur = srv.cur == i;
    wlr_scene_node_destroy(&srv.spaces[i].tree->node);
    for (int k = i; k < srv.nspaces - 1; k++)
        srv.spaces[k] = srv.spaces[k + 1];
    srv.nspaces--;
    wl_list_for_each(v, &srv.views, link)
        if (v->space > i)
            v->space--;
    if (srv.cur > i || (was_cur && srv.cur == i && i > 0))
        srv.cur--;
    if (srv.cur >= srv.nspaces)
        srv.cur = srv.nspaces - 1;
    wlr_scene_node_set_enabled(&srv.spaces[srv.cur].tree->node, true);
    if (built) {
        /* Windows that came onto this desktop join Mission Control. */
        wl_list_for_each_reverse(v, &srv.views, link)
            if (!v->mc.tree && in_mc(v)) {
                thumb_create(v);
                v->mc.to_c = v->mc.to_e = v->mc.from;
            }
        bar_build();
        relayout(true);
        mc_apply();
        schedule_frame();
    }
}

void swipe(int fingers, double dx, double dy)
{
    if (fingers < 3)
        return;
    if (fabs(dy) > fabs(dx) && fabs(dy) > 40) {
        if (dy < 0 && !srv.mc.open)
            mc_toggle(false);
        else if (dy > 0)
            srv.mc.open ? close_mc(NULL) : mc_toggle(true);
    } else if (fabs(dx) > 40) {
        space_switch(srv.cur + (dx < 0 ? 1 : -1));
    }
}

void mc_init(void)
{
    struct mc *m = &srv.mc;
    static const float black[4] = {0, 0, 0, 0};
    m->dim = wlr_scene_rect_create(m->tree, 1, 1, black);
    m->windows = wlr_scene_tree_create(m->tree);
    m->bar_tree = wlr_scene_tree_create(m->tree);
    m->label = wlr_scene_buffer_create(m->tree, NULL);
    m->add_pic = draw_round_button(48, true);
    m->close_pic = draw_round_button(22, false);
    m->add = wlr_scene_buffer_create(m->bar_tree, m->add_pic ? m->add_pic->base : NULL);
    m->add_hit = (struct hit){.kind = K_HIT, .what = H_SPACE_ADD};
    m->add->node.data = &m->add_hit;
    m->drag_space = -1;
    m->mip_timer = wl_event_loop_add_timer(srv.loop, mip_tick, NULL);
    m->mini_timer = wl_event_loop_add_timer(srv.loop, mini_tick, NULL);
    wlr_scene_node_set_enabled(&m->tree->node, false);
    srv.curl->point_accepts_input = curl_accepts;
}
