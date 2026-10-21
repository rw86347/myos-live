/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * input.c — keyboard, mouse and touchpad.
 *
 * Keys that belong to the window server (the Mac's, where MyOS has them):
 *   F3, Ctrl+Up          Mission Control (again: back)
 *   Ctrl+Down            App Exposé: the focused program's windows
 *   Ctrl+Left/Right      the desktop to the left / right
 *   Ctrl+1 .. Ctrl+9     desktop 1 .. 9
 *   Alt+Tab, Alt+F4      next window, close window
 *   Ctrl+Alt+T           a terminal (foot)
 *   Ctrl+Alt+F1..F12     the text consoles; Ctrl+Alt+Backspace ends the session
 * While Mission Control is open it gets every key (Esc, Space, Return).
 *
 * The mouse goes to whatever is under it, except our own parts: the curl,
 * title bars, the 6 px around a window (resize), and all of Mission
 * Control. Three fingers on a touchpad: up opens Mission Control, down App
 * Exposé, left and right change desktop.
 */
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>
#include "wm.h"

#define EDGE 6                      /* the grab zone around a window */

struct keyboard {
    struct wl_list link;
    struct wlr_keyboard *kb;
    struct wl_listener modifiers, key, destroy;
};

static uint8_t eaten[256 / 8];      /* keys we kept: their release is ours too */
static uint32_t last_title_click;
static struct view *last_title_view;
static const char *cursor_name = "";

static void set_cursor(const char *name)
{
    if (strcmp(cursor_name, name) == 0)
        return;
    cursor_name = name;
    wlr_cursor_set_xcursor(srv.cursor, srv.cursor_mgr, name);
}

/* --- keyboard --------------------------------------------------------------------- */

static struct view *next_on_desktop(void)
{
    struct view *v, *last = NULL;
    wl_list_for_each(v, &srv.views, link)
        if (v->kind == K_VIEW && view_on_screen(v))
            last = v;
    return last;
}

static bool binding(uint32_t mods, xkb_keysym_t sym)
{
    bool ctrl = mods & WLR_MODIFIER_CTRL, alt = mods & WLR_MODIFIER_ALT;
    if (ctrl && alt && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (srv.session)
            wlr_session_change_vt(srv.session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
        return true;
    }
    if (srv.mc.open && mc_key(sym))
        return true;
    if (sym == XKB_KEY_F3 || sym == XKB_KEY_XF86LaunchA || (ctrl && !alt && sym == XKB_KEY_Up)) {
        mc_toggle(false);
        return true;
    }
    if (ctrl && !alt && sym == XKB_KEY_Down) {
        mc_toggle(true);
        return true;
    }
    if (ctrl && !alt && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
        space_switch(srv.cur + (sym == XKB_KEY_Left ? -1 : 1));
        return true;
    }
    if (ctrl && !alt && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        space_switch((int)(sym - XKB_KEY_1));
        return true;
    }
    if (alt && !ctrl && sym == XKB_KEY_Tab) {
        struct view *v = next_on_desktop();
        if (v)
            focus_view(v);
        return true;
    }
    if (alt && !ctrl && sym == XKB_KEY_F4) {
        if (srv.focused)
            view_close(srv.focused);
        return true;
    }
    if (ctrl && alt && (sym == XKB_KEY_t || sym == XKB_KEY_T)) {
        spawn("foot");
        return true;
    }
    if (ctrl && alt && sym == XKB_KEY_BackSpace) {
        wl_display_terminate(srv.display);
        return true;
    }
    return false;
}

static void kb_key(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, key);
    struct wlr_keyboard_key_event *ev = data;
    uint32_t code = ev->keycode;
    bool handled = false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        const xkb_keysym_t *syms;
        int n = xkb_state_key_get_syms(k->kb->xkb_state, code + 8, &syms);
        uint32_t mods = wlr_keyboard_get_modifiers(k->kb);
        for (int i = 0; i < n && !handled; i++)
            handled = binding(mods, syms[i]);
        if (code < 256) {
            if (handled)
                eaten[code / 8] |= 1 << (code % 8);
            else
                eaten[code / 8] &= ~(1 << (code % 8));
        }
    } else if (code < 256 && (eaten[code / 8] & (1 << (code % 8)))) {
        eaten[code / 8] &= ~(1 << (code % 8));
        handled = true;
    }
    if (!handled) {
        wlr_seat_set_keyboard(srv.seat, k->kb);
        wlr_seat_keyboard_notify_key(srv.seat, ev->time_msec, code, ev->state);
    }
}

static void kb_modifiers(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, modifiers);
    wlr_seat_set_keyboard(srv.seat, k->kb);
    wlr_seat_keyboard_notify_modifiers(srv.seat, &k->kb->modifiers);
}

static void kb_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct keyboard *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->destroy.link);
    wl_list_remove(&k->link);
    free(k);
}

static void new_keyboard(struct wlr_input_device *dev)
{
    struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
    struct keyboard *k = calloc(1, sizeof *k);
    k->kb = kb;
    /* The layout comes from XKB_DEFAULT_LAYOUT and friends, as everywhere. */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, map);
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(kb, 25, 600);
    k->modifiers.notify = kb_modifiers;
    wl_signal_add(&kb->events.modifiers, &k->modifiers);
    k->key.notify = kb_key;
    wl_signal_add(&kb->events.key, &k->key);
    k->destroy.notify = kb_destroy;
    wl_signal_add(&dev->events.destroy, &k->destroy);
    wlr_seat_set_keyboard(srv.seat, kb);
    wl_list_insert(&srv.keyboards, &k->link);
}

/* --- the pointer ------------------------------------------------------------------ */

/* Which title-bar button is at sx (title-bar coordinates), or 0. */
static int title_button(struct view *v, double sx, double sy)
{
    int w = v->w + 2 * FRAME_W;
    if (sy < 5 || sy > 26)
        return 0;
    static const int which[3] = {H_MIN, H_MAX, H_CLOSE};
    for (int k = 0; k < 3; k++) {
        double x = w - 78 + k * 24;
        if (sx >= x && sx < x + 21)
            return which[k];
    }
    return 0;
}

static void set_hover_button(struct view *v, int b)
{
    static struct view *hovered;
    if (hovered && hovered != v && hovered->hover_button) {
        hovered->hover_button = 0;
        view_redraw_title(hovered);
    }
    hovered = v;
    if (v && v->hover_button != b) {
        v->hover_button = b;
        view_redraw_title(v);
    }
}

/* A decorated window whose edge zone (EDGE px outside it) holds the
   pointer: which edges. Front windows first. */
static struct view *edge_at(double x, double y, uint32_t *edges)
{
    struct view *v;
    wl_list_for_each(v, &srv.views, link) {
        if (v->kind != K_VIEW || !view_on_screen(v) || !v->ssd || v->maximized)
            continue;
        double l = v->x - FRAME_W, t = v->y - TITLE_H, r = v->x + v->w + FRAME_W,
               b = v->y + v->h + FRAME_W;
        if (x < l - EDGE || x >= r + EDGE || y < t - EDGE || y >= b + EDGE)
            continue;
        if (x >= l && x < r && y >= t && y < b)
            return NULL;            /* on the window itself: it is in front */
        *edges = (x < l ? WLR_EDGE_LEFT : x >= r ? WLR_EDGE_RIGHT : 0) |
                 (y < t ? WLR_EDGE_TOP : y >= b ? WLR_EDGE_BOTTOM : 0);
        return v;
    }
    return NULL;
}

static const char *edge_cursor(uint32_t e)
{
    switch (e) {
    case WLR_EDGE_TOP: return "n-resize";
    case WLR_EDGE_BOTTOM: return "s-resize";
    case WLR_EDGE_LEFT: return "w-resize";
    case WLR_EDGE_RIGHT: return "e-resize";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
    default: return "se-resize";
    }
}

static void do_resize(void)
{
    struct view *v = srv.grab;
    struct wlr_box b = srv.grab_box;
    double dx = srv.cursor->x - srv.grab_x, dy = srv.cursor->y - srv.grab_y;
    int l = b.x, t = b.y, r = b.x + b.width, bt = b.y + b.height;
    if (srv.resize_edges & WLR_EDGE_LEFT)
        l = (int)(b.x + dx);
    if (srv.resize_edges & WLR_EDGE_RIGHT)
        r = (int)(r + dx);
    if (srv.resize_edges & WLR_EDGE_TOP)
        t = (int)(b.y + dy);
    if (srv.resize_edges & WLR_EDGE_BOTTOM)
        bt = (int)(bt + dy);
    if (r - l < 120)
        (srv.resize_edges & WLR_EDGE_LEFT) ? (l = r - 120) : (r = l + 120);
    if (bt - t < 60)
        (srv.resize_edges & WLR_EDGE_TOP) ? (t = bt - 60) : (bt = t + 60);
    if (v->type == VIEW_XDG) {
        /* The place follows when the program commits the new size (view.c). */
        wlr_xdg_toplevel_set_size(v->xdg, r - l, bt - t);
    } else {
        view_configure(v, l, t, r - l, bt - t);
    }
}

static void motion(uint32_t time)
{
    double x = srv.cursor->x, y = srv.cursor->y;
    if (srv.mc.open || mc_animating()) {
        if (mc_motion(x, y)) {
            set_cursor("default");
            wlr_seat_pointer_clear_focus(srv.seat);
            return;
        }
    }
    if (srv.cursor_mode == CUR_MOVE && srv.grab) {
        view_set_position(srv.grab, (int)(x - srv.grab_x), (int)(y - srv.grab_y));
        return;
    }
    if (srv.cursor_mode == CUR_RESIZE && srv.grab) {
        do_resize();
        return;
    }
    if (srv.cursor_mode == CUR_BUTTON && srv.grab) {
        struct view *v = srv.grab;
        int b = title_button(v, x - (v->x - FRAME_W), y - (v->y - TITLE_H));
        set_hover_button(v, b);
        return;
    }

    uint32_t edges = 0;
    struct view *ev = edge_at(x, y, &edges);
    struct wlr_surface *surface = NULL;
    double sx = 0, sy = 0;
    enum kind *owner = ev ? NULL : owner_at(x, y, &surface, &sx, &sy);
    bool on_curl = owner && *owner == K_HIT && ((struct hit *)owner)->what == H_CURL;
    curl_hover(on_curl);
    struct view *title_view = NULL;
    if (owner && *owner == K_HIT && ((struct hit *)owner)->what == H_TITLE) {
        title_view = ((struct hit *)owner)->view;
        set_hover_button(title_view, title_button(title_view, sx, sy));
    } else {
        set_hover_button(NULL, 0);
    }
    if (ev) {
        set_cursor(edge_cursor(edges));
        wlr_seat_pointer_clear_focus(srv.seat);
    } else if (surface) {
        if (!srv.seat->pointer_state.focused_surface)
            cursor_name = "";       /* the program sets its own */
        wlr_seat_pointer_notify_enter(srv.seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(srv.seat, time, sx, sy);
    } else {
        set_cursor("default");
        wlr_seat_pointer_clear_focus(srv.seat);
    }
}

void pointer_refocus(void)
{
    motion(now_ms());
}

static void cursor_motion(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(srv.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    motion(ev->time_msec);
}

static void cursor_motion_abs(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(srv.cursor, &ev->pointer->base, ev->x, ev->y);
    motion(ev->time_msec);
}

static void end_grab(void)
{
    if (srv.cursor_mode == CUR_RESIZE && srv.grab && srv.grab->type == VIEW_XDG)
        wlr_xdg_toplevel_set_resizing(srv.grab->xdg, false);
    srv.cursor_mode = CUR_PASS;
    srv.grab = NULL;
}

static void cursor_button(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_button_event *ev = data;
    bool pressed = ev->state == WLR_BUTTON_PRESSED;
    if ((srv.mc.open || mc_animating()) && mc_button(ev->button, pressed))
        return;

    if (!pressed) {
        if (srv.cursor_mode == CUR_BUTTON && srv.grab) {
            struct view *v = srv.grab;
            int b = v->pressed_button;
            v->pressed_button = 0;
            end_grab();
            view_redraw_title(v);
            if (b && b == v->hover_button) {
                if (b == H_CLOSE)
                    view_close(v);
                else if (b == H_MAX)
                    view_toggle_maximize(v);
                else
                    view_set_minimized(v, true);
            }
            pointer_refocus();
            return;
        }
        if (srv.cursor_mode == CUR_MOVE || srv.cursor_mode == CUR_RESIZE) {
            end_grab();
            pointer_refocus();
        }
        wlr_seat_pointer_notify_button(srv.seat, ev->time_msec, ev->button, ev->state);
        return;
    }

    double x = srv.cursor->x, y = srv.cursor->y;
    uint32_t edges = 0;
    struct view *ev_view = edge_at(x, y, &edges);
    if (ev_view) {
        focus_view(ev_view);
        begin_interactive(ev_view, CUR_RESIZE, edges);
        return;
    }
    struct wlr_surface *surface = NULL;
    double sx, sy;
    enum kind *owner = owner_at(x, y, &surface, &sx, &sy);
    if (owner && *owner == K_HIT) {
        struct hit *h = (struct hit *)owner;
        if (h->what == H_CURL) {
            mc_toggle(false);
            return;
        }
        if (h->what == H_TITLE) {
            struct view *v = h->view;
            focus_view(v);
            int b = title_button(v, sx, sy);
            if (b && ev->button == BTN_LEFT) {
                v->pressed_button = b;
                srv.grab = v;
                srv.cursor_mode = CUR_BUTTON;
                view_redraw_title(v);
                return;
            }
            if (ev->button == BTN_LEFT) {
                /* A double click on the title bar maximizes, as on Windows
                   (the Mac zooms, which is the same thing here). */
                if (last_title_view == v && ev->time_msec - last_title_click < 400) {
                    last_title_view = NULL;
                    view_toggle_maximize(v);
                    return;
                }
                last_title_view = v;
                last_title_click = ev->time_msec;
                begin_interactive(v, CUR_MOVE, 0);
            }
            return;
        }
    }
    if (owner && *owner == K_VIEW) {
        struct view *v = (struct view *)owner;
        if (v != srv.focused)
            focus_view(v);
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
        if (kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_ALT)) {
            if (ev->button == BTN_LEFT)
                begin_interactive(v, CUR_MOVE, 0);
            else if (ev->button == BTN_RIGHT)
                begin_interactive(v, CUR_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
            return;
        }
    } else if (owner && *owner == K_LAYER && surface) {
        struct layer *ly = (struct layer *)owner;
        if (ly->ls->current.keyboard_interactive) {
            struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
            if (kb)
                wlr_seat_keyboard_notify_enter(srv.seat, ly->ls->surface, kb->keycodes,
                                               kb->num_keycodes, &kb->modifiers);
        }
    }
    wlr_seat_pointer_notify_button(srv.seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_axis_event *ev = data;
    if (srv.mc.open)
        return;
    wlr_seat_pointer_notify_axis(srv.seat, ev->time_msec, ev->orientation, ev->delta,
                                 ev->delta_discrete, ev->source);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
    (void)l;
    (void)data;
    wlr_seat_pointer_notify_frame(srv.seat);
}

/* --- touchpad gestures ------------------------------------------------------------ */

static void swipe_begin(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_swipe_begin_event *ev = data;
    srv.swipe_fingers = (int)ev->fingers;
    srv.swipe_dx = srv.swipe_dy = 0;
}

static void swipe_update(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_swipe_update_event *ev = data;
    srv.swipe_dx += ev->dx;
    srv.swipe_dy += ev->dy;
}

static void swipe_end(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_pointer_swipe_end_event *ev = data;
    if (!ev->cancelled)
        swipe(srv.swipe_fingers, srv.swipe_dx, srv.swipe_dy);
    srv.swipe_fingers = 0;
}

/* --- devices and the seat --------------------------------------------------------- */

static void new_input(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_input_device *dev = data;
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
        new_keyboard(dev);
    else if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(srv.cursor, dev);
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&srv.keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(srv.seat, caps);
}

static void request_cursor(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    if (srv.seat->pointer_state.focused_client == ev->seat_client && !srv.mc.open) {
        cursor_name = "";
        wlr_cursor_set_surface(srv.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
    }
}

static void request_selection(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(srv.seat, ev->source, ev->serial);
}

static void request_primary(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_seat_request_set_primary_selection_event *ev = data;
    wlr_seat_set_primary_selection(srv.seat, ev->source, ev->serial);
}

/* X11 programs that don't set a cursor get ours. */
static void xwayland_ready(struct wl_listener *l, void *data)
{
    (void)l;
    (void)data;
    wlr_xwayland_set_seat(srv.xwayland, srv.seat);
    struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(srv.cursor_mgr, "default", 1);
    if (xc && xc->image_count) {
        struct wlr_xcursor_image *im = xc->images[0];
        wlr_xwayland_set_cursor(srv.xwayland, im->buffer, im->width * 4, im->width, im->height,
                                (int32_t)im->hotspot_x, (int32_t)im->hotspot_y);
    }
}

void input_init(void)
{
    srv.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(srv.cursor, srv.output_layout);
    srv.cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24);
    wlr_xcursor_manager_load(srv.cursor_mgr, srv.scale);

    static struct wl_listener l_motion = {.notify = cursor_motion},
                              l_abs = {.notify = cursor_motion_abs},
                              l_button = {.notify = cursor_button},
                              l_axis = {.notify = cursor_axis},
                              l_frame = {.notify = cursor_frame},
                              l_sw_begin = {.notify = swipe_begin},
                              l_sw_update = {.notify = swipe_update},
                              l_sw_end = {.notify = swipe_end};
    wl_signal_add(&srv.cursor->events.motion, &l_motion);
    wl_signal_add(&srv.cursor->events.motion_absolute, &l_abs);
    wl_signal_add(&srv.cursor->events.button, &l_button);
    wl_signal_add(&srv.cursor->events.axis, &l_axis);
    wl_signal_add(&srv.cursor->events.frame, &l_frame);
    wl_signal_add(&srv.cursor->events.swipe_begin, &l_sw_begin);
    wl_signal_add(&srv.cursor->events.swipe_update, &l_sw_update);
    wl_signal_add(&srv.cursor->events.swipe_end, &l_sw_end);

    wl_list_init(&srv.keyboards);
    static struct wl_listener l_input = {.notify = new_input};
    wl_signal_add(&srv.backend->events.new_input, &l_input);
    srv.seat = wlr_seat_create(srv.display, "seat0");
    static struct wl_listener l_cursor = {.notify = request_cursor},
                              l_sel = {.notify = request_selection},
                              l_prim = {.notify = request_primary};
    wl_signal_add(&srv.seat->events.request_set_cursor, &l_cursor);
    wl_signal_add(&srv.seat->events.request_set_selection, &l_sel);
    wl_signal_add(&srv.seat->events.request_set_primary_selection, &l_prim);

    if (srv.xwayland) {
        static struct wl_listener l_ready = {.notify = xwayland_ready};
        wl_signal_add(&srv.xwayland->events.ready, &l_ready);
    }
}
