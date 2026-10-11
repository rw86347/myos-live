/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * programs.h — the Start menu's All Programs flyout.
 *
 * Windows XP's Start menu had a Programs item (called All Programs in the
 * two-column menu) that opened the Start Menu\Programs folder: folders
 * became submenus, shortcuts became entries, and a folder's submenu opened
 * to the right of the row under the mouse. MyOS keeps the same shape. The
 * folders live on disk, so a new program is a new file, not a new theme:
 *
 *   /usr/share/myos/programs/                  every user's tree
 *   ~/.local/share/myos/programs/              this user's, merged on top
 *   /usr/share/applications                    desktop files not already listed,
 *                                              filed into a folder by category
 */
#ifndef PROGRAMS_H
#define PROGRAMS_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

struct programs_env {
    Display *dpy;
    Visual *vis;
    Colormap cmap;
    Window root;
    int screen, depth, sw, sh, scale;
    XftFont *font;
};

/* Read the folders. Call each time the Start menu opens. */
void programs_load(const struct programs_env *env);

/* The Start menu's place on the screen, and the All Programs row inside
   it (coordinates relative to the menu window). */
void programs_place(int menu_x, int menu_y, int menu_w, int menu_h,
                    int item_x, int item_y, int item_w, int item_h);

void programs_close(void);

/* 1: the pointer is on All Programs. 0: it is on a different Start item.
   -1: it is on the menu, but not on an item. */
void programs_hover_item(int on);

/* Open the flyout now, or close it if it is already open. */
void programs_toggle(void);

int programs_open(void);
int programs_owns(Window w);
int programs_hit(int x_root, int y_root);

/* Milliseconds until a hovered folder should open, or -1 to wait for
   the next event. */
int programs_arm_ms(void);
void programs_tick(void);

/* An event delivered to a flyout. 1: launch cmd. -1: close the Start
   menu. 0: still open. */
int programs_event(XEvent *ev, char *cmd, int cmdlen);

/* A key while a flyout is open. Same codes as programs_event. */
int programs_key(unsigned long keysym, char *cmd, int cmdlen);

#endif
