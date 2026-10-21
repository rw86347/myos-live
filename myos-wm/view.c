/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * view.c — windows: Wayland ones (xdg-shell) and X11 ones (XWayland), the
 * title bars we draw on them, focus and stacking, maximize and minimize,
 * and the list the taskbar reads (wlr-foreign-toplevel).
 *
 * A window ("view") is one scene tree in its desktop's tree:
 *     tree      at the frame's top-left
 *       frame   a rectangle 1 px bigger than title bar + content, behind
 *       title   our title bar picture (draw.c), when we decorate it
 *       content the program's surfaces, at (FRAME_W, TITLE_H)
 * v->x, v->y is where the content's top-left is on the screen; v->w, v->h its
 * size. Programs that draw their own title bars (GTK header bars and the
 * like) get no frame or title: the decoration protocols tell us which.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include "wm.h"

static const float frame_active[4] = {0x0a / 255.f, 0x40 / 255.f, 0x48 / 255.f, 1};
static const float frame_inactive[4] = {0x8a / 255.f, 0xa9 / 255.f, 0xac / 255.f, 1};

const char *view_title(struct view *v)
{
    const char *t = v->type == VIEW_XDG ? v->xdg->title : v->xw->title;
    return t && *t ? t : view_app(v);
}

const char *view_app(struct view *v)
{
    const char *a = v->type == VIEW_XDG ? v->xdg->app_id : v->xw->class;
    return a ? a : "";
}

bool view_on_screen(struct view *v)
{
    return v->mapped && v->space == srv.cur && !v->minimized;
}

/* --- geometry --------------------------------------------------------------------- */

static int top_h(struct view *v)
{
    return v->ssd ? TITLE_H : 0;
}

static int side_w(struct view *v)
{
    return v->ssd ? FRAME_W : 0;
}

/* Put the scene nodes where v->x, y, w, h say. */
static void place(struct view *v)
{
    if (!v->tree)
        return;
    wlr_scene_node_set_position(&v->tree->node, v->x - side_w(v), v->y - top_h(v));
    int cx = side_w(v), cy = top_h(v);
    if (v->type == VIEW_XDG) {
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
        cx -= geo.x;
        cy -= geo.y;
    }
    wlr_scene_node_set_position(&v->content->node, cx, cy);
    if (v->frame) {
        wlr_scene_node_set_enabled(&v->frame->node, v->ssd);
        wlr_scene_rect_set_size(v->frame, v->w + 2 * FRAME_W, TITLE_H + v->h + FRAME_W);
    }
    if (v->title)
        wlr_scene_node_set_enabled(&v->title->node, v->ssd);
}

void view_redraw_title(struct view *v)
{
    if (!v->ssd || !v->title)
        return;
    draw_title(v);
    if (v->title_pic) {
        pixbuf_damage(v->title, v->title_pic);
        wlr_scene_buffer_set_dest_size(v->title, v->w + 2 * FRAME_W, TITLE_H);
    }
    if (v->frame)
        wlr_scene_rect_set_color(v->frame, v == srv.focused ? frame_active : frame_inactive);
}

void view_set_position(struct view *v, int x, int y)
{
    v->x = x;
    v->y = y;
    place(v);
    if (v->type == VIEW_X11)
        wlr_xwayland_surface_configure(v->xw, x, y, v->w, v->h);
}

/* A new place and size at once (resizing an X11 window from its edge). */
void view_configure(struct view *v, int x, int y, int w, int h)
{
    v->x = x;
    v->y = y;
    place(v);
    if (v->type == VIEW_X11)
        wlr_xwayland_surface_configure(v->xw, x, y, w, h);
    else
        wlr_xdg_toplevel_set_size(v->xdg, w, h);
}

static void request_size(struct view *v, int w, int h)
{
    if (v->type == VIEW_XDG)
        wlr_xdg_toplevel_set_size(v->xdg, w, h);
    else
        wlr_xwayland_surface_configure(v->xw, v->x, v->y, w, h);
}

/* Where a new window goes: centred in the free part of the screen, a
   little lower and to the right of the last one, so they don't stack up
   exactly. */
static void initial_place(struct view *v)
{
    struct output *o = primary_output();
    if (!o)
        return;
    static int cascade;
    struct wlr_box a = o->usable;
    int x = a.x + (a.width - v->w) / 2 + cascade, y = a.y + (a.height - v->h) / 2 + cascade;
    cascade = (cascade + 24) % 120;
    if (y - top_h(v) < a.y)
        y = a.y + top_h(v);
    if (x < a.x)
        x = a.x;
    view_set_position(v, x, y);
}

/* --- focus ------------------------------------------------------------------------ */

static void activate(struct view *v, bool on)
{
    if (v->type == VIEW_XDG)
        wlr_xdg_toplevel_set_activated(v->xdg, on);
    else
        wlr_xwayland_surface_activate(v->xw, on);
    if (v->ftl)
        wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, on);
}

void focus_view(struct view *v)
{
    if (v && (!v->mapped || v->kind != K_VIEW))
        return;
    struct view *old = srv.focused;
    if (v && v->space != srv.cur)
        space_switch(v->space);
    if (v && v->minimized)
        view_set_minimized(v, false);
    if (old && old != v) {
        srv.focused = NULL;
        activate(old, false);
        view_redraw_title(old);
    }
    srv.focused = v;
    if (!v) {
        wlr_seat_keyboard_notify_clear_focus(srv.seat);
        return;
    }
    wlr_scene_node_raise_to_top(&v->tree->node);
    wl_list_remove(&v->link);
    wl_list_insert(&srv.views, &v->link);
    if (v->type == VIEW_X11)
        wlr_xwayland_surface_restack(v->xw, NULL, XCB_STACK_MODE_ABOVE);
    activate(v, true);
    view_redraw_title(v);
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
    if (kb)
        wlr_seat_keyboard_notify_enter(srv.seat, v->surface, kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
    else
        wlr_seat_keyboard_notify_enter(srv.seat, v->surface, NULL, 0, NULL);
}

/* The window that should have the focus once v is gone or hidden. */
static void focus_next(struct view *gone)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link)
        if (v != gone && v->kind == K_VIEW && view_on_screen(v)) {
            focus_view(v);
            return;
        }
    if (srv.focused == gone)
        focus_view(NULL);
}

/* --- what the pointer is over ----------------------------------------------------- */

enum kind *owner_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
    *surface = NULL;
    struct wlr_scene_node *node = wlr_scene_node_at(&srv.scene->tree.node, lx, ly, sx, sy);
    if (!node)
        return NULL;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss =
            wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss)
            *surface = ss->surface;
    }
    for (struct wlr_scene_node *n = node; n; n = n->parent ? &n->parent->node : NULL)
        if (n->data)
            return n->data;
    return NULL;
}

/* --- actions ---------------------------------------------------------------------- */

void view_close(struct view *v)
{
    if (v->type == VIEW_XDG)
        wlr_xdg_toplevel_send_close(v->xdg);
    else
        wlr_xwayland_surface_close(v->xw);
}

void view_set_minimized(struct view *v, bool min)
{
    if (v->minimized == min || !v->tree)
        return;
    v->minimized = min;
    wlr_scene_node_set_enabled(&v->tree->node, !min);
    if (v->ftl)
        wlr_foreign_toplevel_handle_v1_set_minimized(v->ftl, min);
    if (v->type == VIEW_X11)
        wlr_xwayland_surface_set_minimized(v->xw, min);
    if (min)
        focus_next(v);
}

void view_toggle_maximize(struct view *v)
{
    struct output *o = primary_output();
    if (!o)
        return;
    v->maximized = !v->maximized;
    if (v->maximized) {
        v->saved = (struct wlr_box){v->x, v->y, v->w, v->h};
        struct wlr_box a = o->usable;
        int w = a.width - 2 * side_w(v), h = a.height - top_h(v) - side_w(v);
        v->x = a.x + side_w(v);
        v->y = a.y + top_h(v);
        place(v);
        request_size(v, w, h);
    } else {
        v->x = v->saved.x;
        v->y = v->saved.y;
        place(v);
        request_size(v, v->saved.width, v->saved.height);
    }
    if (v->type == VIEW_XDG)
        wlr_xdg_toplevel_set_maximized(v->xdg, v->maximized);
    else
        wlr_xwayland_surface_set_maximized(v->xw, v->maximized);
    if (v->ftl)
        wlr_foreign_toplevel_handle_v1_set_maximized(v->ftl, v->maximized);
}

void view_move_to_space(struct view *v, int space)
{
    if (space < 0 || space >= srv.nspaces || !v->tree)
        return;
    v->space = space;
    wlr_scene_node_reparent(&v->tree->node, srv.spaces[space].tree);
    if (srv.focused == v && space != srv.cur)
        focus_next(v);
}

static void frame_done_iter(struct wlr_surface *s, int sx, int sy, void *data)
{
    (void)sx;
    (void)sy;
    wlr_surface_send_frame_done(s, data);
}

/* "Draw your next frame": what a hidden window gets when Mission Control
   wants a fresh picture of it. */
void view_send_frame(struct view *v)
{
    if (!v->surface)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (v->type == VIEW_XDG)
        wlr_xdg_surface_for_each_surface(v->xdg->base, frame_done_iter, &now);
    else
        wlr_surface_for_each_surface(v->surface, frame_done_iter, &now);
}

void begin_interactive(struct view *v, enum cursor_mode mode, uint32_t edges)
{
    if (v->maximized)
        view_toggle_maximize(v);
    srv.grab = v;
    srv.cursor_mode = mode;
    if (mode == CUR_MOVE) {
        srv.grab_x = srv.cursor->x - v->x;
        srv.grab_y = srv.cursor->y - v->y;
    } else {
        srv.grab_box = (struct wlr_box){v->x, v->y, v->w, v->h};
        srv.grab_x = srv.cursor->x;
        srv.grab_y = srv.cursor->y;
        srv.resize_edges = edges;
        if (v->type == VIEW_XDG)
            wlr_xdg_toplevel_set_resizing(v->xdg, true);
    }
}

/* --- the taskbar's list (foreign-toplevel) ---------------------------------------- */

static void ftl_activate(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, ftl_activate);
    focus_view(v);
}

static void ftl_minimize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, ftl_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
    view_set_minimized(v, ev->minimized);
}

static void ftl_maximize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, ftl_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
    if (ev->maximized != v->maximized)
        view_toggle_maximize(v);
}

static void ftl_close(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, ftl_close);
    view_close(v);
}

static void ftl_create(struct view *v)
{
    v->ftl = wlr_foreign_toplevel_handle_v1_create(srv.ftl_mgr);
    wlr_foreign_toplevel_handle_v1_set_title(v->ftl, view_title(v));
    wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, view_app(v));
    struct output *o = primary_output();
    if (o)
        wlr_foreign_toplevel_handle_v1_output_enter(v->ftl, o->wlr_output);
    v->ftl_activate.notify = ftl_activate;
    wl_signal_add(&v->ftl->events.request_activate, &v->ftl_activate);
    v->ftl_minimize.notify = ftl_minimize;
    wl_signal_add(&v->ftl->events.request_minimize, &v->ftl_minimize);
    v->ftl_maximize.notify = ftl_maximize;
    wl_signal_add(&v->ftl->events.request_maximize, &v->ftl_maximize);
    v->ftl_close.notify = ftl_close;
    wl_signal_add(&v->ftl->events.request_close, &v->ftl_close);
}

static void ftl_destroy(struct view *v)
{
    if (!v->ftl)
        return;
    wl_list_remove(&v->ftl_activate.link);
    wl_list_remove(&v->ftl_minimize.link);
    wl_list_remove(&v->ftl_maximize.link);
    wl_list_remove(&v->ftl_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(v->ftl);
    v->ftl = NULL;
}

/* --- decorations: do we draw the title bar? --------------------------------------- */

static bool wants_ssd(struct view *v)
{
    if (v->type == VIEW_X11)
        return !v->xw->override_redirect &&
               !(v->xw->decorations & WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE);
    struct wlr_xdg_toplevel_decoration_v1 *d;
    wl_list_for_each(d, &srv.xdg_deco->decorations, link)
        if (d->toplevel == v->xdg)
            return d->current.mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    struct wlr_server_decoration *k;
    wl_list_for_each(k, &srv.kde_deco->decorations, link)
        if (k->surface == v->xdg->base->surface)
            return k->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
    return false;                   /* knows neither protocol: it draws its own */
}

static void update_ssd(struct view *v)
{
    bool ssd = wants_ssd(v);
    if (ssd == v->ssd)
        return;
    v->ssd = ssd;
    place(v);
    view_redraw_title(v);
}

struct deco {
    struct wlr_xdg_toplevel_decoration_v1 *d;
    struct wl_listener request, destroy;
};

static void xdg_deco_request(struct wl_listener *l, void *data)
{
    (void)data;
    struct deco *dc = wl_container_of(l, dc, request);
    wlr_xdg_toplevel_decoration_v1_set_mode(dc->d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void xdg_deco_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct deco *dc = wl_container_of(l, dc, destroy);
    wl_list_remove(&dc->request.link);
    wl_list_remove(&dc->destroy.link);
    free(dc);
}

/* A program asked through xdg-decoration: whatever it asks, we decorate
   (one look for every window, as on the Mac). */
static void new_xdg_deco(struct wl_listener *l, void *data)
{
    (void)l;
    struct deco *dc = calloc(1, sizeof *dc);
    dc->d = data;
    dc->request.notify = xdg_deco_request;
    wl_signal_add(&dc->d->events.request_mode, &dc->request);
    dc->destroy.notify = xdg_deco_destroy;
    wl_signal_add(&dc->d->events.destroy, &dc->destroy);
    wlr_xdg_toplevel_decoration_v1_set_mode(dc->d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* --- shared by both kinds --------------------------------------------------------- */

static void make_tree(struct view *v, struct wlr_scene_tree *parent)
{
    v->tree = wlr_scene_tree_create(parent);
    v->tree->node.data = v;
    if (v->kind == K_VIEW) {
        v->frame = wlr_scene_rect_create(v->tree, 1, 1, frame_inactive);
        v->title = wlr_scene_buffer_create(v->tree, NULL);
        v->title_hit = (struct hit){.kind = K_HIT, .what = H_TITLE, .view = v};
        v->title->node.data = &v->title_hit;
    }
}

static void mapped(struct view *v)
{
    v->mapped = true;
    v->space = srv.cur;
    v->ssd = wants_ssd(v);
    wl_list_insert(&srv.views, &v->link);
    ftl_create(v);
    place(v);
    focus_view(v);
    view_redraw_title(v);
}

static void unmapped(struct view *v)
{
    v->mapped = false;
    if (srv.grab == v) {
        srv.grab = NULL;
        srv.cursor_mode = CUR_PASS;
    }
    mc_view_gone(v);
    wl_list_remove(&v->link);
    ftl_destroy(v);
    if (srv.focused == v) {
        srv.focused = NULL;
        focus_next(v);
    }
}

static void set_title(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, set_title);
    if (v->ftl)
        wlr_foreign_toplevel_handle_v1_set_title(v->ftl, view_title(v));
    view_redraw_title(v);
}

static void set_app_id(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, set_app_id);
    if (v->ftl)
        wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, view_app(v));
    view_redraw_title(v);
}

static void request_move(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_move);
    if (v->mapped)
        begin_interactive(v, CUR_MOVE, 0);
}

static void request_maximize(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_maximize);
    if (v->mapped)
        view_toggle_maximize(v);
    else if (v->type == VIEW_XDG && v->xdg->base->initialized)
        wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void request_minimize(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_minimize);
    if (v->mapped)
        view_set_minimized(v, true);
}

static void request_fullscreen(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_fullscreen);
    if (v->type == VIEW_XDG && v->xdg->base->initialized)
        wlr_xdg_surface_schedule_configure(v->xdg->base);
}

/* --- Wayland windows (xdg-shell) -------------------------------------------------- */

static void xdg_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, map);
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
    v->w = geo.width;
    v->h = geo.height;
    v->surface = v->xdg->base->surface;
    wlr_scene_node_reparent(&v->tree->node, srv.spaces[srv.cur].tree);
    wlr_scene_node_set_enabled(&v->tree->node, true);
    v->ssd = wants_ssd(v);
    initial_place(v);
    mapped(v);
}

static void xdg_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, unmap);
    unmapped(v);
    wlr_scene_node_set_enabled(&v->tree->node, false);
}

static void xdg_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, commit);
    if (!v->mapped)
        return;
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
    update_ssd(v);
    if (geo.width != v->w || geo.height != v->h) {
        /* Resizing from the left or top edge moves the window too. */
        if (srv.cursor_mode == CUR_RESIZE && srv.grab == v) {
            if (srv.resize_edges & WLR_EDGE_LEFT)
                v->x = srv.grab_box.x + srv.grab_box.width - geo.width;
            if (srv.resize_edges & WLR_EDGE_TOP)
                v->y = srv.grab_box.y + srv.grab_box.height - geo.height;
        }
        v->w = geo.width;
        v->h = geo.height;
        view_redraw_title(v);
    }
    place(v);
    mc_view_commit(v);
}

static void xdg_resize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_resize);
    struct wlr_xdg_toplevel_resize_event *ev = data;
    if (v->mapped)
        begin_interactive(v, CUR_RESIZE, ev->edges);
}

static void xdg_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, destroy);
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
    wl_list_remove(&v->destroy.link);
    wl_list_remove(&v->request_move.link);
    wl_list_remove(&v->request_resize.link);
    wl_list_remove(&v->request_maximize.link);
    wl_list_remove(&v->request_minimize.link);
    wl_list_remove(&v->request_fullscreen.link);
    wl_list_remove(&v->set_title.link);
    wl_list_remove(&v->set_app_id.link);
    wlr_scene_node_destroy(&v->tree->node);
    pixbuf_drop(v->title_pic);
    free(v);
}

/* The toplevel a popup belongs to, through any popups in between. */
static struct view *popup_view(struct wlr_xdg_surface *xs)
{
    while (xs && xs->role == WLR_XDG_SURFACE_ROLE_POPUP)
        xs = xs->popup->parent ? wlr_xdg_surface_try_from_wlr_surface(xs->popup->parent) : NULL;
    if (!xs || xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !xs->data)
        return NULL;
    struct wlr_scene_tree *content = xs->data;
    return content->node.parent ? content->node.parent->node.data : NULL;
}

static void new_xdg_surface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_surface *xs = data;
    if (xs->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        /* A layer surface's popups arrive through its new_popup event. */
        if (!xs->popup->parent)
            return;
        struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xs->popup->parent);
        if (!parent || !parent->data)
            return;
        xs->data = wlr_scene_xdg_surface_create(parent->data, xs);
        struct view *v = popup_view(xs);
        struct output *o = primary_output();
        if (v && o) {
            struct wlr_box geo;
            wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
            struct wlr_box box = {o->full.x - v->x + geo.x, o->full.y - v->y + geo.y,
                                  o->full.width, o->full.height};
            wlr_xdg_popup_unconstrain_from_box(xs->popup, &box);
        }
        return;
    }
    if (xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        return;
    struct view *v = calloc(1, sizeof *v);
    v->kind = K_VIEW;
    v->type = VIEW_XDG;
    v->xdg = xs->toplevel;
    wl_list_init(&v->link);
    make_tree(v, srv.spaces[srv.cur].tree);
    wlr_scene_node_set_enabled(&v->tree->node, false);
    v->content = wlr_scene_xdg_surface_create(v->tree, xs);
    xs->data = v->content;

    v->map.notify = xdg_map;
    wl_signal_add(&xs->surface->events.map, &v->map);
    v->unmap.notify = xdg_unmap;
    wl_signal_add(&xs->surface->events.unmap, &v->unmap);
    v->commit.notify = xdg_commit;
    wl_signal_add(&xs->surface->events.commit, &v->commit);
    v->destroy.notify = xdg_destroy;
    wl_signal_add(&xs->events.destroy, &v->destroy);
    struct wlr_xdg_toplevel *t = xs->toplevel;
    v->request_move.notify = request_move;
    wl_signal_add(&t->events.request_move, &v->request_move);
    v->request_resize.notify = xdg_resize;
    wl_signal_add(&t->events.request_resize, &v->request_resize);
    v->request_maximize.notify = request_maximize;
    wl_signal_add(&t->events.request_maximize, &v->request_maximize);
    v->request_minimize.notify = request_minimize;
    wl_signal_add(&t->events.request_minimize, &v->request_minimize);
    v->request_fullscreen.notify = request_fullscreen;
    wl_signal_add(&t->events.request_fullscreen, &v->request_fullscreen);
    v->set_title.notify = set_title;
    wl_signal_add(&t->events.set_title, &v->set_title);
    v->set_app_id.notify = set_app_id;
    wl_signal_add(&t->events.set_app_id, &v->set_app_id);
}

/* --- X11 windows (XWayland) ------------------------------------------------------- */

static void x11_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, commit);
    if (!v->mapped || !v->surface)
        return;
    int w = v->surface->current.width, h = v->surface->current.height;
    if (w != v->w || h != v->h) {
        v->w = w;
        v->h = h;
        view_redraw_title(v);
        place(v);
    }
    mc_view_commit(v);
}

static void x11_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, map);
    struct wlr_xwayland_surface *xw = v->xw;
    v->surface = xw->surface;
    v->w = xw->width;
    v->h = xw->height;
    if (xw->override_redirect) {
        v->kind = K_UNMANAGED;
        make_tree(v, srv.unmanaged);
        v->content = wlr_scene_subsurface_tree_create(v->tree, xw->surface);
        wlr_scene_node_set_position(&v->tree->node, xw->x, xw->y);
        v->mapped = true;
        wl_list_insert(&srv.views, &v->link);
        if (wlr_xwayland_or_surface_wants_focus(xw)) {
            struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
            if (kb)
                wlr_seat_keyboard_notify_enter(srv.seat, xw->surface, kb->keycodes,
                                               kb->num_keycodes, &kb->modifiers);
        }
        return;
    }
    v->kind = K_VIEW;
    make_tree(v, srv.spaces[srv.cur].tree);
    v->content = wlr_scene_subsurface_tree_create(v->tree, xw->surface);
    v->ssd = wants_ssd(v);
    bool positioned = xw->size_hints &&
                      (xw->size_hints->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION |
                                                XCB_ICCCM_SIZE_HINT_P_POSITION)) &&
                      (xw->x || xw->y);
    if (positioned)
        view_set_position(v, xw->x, xw->y);
    else
        initial_place(v);
    mapped(v);
}

static void x11_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, unmap);
    if (v->kind == K_UNMANAGED) {
        v->mapped = false;
        wl_list_remove(&v->link);
        if (srv.seat->keyboard_state.focused_surface == v->surface)
            focus_view(srv.focused);
    } else {
        unmapped(v);
    }
    wlr_scene_node_destroy(&v->tree->node);
    v->tree = NULL;
    v->content = NULL;
    v->frame = NULL;
    v->title = NULL;
    v->surface = NULL;
}

static void x11_associate(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, associate);
    struct wlr_surface *s = v->xw->surface;
    v->map.notify = x11_map;
    wl_signal_add(&s->events.map, &v->map);
    v->unmap.notify = x11_unmap;
    wl_signal_add(&s->events.unmap, &v->unmap);
    v->commit.notify = x11_commit;
    wl_signal_add(&s->events.commit, &v->commit);
}

static void x11_dissociate(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, dissociate);
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
}

static void x11_request_configure(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    if (!v->mapped || v->kind == K_UNMANAGED) {
        wlr_xwayland_surface_configure(v->xw, ev->x, ev->y, ev->width, ev->height);
        if (v->tree && v->kind == K_UNMANAGED)
            wlr_scene_node_set_position(&v->tree->node, ev->x, ev->y);
        return;
    }
    /* A managed window may change its size, not its place. */
    wlr_xwayland_surface_configure(v->xw, v->x, v->y, ev->width, ev->height);
}

static void x11_request_activate(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, request_activate);
    if (v->mapped && v->kind == K_VIEW)
        focus_view(v);
}

static void x11_set_decorations(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, set_decorations);
    if (v->mapped && v->kind == K_VIEW)
        update_ssd(v);
}

static void x11_request_resize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_resize);
    struct wlr_xwayland_resize_event *ev = data;
    if (v->mapped && v->kind == K_VIEW)
        begin_interactive(v, CUR_RESIZE, ev->edges);
}

static void x11_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct view *v = wl_container_of(l, v, destroy);
    if (v->tree)
        wlr_scene_node_destroy(&v->tree->node);
    wl_list_remove(&v->destroy.link);
    wl_list_remove(&v->associate.link);
    wl_list_remove(&v->dissociate.link);
    wl_list_remove(&v->request_configure.link);
    wl_list_remove(&v->request_activate.link);
    wl_list_remove(&v->set_decorations.link);
    wl_list_remove(&v->set_title.link);
    wl_list_remove(&v->request_move.link);
    wl_list_remove(&v->request_resize.link);
    wl_list_remove(&v->request_maximize.link);
    wl_list_remove(&v->request_minimize.link);
    wl_list_remove(&v->set_app_id.link);
    pixbuf_drop(v->title_pic);
    free(v);
}

static void new_x11_surface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xwayland_surface *xw = data;
    struct view *v = calloc(1, sizeof *v);
    v->kind = xw->override_redirect ? K_UNMANAGED : K_VIEW;
    v->type = VIEW_X11;
    v->xw = xw;
    xw->data = v;
    wl_list_init(&v->link);
    v->destroy.notify = x11_destroy;
    wl_signal_add(&xw->events.destroy, &v->destroy);
    v->associate.notify = x11_associate;
    wl_signal_add(&xw->events.associate, &v->associate);
    v->dissociate.notify = x11_dissociate;
    wl_signal_add(&xw->events.dissociate, &v->dissociate);
    v->request_configure.notify = x11_request_configure;
    wl_signal_add(&xw->events.request_configure, &v->request_configure);
    v->request_activate.notify = x11_request_activate;
    wl_signal_add(&xw->events.request_activate, &v->request_activate);
    v->set_decorations.notify = x11_set_decorations;
    wl_signal_add(&xw->events.set_decorations, &v->set_decorations);
    v->set_title.notify = set_title;
    wl_signal_add(&xw->events.set_title, &v->set_title);
    v->set_app_id.notify = set_app_id;
    wl_signal_add(&xw->events.set_class, &v->set_app_id);
    v->request_move.notify = request_move;
    wl_signal_add(&xw->events.request_move, &v->request_move);
    v->request_resize.notify = x11_request_resize;
    wl_signal_add(&xw->events.request_resize, &v->request_resize);
    v->request_maximize.notify = request_maximize;
    wl_signal_add(&xw->events.request_maximize, &v->request_maximize);
    v->request_minimize.notify = request_minimize;
    wl_signal_add(&xw->events.request_minimize, &v->request_minimize);
}

/* --- start-up --------------------------------------------------------------------- */

void view_init(void)
{
    srv.xdg_shell = wlr_xdg_shell_create(srv.display, 3);
    static struct wl_listener new_xdg = {.notify = new_xdg_surface};
    wl_signal_add(&srv.xdg_shell->events.new_surface, &new_xdg);

    srv.ftl_mgr = wlr_foreign_toplevel_manager_v1_create(srv.display);

    /* GTK 3 asks through KDE's protocol, foot and Qt through xdg-decoration. */
    srv.kde_deco = wlr_server_decoration_manager_create(srv.display);
    wlr_server_decoration_manager_set_default_mode(srv.kde_deco,
                                                   WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    srv.xdg_deco = wlr_xdg_decoration_manager_v1_create(srv.display);
    static struct wl_listener new_deco = {.notify = new_xdg_deco};
    wl_signal_add(&srv.xdg_deco->events.new_toplevel_decoration, &new_deco);

    /* X11 programs: XWayland starts the first time one connects. */
    srv.xwayland = wlr_xwayland_create(srv.display, srv.compositor, true);
    if (srv.xwayland) {
        static struct wl_listener new_x11 = {.notify = new_x11_surface};
        wl_signal_add(&srv.xwayland->events.new_surface, &new_x11);
    } else {
        wlr_log(WLR_ERROR, "no XWayland: X11 programs won't start");
    }
}
