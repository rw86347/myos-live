/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* layout_test.c — checks Mission Control's layout on the build machine
   (`make -f live/live.mk wm-layout-test`, part of `make live-test`): random
   desktops of 1 to MC_MAX windows, and the rules layout.c promises. */
#include <stdio.h>
#include <stdlib.h>
#include "layout.h"

static int fails, checks;

static void check(int ok, const char *what, int round, int i)
{
    checks++;
    if (!ok && fails++ < 20)
        printf("layout_test: round %d window %d: %s\n", round, i, what);
}

int main(void)
{
    srand(1);
    struct mc_rect area = { 40, 120, 1840, 1000 };   /* a 1920x1200 screen, top band free */
    const int gap = 24;
    for (int round = 0; round < 2000; round++) {
        int n = 1 + rand() % (round < 1000 ? 12 : MC_MAX);
        struct mc_rect in[MC_MAX], out[MC_MAX];
        for (int i = 0; i < n; i++) {
            in[i].w = 120 + rand() % 1800;
            in[i].h = 80 + rand() % 1100;
            in[i].x = rand() % 1920 - in[i].w / 2;
            in[i].y = rand() % 1200 - in[i].h / 2;
        }
        double s = mc_layout(in, out, n, area, gap);
        check(s > 0 && s <= 1.0, "scale out of range", round, -1);
        for (int i = 0; i < n; i++) {
            check(out[i].x >= area.x && out[i].y >= area.y &&
                  out[i].x + out[i].w <= area.x + area.w &&
                  out[i].y + out[i].h <= area.y + area.h, "outside the area", round, i);
            check(out[i].w <= in[i].w && out[i].h <= in[i].h, "larger than the window", round, i);
            check(abs(out[i].w - (int)(in[i].w * s)) <= 1 && abs(out[i].h - (int)(in[i].h * s)) <= 1,
                  "not the common scale", round, i);
            for (int j = 0; j < i; j++) {
                const struct mc_rect *a = &out[i], *b = &out[j];
                int apart = a->x + a->w + gap <= b->x || b->x + b->w + gap <= a->x ||
                            a->y + a->h + gap <= b->y || b->y + b->h + gap <= a->y;
                check(apart, "overlaps or closer than the gap", round, i);
                /* Same row (their vertical spans overlap): the old
                   left-to-right order of their middles is kept. */
                int same_row = !(a->y + a->h <= b->y || b->y + b->h <= a->y);
                if (same_row) {
                    int was = (in[i].x + in[i].w / 2) - (in[j].x + in[j].w / 2);
                    int now = a->x - b->x;
                    check(was == 0 || (was > 0) == (now > 0), "row order changed", round, i);
                } else {
                    int was = (in[i].y + in[i].h / 2) - (in[j].y + in[j].h / 2);
                    int now = a->y - b->y;
                    check(was == 0 || (was > 0) == (now > 0), "row of a lower window is higher", round, i);
                }
            }
        }
    }
    /* One small window: shown at its real size. */
    struct mc_rect one = { 300, 300, 640, 480 }, got;
    double s = mc_layout(&one, &got, 1, area, gap);
    check(s == 1.0 && got.w == 640 && got.h == 480, "a small window was scaled", -1, 0);

    if (fails) {
        printf("layout_test: %d of %d checks failed\n", fails, checks);
        return 1;
    }
    printf("layout_test: all %d checks passed\n", checks);
    return 0;
}
