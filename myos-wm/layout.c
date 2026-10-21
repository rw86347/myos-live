/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* layout.c — where Mission Control puts the windows.
 *
 * What the Mac does, and so what this does:
 *   - no two windows overlap;
 *   - every window is shrunk by the same amount, so the big ones stay the
 *     biggest, and none is ever shown larger than it really is;
 *   - each one stays roughly where it was: windows higher on the screen go
 *     in higher rows, and in a row they keep their left-to-right order.
 *
 * How: sort the windows by the height of their middle and cut that list
 * into rows (try 1 row, 2 rows, ... n rows); in each row, sort by the
 * middle's x. A row's windows sit side by side, gap pixels apart; the rows
 * sit one above the other. The scale is the largest at which the widest
 * row and the stack of rows both fit the area. Keep the row count with the
 * largest scale (the fewer rows on a tie), centre every row, and centre
 * each window in its row's height.
 *
 * Cost: n row counts, each O(n log n) with n at most MC_MAX — a few
 * thousand steps, once when Mission Control opens. Nothing is allocated. */
#include "layout.h"

/* Insertion sort of idx[0..n) by key[]: n is small, and it is stable, so
   windows with the same middle keep their stacking order. */
static void sort_by(int *idx, int n, const int *key)
{
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i;
        while (j > 0 && key[idx[j - 1]] > key[v]) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
}

/* One try: cut the windows (by_y, sorted by middle y) into at most rows
   rows. Fills row_of[] and returns the scale at which it fits. */
static double try_rows(const struct mc_rect *in, const int *by_y, int n, int rows,
                       struct mc_rect area, int gap, int *row_of, int *nrows)
{
    long total = 0;
    for (int i = 0; i < n; i++)
        total += in[i].w;
    double ideal = (double)total / rows;
    int row = 0;
    double width = 0;
    for (int k = 0; k < n; k++) {
        int i = by_y[k];
        /* Start a new row when this window's middle would pass the ideal width. */
        if (width > 0 && width + in[i].w / 2.0 > ideal && row < rows - 1) {
            row++;
            width = 0;
        }
        row_of[i] = row;
        width += in[i].w;
    }
    *nrows = row + 1;

    double scale = 1.0;
    long height = 0;
    for (int r = 0; r < *nrows; r++) {
        long w = 0;
        int count = 0, h = 0;
        for (int i = 0; i < n; i++)
            if (row_of[i] == r) {
                w += in[i].w;
                count++;
                if (in[i].h > h)
                    h = in[i].h;
            }
        height += h;
        double s = (double)(area.w - gap * (count - 1)) / (double)w;
        if (s < scale)
            scale = s;
    }
    double s = (double)(area.h - gap * (*nrows - 1)) / (double)height;
    return s < scale ? s : scale;
}

double mc_layout(const struct mc_rect *in, struct mc_rect *out, int n,
                 struct mc_rect area, int gap)
{
    if (n <= 0)
        return 1.0;
    if (n > MC_MAX)
        n = MC_MAX;
    int mid_x[MC_MAX], mid_y[MC_MAX], by_y[MC_MAX], row_of[MC_MAX], best_row[MC_MAX];
    for (int i = 0; i < n; i++) {
        mid_x[i] = in[i].x + in[i].w / 2;
        mid_y[i] = in[i].y + in[i].h / 2;
        by_y[i] = i;
    }
    sort_by(by_y, n, mid_y);

    double best = -1;
    int best_rows = 1;
    for (int rows = 1; rows <= n; rows++) {
        int got;
        double s = try_rows(in, by_y, n, rows, area, gap, row_of, &got);
        if (s > best + 1e-9) {
            best = s;
            best_rows = got;
            for (int i = 0; i < n; i++)
                best_row[i] = row_of[i];
        }
        if (got < rows)                 /* more rows can't be made */
            break;
    }
    double scale = best > 0 ? best : 0.01;

    /* Place: rows centred top to bottom, windows centred in each row. */
    int row_h[MC_MAX], total_h = gap * (best_rows - 1);
    for (int r = 0; r < best_rows; r++) {
        row_h[r] = 0;
        for (int i = 0; i < n; i++)
            if (best_row[i] == r && (int)(in[i].h * scale) > row_h[r])
                row_h[r] = (int)(in[i].h * scale);
        total_h += row_h[r];
    }
    int y = area.y + (area.h - total_h) / 2;
    for (int r = 0; r < best_rows; r++) {
        int idx[MC_MAX], count = 0, row_w = 0;
        for (int i = 0; i < n; i++)
            if (best_row[i] == r) {
                idx[count++] = i;
                row_w += (int)(in[i].w * scale);
            }
        sort_by(idx, count, mid_x);
        row_w += gap * (count - 1);
        int x = area.x + (area.w - row_w) / 2;
        for (int k = 0; k < count; k++) {
            int i = idx[k];
            out[i].w = (int)(in[i].w * scale);
            out[i].h = (int)(in[i].h * scale);
            if (out[i].w < 1)
                out[i].w = 1;
            if (out[i].h < 1)
                out[i].h = 1;
            out[i].x = x;
            out[i].y = y + (row_h[r] - out[i].h) / 2;
            x += out[i].w + gap;
        }
        y += row_h[r] + gap;
    }
    return scale;
}
