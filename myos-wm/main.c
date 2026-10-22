/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-wm — MyOS's Wayland window server, with the Mac's Mission Control.
 *
 * Started by /etc/myos/wayland-session (the login screen's "MyOS (Wayland,
 * preview)"). It began as wlroots' tinywl (CC0) and keeps its shape: one
 * wlroots scene graph that wlroots draws and damage-tracks; we only move
 * nodes around. See wm.h for the files and the scene's layers.
 *
 * Drawing is on the CPU (WLR_RENDERER=pixman: MyOS has no GPU drivers), into
 * dumb buffers on /dev/dri/card0, flipped by the kernel at the next vblank.
 * The frame loop is wlroots': a frame event per vblank while something
 * changed, none while the screen is still — an idle desktop costs nothing.
 *
 * This file: start-up, outputs, the frame loop, layer-shell (the taskbar),
 * and starting programs.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "wm.h"

struct server srv;

uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* Mission Control, the curl and the Spaces bar are laid out on this one. */
struct output *primary_output(void)
{
    if (wl_list_empty(&srv.outputs))
        return NULL;
    struct output *o = wl_container_of(srv.outputs.prev, o, link);
    return o;
}

void schedule_frame(void)
{
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link)
        wlr_output_schedule_frame(o->wlr_output);
}

void spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* --- layer-shell: the taskbar and anything else docked to an edge ----------------- */

static struct wlr_scene_tree *layer_tree(enum zwlr_layer_shell_v1_layer l)
{
    switch (l) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return srv.layer_bg;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return srv.layer_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return srv.layer_top;
    default: return srv.layer_overlay;
    }
}

/* Place every layer surface on o: the ones that reserve an edge first (the
   taskbar), then the rest in what is left. What is left is o->usable, where
   windows are maximized and Mission Control lays out. */
void arrange_layers(struct output *o)
{
    struct wlr_box usable = o->full;
    for (int pass = 0; pass < 2; pass++) {
        struct layer *l;
        wl_list_for_each(l, &srv.layers, link) {
            if (l->ls->output != o->wlr_output || !l->ls->initialized)
                continue;
            bool excl = l->ls->current.exclusive_zone > 0;
            if (excl != (pass == 0))
                continue;
            wlr_scene_layer_surface_v1_configure(l->scene, &o->full, &usable);
        }
    }
    o->usable = usable;
}

static struct output *output_of(struct wlr_output *wo)
{
    struct output *o;
    wl_list_for_each(o, &srv.outputs, link)
        if (o->wlr_output == wo)
            return o;
    return NULL;
}

static void layer_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct layer *ly = wl_container_of(l, ly, commit);
    struct wlr_layer_surface_v1 *ls = ly->ls;
    struct output *o = output_of(ls->output);
    if (!o)
        return;
    if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
        struct wlr_scene_tree *t = layer_tree(ls->current.layer);
        wlr_scene_node_reparent(&ly->scene->tree->node, t);
    }
    if (ls->initial_commit || ls->current.committed)
        arrange_layers(o);
}

static void layer_map(struct wl_listener *l, void *data)
{
    (void)data;
    struct layer *ly = wl_container_of(l, ly, map);
    struct output *o = output_of(ly->ls->output);
    if (o)
        arrange_layers(o);
    if (ly->ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv.seat);
        if (kb)
            wlr_seat_keyboard_notify_enter(srv.seat, ly->ls->surface, kb->keycodes,
                                           kb->num_keycodes, &kb->modifiers);
    }
}

static void layer_unmap(struct wl_listener *l, void *data)
{
    (void)data;
    struct layer *ly = wl_container_of(l, ly, unmap);
    struct output *o = output_of(ly->ls->output);
    if (o)
        arrange_layers(o);
    if (srv.seat->keyboard_state.focused_surface == ly->ls->surface)
        focus_view(srv.focused);
}

static void layer_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct layer *ly = wl_container_of(l, ly, destroy);
    wl_list_remove(&ly->link);
    wl_list_remove(&ly->map.link);
    wl_list_remove(&ly->unmap.link);
    wl_list_remove(&ly->destroy.link);
    wl_list_remove(&ly->commit.link);
    wl_list_remove(&ly->new_popup.link);
    free(ly);
}

static void layer_new_popup(struct wl_listener *l, void *data)
{
    struct layer *ly = wl_container_of(l, ly, new_popup);
    struct wlr_xdg_popup *popup = data;
    struct wlr_scene_tree *t = wlr_scene_xdg_surface_create(ly->scene->tree, popup->base);
    popup->base->data = t;
    /* Keep the menu on the screen. */
    struct output *o = output_of(ly->ls->output);
    if (o) {
        int lx, ly_;
        wlr_scene_node_coords(&ly->scene->tree->node, &lx, &ly_);
        struct wlr_box box = {o->full.x - lx, o->full.y - ly_, o->full.width, o->full.height};
        wlr_xdg_popup_unconstrain_from_box(popup, &box);
    }
}

static void new_layer_surface(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_layer_surface_v1 *ls = data;
    if (!ls->output) {
        struct output *o = primary_output();
        if (!o) {
            wlr_layer_surface_v1_destroy(ls);
            return;
        }
        ls->output = o->wlr_output;
    }
    struct layer *ly = calloc(1, sizeof *ly);
    ly->kind = K_LAYER;
    ly->ls = ls;
    ly->scene = wlr_scene_layer_surface_v1_create(layer_tree(ls->pending.layer), ls);
    ly->scene->tree->node.data = ly;
    ls->data = ly;
    ly->map.notify = layer_map;
    wl_signal_add(&ls->surface->events.map, &ly->map);
    ly->unmap.notify = layer_unmap;
    wl_signal_add(&ls->surface->events.unmap, &ly->unmap);
    ly->commit.notify = layer_commit;
    wl_signal_add(&ls->surface->events.commit, &ly->commit);
    ly->destroy.notify = layer_destroy;
    wl_signal_add(&ls->events.destroy, &ly->destroy);
    ly->new_popup.notify = layer_new_popup;
    wl_signal_add(&ls->events.new_popup, &ly->new_popup);
    wl_list_insert(&srv.layers, &ly->link);
}

/* --- outputs ---------------------------------------------------------------------- */

static void output_frame(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, frame);
    uint32_t ms = now_ms();
    curl_frame(ms);
    mc_frame(ms);

    int64_t t0 = now_us();
    wlr_scene_output_commit(o->scene_output, NULL);
    int64_t took = now_us() - t0;
    /* Reduce motion turns itself on when the zoom can't keep 60 frames a
       second here: a third slow frame in one zoom (one can be a hiccup). */
    if (mc_animating() && took > 16000 && ++srv.mc.frames_over >= 3 && !srv.reduce_motion) {
        srv.reduce_motion = srv.reduce_motion_auto = true;
        wlr_log(WLR_INFO, "frames take %lld us: reduce motion on", (long long)took);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);
    if (mc_animating())
        wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data)
{
    struct output *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *ev = data;
    wlr_output_commit_state(o->wlr_output, ev->state);
}

static void output_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct output *o = wl_container_of(l, o, destroy);
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->link);
    free(o);
}

/* The screen's size changed (or it is new): the pictures sized to it. */
static void output_resized(struct output *o)
{
    struct wlr_box box;
    wlr_output_layout_get_box(srv.output_layout, o->wlr_output, &box);
    o->full = box;
    arrange_layers(o);
    if (o != primary_output())
        return;
    draw_wallpaper(box.width, box.height);
    if (srv.wall_pic) {
        pixbuf_damage(srv.wallpaper, srv.wall_pic);
        wlr_scene_buffer_set_dest_size(srv.wallpaper, box.width, box.height);
        wlr_scene_node_set_position(&srv.wallpaper->node, box.x, box.y);
    }
    wlr_scene_node_set_position(&srv.curl->node, box.x + box.width - (CURL_MAX + 4), box.y);
}

static void new_output(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_output *wo = data;
    wlr_output_init_render(wo, srv.allocator, srv.renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
    if (mode)
        wlr_output_state_set_mode(&st, mode);
    wlr_output_state_set_scale(&st, srv.scale);
    wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);

    struct output *o = calloc(1, sizeof *o);
    o->wlr_output = wo;
    o->frame.notify = output_frame;
    wl_signal_add(&wo->events.frame, &o->frame);
    o->request_state.notify = output_request_state;
    wl_signal_add(&wo->events.request_state, &o->request_state);
    o->destroy.notify = output_destroy;
    wl_signal_add(&wo->events.destroy, &o->destroy);
    wl_list_insert(&srv.outputs, &o->link);

    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(srv.output_layout, wo);
    o->scene_output = wlr_scene_output_create(srv.scene, wo);
    wlr_scene_output_layout_add_output(srv.scene_layout, lo, o->scene_output);
    output_resized(o);
}

/* --- start-up --------------------------------------------------------------------- */

static int on_signal(int sig, void *data)
{
    (void)data;
    if (sig == SIGCHLD) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        return 0;
    }
    wl_display_terminate(srv.display);
    return 0;
}

/* What runs when the session starts: ~/.config/myos/wayland-startup if
   the user has one, else the taskbar (when it is installed). */
static void autostart(const char *extra)
{
    char path[512];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/.config/myos/wayland-startup", home ? home : "/");
    if (access(path, X_OK) == 0)
        spawn(path);
    else if (access("/usr/bin/myos-panel", X_OK) == 0)
        spawn("myos-panel");
    if (extra)
        spawn(extra);
}

int main(int argc, char **argv)
{
    const char *startup = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            startup = argv[++i];
        else {
            fprintf(stderr, "usage: myos-wm [-s command]\n");
            return 2;
        }
    }
    wlr_log_init(getenv("MYOS_WM_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
    const char *sc = getenv("MYOS_SCALE");
    srv.scale = sc ? (float)atof(sc) : 1.0f;
    if (srv.scale < 1 || srv.scale > 3)
        srv.scale = 1;

    srv.display = wl_display_create();
    srv.loop = wl_display_get_event_loop(srv.display);
    srv.backend = wlr_backend_autocreate(srv.display, &srv.session);
    if (!srv.backend) {
        wlr_log(WLR_ERROR, "no backend: is seatd running, and /dev/dri/card0 there?");
        return 1;
    }
    srv.renderer = wlr_renderer_autocreate(srv.backend);
    if (!srv.renderer)
        return 1;
    wlr_renderer_init_wl_display(srv.renderer, srv.display);
    srv.allocator = wlr_allocator_autocreate(srv.backend, srv.renderer);
    if (!srv.allocator)
        return 1;

    srv.compositor = wlr_compositor_create(srv.display, 5, srv.renderer);
    wlr_subcompositor_create(srv.display);
    wlr_data_device_manager_create(srv.display);
    wlr_primary_selection_v1_device_manager_create(srv.display);
    wlr_viewporter_create(srv.display);

    srv.output_layout = wlr_output_layout_create();
    wlr_xdg_output_manager_v1_create(srv.display, srv.output_layout);
    wl_list_init(&srv.outputs);
    wl_list_init(&srv.views);
    wl_list_init(&srv.layers);

    /* The scene, bottom to top (wm.h). */
    srv.scene = wlr_scene_create();
    srv.scene_layout = wlr_scene_attach_output_layout(srv.scene, srv.output_layout);
    struct wlr_scene_tree *root = &srv.scene->tree;
    srv.bg = wlr_scene_tree_create(root);
    srv.wallpaper = wlr_scene_buffer_create(srv.bg, NULL);
    srv.layer_bg = wlr_scene_tree_create(root);
    srv.curl_tree = wlr_scene_tree_create(root);
    srv.layer_bottom = wlr_scene_tree_create(root);
    srv.spaces_root = wlr_scene_tree_create(root);
    srv.unmanaged = wlr_scene_tree_create(root);
    srv.mc.tree = wlr_scene_tree_create(root);
    srv.layer_top = wlr_scene_tree_create(root);
    srv.layer_overlay = wlr_scene_tree_create(root);

    srv.nspaces = 1;
    srv.spaces[0].tree = wlr_scene_tree_create(srv.spaces_root);
    snprintf(srv.spaces[0].name, sizeof srv.spaces[0].name, "Desktop 1");
    /* Four desktops from the start, as IceWM has; the bar's + adds more. */
    while (srv.nspaces < 4)
        space_add();

    draw_curl_pics();
    srv.curl = wlr_scene_buffer_create(srv.curl_tree, srv.curl_pics[CURL_MAX - CURL_MIN + 1]->base);
    wlr_scene_buffer_set_dest_size(srv.curl, CURL_MAX + 4, CURL_MAX + 4);
    srv.curl_hit = (struct hit){.kind = K_HIT, .what = H_CURL};
    srv.curl->node.data = &srv.curl_hit;

    srv.layer_shell = wlr_layer_shell_v1_create(srv.display, 4);
    static struct wl_listener new_layer = {.notify = new_layer_surface};
    wl_signal_add(&srv.layer_shell->events.new_surface, &new_layer);

    view_init();
    input_init();
    mc_init();
    read_motion_setting();

    static struct wl_listener new_out = {.notify = new_output};
    wl_signal_add(&srv.backend->events.new_output, &new_out);

    const char *socket = wl_display_add_socket_auto(srv.display);
    if (!socket) {
        wlr_backend_destroy(srv.backend);
        return 1;
    }
    if (!wlr_backend_start(srv.backend)) {
        wlr_backend_destroy(srv.backend);
        wl_display_destroy(srv.display);
        return 1;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    if (srv.xwayland)
        setenv("DISPLAY", srv.xwayland->display_name, 1);
    wl_event_loop_add_signal(srv.loop, SIGTERM, on_signal, NULL);
    wl_event_loop_add_signal(srv.loop, SIGINT, on_signal, NULL);
    wl_event_loop_add_signal(srv.loop, SIGCHLD, on_signal, NULL);
    wlr_log(WLR_INFO, "myos-wm on WAYLAND_DISPLAY=%s", socket);
    autostart(startup);

    wl_display_run(srv.display);

    wl_display_destroy_clients(srv.display);
    if (srv.xwayland)
        wlr_xwayland_destroy(srv.xwayland);
    wlr_scene_node_destroy(&srv.scene->tree.node);
    wlr_output_layout_destroy(srv.output_layout);
    wl_display_destroy(srv.display);
    return 0;
}
