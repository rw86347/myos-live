/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * scale.h — the desktop's scale and pointer size settings, shared by
 * myos-xsettings (which applies them) and myos-settings (where people
 * choose them).
 *
 * The scale is a percentage, 100 to 300, in steps of 25 (the theme has a
 * set of pictures for each). Past 200 GTK still draws whole windows 2x
 * and the fonts grow the rest of the way, so every program that sizes its
 * own drawing by pct / 200 above 200 goes on working: small, sharp
 * screens (the GPD P2 Max: 2560 x 1600 at 8.9") want 250-300.
 * 0 means "automatic": whatever
 * auto_scale() picks for the screen. It is kept per user, in
 * ~/.config/myos/display.conf, as "scale=150" or "scale=auto".
 */
#ifndef MYOS_SCALE_H
#define MYOS_SCALE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SCALE_MIN 100
#define SCALE_MAX 300
#define SCALE_STEP 25

/* What "automatic" means: the size that makes a 1920x1200 panel (a modern
   13-14" laptop, or a VM window on one) about as roomy as a 1280x800 one,
   and leaves the screens of 2008 (1024x768 to 1680x1050) at or near 100%.
   X can't tell us the panel's real size in a VM, so this goes by pixels. */
static inline int auto_scale(int width)
{
    return width >= 3000 ? 200 : width >= 2400 ? 175 : width >= 1800 ? 150 : width >= 1600 ? 125 : 100;
}

static inline int clamp_scale(int s)
{
    return s < SCALE_MIN ? SCALE_MIN : s > SCALE_MAX ? SCALE_MAX : s;
}

static inline void scale_conf_path(char *path, size_t size, const char *leaf)
{
    const char *home = getenv("HOME");
    snprintf(path, size, "%s/.config%s%s", home ? home : "", leaf ? "/" : "", leaf ? leaf : "");
}

/* 0 = automatic (also when there is no file). */
static inline int scale_read_conf(void)
{
    char path[512], line[128];
    int s = 0;
    scale_conf_path(path, sizeof path, "myos/display.conf");
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "scale=", 6))
            s = strncmp(line + 6, "auto", 4) ? clamp_scale(atoi(line + 6)) : 0;
    fclose(f);
    return s;
}

static inline int scale_write_conf(int s)
{
    char path[512];
    scale_conf_path(path, sizeof path, NULL);
    mkdir(path, 0755);
    scale_conf_path(path, sizeof path, "myos");
    mkdir(path, 0755);
    scale_conf_path(path, sizeof path, "myos/display.conf");
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    if (s)
        fprintf(f, "scale=%d\n", s);
    else
        fprintf(f, "scale=auto\n");
    return fclose(f);
}

/* The mouse pointer's size, Settings > Mouse & Keyboard: a percentage of
   the normal size, which itself follows the desktop's scale (24 pixels at
   100%). Kept in ~/.config/myos/mouse.conf as "pointer=200". The pointer
   pictures are Adwaita's (on the disk for GTK), drawn at 24, 32, 48, 64
   and 96 pixels; X picks the nearest. */
#define POINTER_MIN 100
#define POINTER_MAX 300
#define POINTER_BASE 24

static inline int clamp_pointer(int p)
{
    return p < POINTER_MIN ? POINTER_MIN : p > POINTER_MAX ? POINTER_MAX : p;
}

/* The pointer in pixels, for a desktop scale and a pointer size. */
static inline int pointer_pixels(int scale, int pointer)
{
    return POINTER_BASE * scale / 100 * pointer / 100;
}

/* How fast the pointer moves, 1 (slow) to 10, and how much a quick move
   carries it further (acceleration), 0 (none) to 10; 5 and 5 are X's own
   behaviour. Also in mouse.conf: "speed=5", "accel=5". myos-xsettings
   turns them into X's settings (mouse_deceleration, mouse_accel). */
#define SPEED_MIN 1
#define SPEED_MAX 10
#define ACCEL_MIN 0
#define ACCEL_MAX 10
#define SPEED_NORMAL 5
#define ACCEL_NORMAL 5

/* And "scroll=1": two fingers on a touchpad scroll (0: they point, as one
   does). The kernel does it (drivers/hid/hid.c); this tells it. */
struct mouse_conf {
    int pointer, speed, accel, scroll;
};

#define TWO_FINGER_SCROLL "/sys/module/hid/parameters/two_finger_scroll"

static inline void touchpad_apply(int scroll)
{
    FILE *f = fopen(TWO_FINGER_SCROLL, "w");
    if (f) {
        fputs(scroll ? "Y\n" : "N\n", f);
        fclose(f);
    }
}

static inline int clamp_to(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline struct mouse_conf mouse_read_conf(void)
{
    char path[512], line[128];
    struct mouse_conf m = { POINTER_MIN, SPEED_NORMAL, ACCEL_NORMAL, 1 };
    scale_conf_path(path, sizeof path, "myos/mouse.conf");
    FILE *f = fopen(path, "r");
    if (!f)
        return m;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "pointer=", 8))
            m.pointer = clamp_pointer(atoi(line + 8));
        else if (!strncmp(line, "speed=", 6))
            m.speed = clamp_to(atoi(line + 6), SPEED_MIN, SPEED_MAX);
        else if (!strncmp(line, "accel=", 6))
            m.accel = clamp_to(atoi(line + 6), ACCEL_MIN, ACCEL_MAX);
        else if (!strncmp(line, "scroll=", 7))
            m.scroll = atoi(line + 7) != 0;
    }
    fclose(f);
    return m;
}

static inline int mouse_write_conf(struct mouse_conf m)
{
    char path[512];
    scale_conf_path(path, sizeof path, NULL);
    mkdir(path, 0755);
    scale_conf_path(path, sizeof path, "myos");
    mkdir(path, 0755);
    scale_conf_path(path, sizeof path, "myos/mouse.conf");
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "pointer=%d\nspeed=%d\naccel=%d\nscroll=%d\n", m.pointer, m.speed, m.accel, m.scroll);
    return fclose(f);
}

static inline int pointer_read_conf(void)
{
    return mouse_read_conf().pointer;
}

static inline int pointer_write_conf(int p)
{
    struct mouse_conf m = mouse_read_conf();
    m.pointer = p;
    return mouse_write_conf(m);
}

/* Speed s as X's "constant deceleration" (2 halves the speed): about
   0.4x to 3x around 1x at 5, in steps a third of a doubling apart. */
static inline float mouse_deceleration(int s)
{
    static const float d[SPEED_MAX + 1] = { 1, 2.5f, 2.0f, 1.6f, 1.25f, 1.0f, 0.8f, 0.63f, 0.5f,
                                            0.4f, 0.32f };
    return d[clamp_to(s, SPEED_MIN, SPEED_MAX)];
}

/* Acceleration a as X's pointer control: past 4 pixels in one move, the
   rest goes num/10 times as far (X's default is 2: a = 5). 0: none. */
static inline int mouse_accel(int a)
{
    return 10 + 2 * clamp_to(a, ACCEL_MIN, ACCEL_MAX);
}

#endif
