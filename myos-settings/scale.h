/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * scale.h — the desktop's scale and pointer size settings, shared by
 * myos-xsettings (which applies them) and myos-settings (where people
 * choose them).
 *
 * The scale is a percentage, 100 to 200. 0 means "automatic": whatever
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
#define SCALE_MAX 200

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

static inline int pointer_read_conf(void)
{
    char path[512], line[128];
    int p = POINTER_MIN;
    scale_conf_path(path, sizeof path, "myos/mouse.conf");
    FILE *f = fopen(path, "r");
    if (!f)
        return p;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "pointer=", 8))
            p = clamp_pointer(atoi(line + 8));
    fclose(f);
    return p;
}

static inline int pointer_write_conf(int p)
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
    fprintf(f, "pointer=%d\n", p);
    return fclose(f);
}

#endif
