/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * wm.h — myos-wm, MyOS's Wayland window server: what its files share.
 *
 *   main.c     start-up: the display, outputs, the frame loop, layer-shell
 *   view.c     windows (xdg-shell and XWayland), title bars, focus, the taskbar's list
 *   input.c    keyboard, mouse and touchpad
 *   draw.c     pictures: our own buffers, the curl, title bars, labels, the wallpaper
 *   mission.c  Mission Control, the Spaces bar, desktop switching, Quick Look
 *   layout.c   where Mission Control puts the windows (tested on the host)
 *
 * The picture on the screen is a wlroots scene graph. From the bottom up:
 *   wallpaper, background layer, curl, bottom layer, desktops (one tree
 *   each, only the current one shown), X11 menus, Mission Control, top
 *   layer (the taskbar), overlay layer.
 * A desktop that isn't shown is a disabled tree: its windows keep their
 * last picture and get no frame callbacks, so they stop drawing and cost
 * nothing.
 */
#ifndef MYOS_WM_H
#define MYOS_WM_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include "layout.h"

struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_output;

#define MAX_SPACES 16
#define TITLE_H    30               /* the theme's title bar, at scale 1 */
#define FRAME_W    1                /* the window frame (1.5 px in the theme) */
#define CURL_MIN   24               /* the hot corner's curl: resting ... */
#define CURL_MAX   40               /* ... and under the pointer */
#define CURL_MS    120              /* how long it takes to grow */
#define MC_MS      250              /* Mission Control's zoom */
#define SLIDE_MS   350              /* the slide between desktops */
#define BAR_MS     200              /* the Spaces bar growing into miniatures */

/* From Linux's input-event-codes.h and drm_fourcc.h (the live build has no
   kernel headers; these numbers are ABI and never change). */
#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111
#define DRM_FORMAT_ARGB8888 0x34325241  /* 'AR24' */
#define DRM_FORMAT_XRGB8888 0x34325258  /* 'XR24' */

/* What a scene node is, for the pointer: every node.data we set points
   at a struct whose first member is one of these. */
enum kind {
    K_VIEW = 1,                     /* a window (struct view) */
    K_LAYER,                        /* a layer-shell surface (struct layer) */
    K_UNMANAGED,                    /* an X11 menu or tooltip (struct view) */
    K_HIT,                          /* one of our own buttons (struct hit) */
};

enum hit_what {
    H_CURL = 1,
    H_TITLE,                        /* a window's title bar: drag to move */
    H_MIN, H_MAX, H_CLOSE,          /* its buttons */
    H_THUMB,                        /* a window in Mission Control */
    H_SPACE,                        /* a desktop in the Spaces bar */
    H_SPACE_CLOSE,                  /* its x */
    H_SPACE_ADD,                    /* the + */
};

struct hit {
    enum kind kind;                 /* K_HIT */
    enum hit_what what;
    struct view *view;              /* for the title bar and thumbs */
    int index;                      /* for the Spaces bar */
};

/* A picture we draw: a wlr_buffer whose pixels are plain memory (ARGB,
   premultiplied), so cairo can draw into it and the pixman renderer
   reads it in place. */
struct pixbuf {
    struct wlr_buffer *base;        /* points into the private struct in draw.c */
    uint32_t *data;
    int w, h, stride;
};

/* Mission Control's copy of one window. */
struct thumb {
    struct wlr_scene_tree *tree;    /* in mc.windows */
    struct wlr_scene_rect *ring;    /* the blue highlight, behind */
    struct wlr_scene_buffer *deco;  /* the title bar, scaled */
    struct wlr_scene_buffer *body;  /* the window's picture, scaled */
    struct pixbuf *mip[2];          /* half and quarter size copies (or NULL) */
    int mip_used;                   /* which picture body shows: 0 live, 1 half, 2 quarter */
    bool mip_stale;                 /* the window drew since the copies were made */
    struct mc_rect from, to_c, to_e;  /* real place; laid out with the bar small / big */
    struct mc_rect from_shown;      /* where it was when a re-layout began */
    bool relayout;                  /* gliding from from_shown to its new place */
    struct hit hit;
};

enum view_type { VIEW_XDG, VIEW_X11 };

struct view {
    enum kind kind;                 /* K_VIEW or K_UNMANAGED */
    struct wl_list link;            /* server.views: front first */
    enum view_type type;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_xwayland_surface *xw;
    struct wlr_surface *surface;    /* while mapped */

    struct wlr_scene_tree *tree;    /* in the desktop's tree: frame + content */
    struct wlr_scene_tree *content; /* the client's surfaces */
    struct wlr_scene_rect *frame;   /* the 1 px frame, behind */
    struct wlr_scene_buffer *title; /* our title bar (NULL without decorations) */
    struct pixbuf *title_pic;
    struct hit title_hit;
    int hover_button;               /* H_MIN, H_MAX, H_CLOSE or 0 */
    int pressed_button;

    int space;                      /* which desktop */
    int x, y;                       /* the content's top-left, layout coordinates */
    int w, h;                       /* the content's size */
    bool mapped, ssd, maximized, minimized;
    struct wlr_box saved;           /* where it was before maximizing */

    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct thumb mc;                /* while Mission Control shows it */

    struct wl_listener map, unmap, destroy, commit;
    struct wl_listener request_move, request_resize, request_maximize,
        request_minimize, request_fullscreen, set_title, set_app_id;
    struct wl_listener associate, dissociate, request_configure, request_activate,
        set_decorations;
    struct wl_listener ftl_activate, ftl_minimize, ftl_maximize, ftl_close;
};

struct layer {
    enum kind kind;                 /* K_LAYER */
    struct wl_list link;
    struct wlr_layer_surface_v1 *ls;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wlr_scene_tree *popups;
    struct wl_listener map, unmap, destroy, commit, new_popup;
};

struct space {
    struct wlr_scene_tree *tree;
    char name[32];
};

struct output {
    struct wl_list link;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wlr_box full, usable;    /* layout coordinates; usable leaves out the taskbar */
    struct wl_listener frame, request_state, destroy;
};

enum cursor_mode { CUR_PASS, CUR_MOVE, CUR_RESIZE, CUR_BUTTON, CUR_MC_DRAG, CUR_BAR_DRAG };

/* Mission Control's state. */
struct mc {
    bool open;                      /* or opening */
    bool expose;                    /* App Exposé: only one program's windows */
    char app[64];                   /* ... this one's */
    double p;                       /* 0 = windows where they are, 1 = laid out */
    double bar;                     /* 0 = names only, 1 = miniatures */
    int p_dir, bar_dir;             /* where p and bar are heading: +1, -1, 0 */
    uint32_t last_ms;               /* the last frame that moved them */
    struct wlr_scene_tree *tree;    /* everything below, in the scene */
    struct wlr_scene_rect *dim;
    struct wlr_scene_tree *windows;
    struct wlr_scene_tree *bar_tree;
    struct wlr_scene_rect *bar_glass;   /* the bar's smoked glass */
    struct wlr_scene_buffer *label; /* the hovered window's name */
    struct pixbuf *label_pic;
    struct view *hover;
    struct view *quick;             /* Quick Look shows this one big */
    double quick_p;
    struct view *drag;              /* a window being dragged to another desktop */
    int drag_dx, drag_dy;
    int drag_space;                 /* a desktop being dragged along the bar, or -1 */
    struct {
        struct wlr_scene_tree *tree;
        struct wlr_scene_rect *back;
        struct wlr_scene_buffer *name;
        struct wlr_scene_buffer *wall;
        struct wlr_scene_tree *minis;
        struct wlr_scene_buffer *close;
        struct hit hit, close_hit;
        struct pixbuf *name_pic;
        struct wlr_box box;         /* where it is, while the bar is big */
    } slot[MAX_SPACES];
    struct wlr_scene_buffer *add;
    struct hit add_hit;
    struct pixbuf *add_pic, *close_pic;
    struct wl_event_source *mip_timer, *mini_timer;
    int frames_over;                /* zoom frames that took more than 16 ms */
};

/* Sliding from one desktop to the next. */
struct slide {
    bool on;
    int from, to;
    double p;
    uint32_t last_ms;
};

struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wl_list outputs;

    /* The scene's layers, bottom to top. */
    struct wlr_scene_tree *bg, *layer_bg, *curl_tree, *layer_bottom, *spaces_root,
        *unmanaged, *layer_top, *layer_overlay;
    struct wlr_scene_buffer *wallpaper;
    struct pixbuf *wall_pic;        /* the wallpaper at the output's size */

    struct space spaces[MAX_SPACES];
    int nspaces, cur;
    struct wl_list views;           /* struct view.link, front first */
    struct wl_list layers;
    struct view *focused;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_xwayland *xwayland;
    struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;
    struct wlr_server_decoration_manager *kde_deco;
    struct wlr_xdg_decoration_manager_v1 *xdg_deco;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    struct wl_list keyboards;
    enum cursor_mode cursor_mode;
    struct view *grab;
    double grab_x, grab_y;
    struct wlr_box grab_box;
    uint32_t resize_edges;
    double swipe_dx, swipe_dy;      /* a three-finger swipe so far */
    int swipe_fingers;

    /* The hot corner. */
    struct wlr_scene_buffer *curl;
    struct pixbuf *curl_pics[CURL_MAX - CURL_MIN + 2];  /* hover sizes 24..40, then resting 24 */
    struct hit curl_hit;
    double curl_p;                  /* 0 resting .. 1 grown */
    int curl_dir;
    uint32_t curl_ms;

    struct mc mc;
    struct slide slide;
    bool reduce_motion;             /* no zooms or slides: set, or measured */
    bool reduce_motion_auto;        /* measured too slow this session */
    float scale;                    /* the output's scale (2 on big screens) */
};

extern struct server srv;

/* main.c */
uint32_t now_ms(void);
struct output *primary_output(void);
void schedule_frame(void);
void arrange_layers(struct output *o);
void spawn(const char *cmd);

/* view.c */
void view_init(void);
void focus_view(struct view *v);
void view_close(struct view *v);
void view_set_minimized(struct view *v, bool min);
void view_toggle_maximize(struct view *v);
void view_move_to_space(struct view *v, int space);
void view_set_position(struct view *v, int x, int y);
void view_configure(struct view *v, int x, int y, int w, int h);
void view_redraw_title(struct view *v);
enum kind *owner_at(double lx, double ly, struct wlr_surface **surface,
                    double *sx, double *sy);
const char *view_title(struct view *v);
const char *view_app(struct view *v);
void view_send_frame(struct view *v);
bool view_on_screen(struct view *v);
void begin_interactive(struct view *v, enum cursor_mode mode, uint32_t edges);

/* input.c */
void input_init(void);
void pointer_refocus(void);

/* draw.c */
struct pixbuf *pixbuf_new(int w, int h);
void pixbuf_drop(struct pixbuf *p);
void pixbuf_damage(struct wlr_scene_buffer *sb, struct pixbuf *p);
void draw_curl_pics(void);
void draw_title(struct view *v);
struct pixbuf *draw_label(const char *text, struct wlr_surface *icon_from, const char *app,
                          int max_w);
struct pixbuf *draw_space_name(const char *name, bool current, int w, int h);
struct pixbuf *draw_round_button(int size, bool plus);
void draw_wallpaper(int w, int h);
void draw_scaled_copy(struct pixbuf *dst, struct wlr_buffer *src);

/* mission.c */
void mc_init(void);
void mc_toggle(bool expose);
void mc_frame(uint32_t ms);
bool mc_animating(void);
bool mc_motion(double x, double y);
bool mc_button(uint32_t button, bool pressed);
bool mc_key(uint32_t sym);
void mc_view_gone(struct view *v);
void mc_view_commit(struct view *v);
void curl_hover(bool in);
void curl_frame(uint32_t ms);
void space_switch(int to);
int space_add(void);
void space_remove(int i);
void swipe(int fingers, double dx, double dy);
void read_motion_setting(void);

#endif
