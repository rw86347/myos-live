/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* layout.h — where Mission Control puts the windows (layout.c). Plain C, no
   wlroots, so layout_test.c can check it on the build machine. */
#ifndef MYOS_WM_LAYOUT_H
#define MYOS_WM_LAYOUT_H

#define MC_MAX 64                   /* windows on one desktop that get laid out */

struct mc_rect {
    int x, y, w, h;
};

/* Lays out n windows: in[] is where they are, out[] where Mission Control
   shows them, inside area, at least gap pixels apart. Returns the scale
   (the same for every window, never above 1). */
double mc_layout(const struct mc_rect *in, struct mc_rect *out, int n,
                 struct mc_rect area, int gap);

#endif
