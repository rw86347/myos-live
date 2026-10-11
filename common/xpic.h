/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * xpic.h — pictures in memory, to and from the X server. Shared by
 * myos-login and myos-menu.
 *
 * Both programs compose what they show in their own 0xAARRGGBB buffers and
 * hand the result over with XPutImage. (Our kernel has no SysV shared
 * memory, so the MIT-SHM shortcut most toolkits use is not there; plain
 * XPutImage over the local socket is what works.)
 */
#ifndef XPIC_H
#define XPIC_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One colour channel in this screen's format. Our own machines are
   TrueColor 888, but an old laptop's framebuffer can be 16-bit 565. */
static unsigned long xpic_channel(int v, unsigned long mask)
{
    if (!mask)
        return 0;
    int shift = 0, bits = 0;
    for (unsigned long m = mask; !(m & 1); m >>= 1)
        shift++;
    for (unsigned long m = mask >> shift; m & 1; m >>= 1)
        bits++;
    return (unsigned long)(v >> (8 - bits)) << shift;
}

/* 0xAARRGGBB is exactly what a 24-bit TrueColor screen stores, so on our
   screens a picture's rows are a straight memcpy either way. */
static int xpic_direct(const XImage *xi, const Visual *vis)
{
    return xi->bits_per_pixel == 32 && xi->byte_order == LSBFirst && vis->red_mask == 0xff0000 &&
           vis->green_mask == 0x00ff00 && vis->blue_mask == 0x0000ff;
}

/* The picture, in a new Pixmap on the server (None if out of memory). */
static Pixmap xpic_pixmap(Display *dpy, Visual *vis, int depth, Drawable on,
                          const uint32_t *px, int w, int h)
{
    XImage *xi = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, w, h, 32, 0);
    if (!xi)
        return None;
    xi->data = malloc((size_t)xi->bytes_per_line * h);
    if (!xi->data) {
        XDestroyImage(xi);
        return None;
    }
    int direct = xpic_direct(xi, vis);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = px + (size_t)y * w;
        if (direct) {
            memcpy(xi->data + (size_t)y * xi->bytes_per_line, row, (size_t)w * 4);
            continue;
        }
        for (int x = 0; x < w; x++)
            XPutPixel(xi, x, y, xpic_channel(row[x] >> 16 & 0xff, vis->red_mask) |
                                xpic_channel(row[x] >> 8 & 0xff, vis->green_mask) |
                                xpic_channel(row[x] & 0xff, vis->blue_mask));
    }
    Pixmap p = XCreatePixmap(dpy, on, w, h, depth);
    GC gc = XCreateGC(dpy, p, 0, NULL);
    XPutImage(dpy, p, gc, xi, 0, 0, 0, 0, w, h);
    XFreeGC(dpy, gc);
    XDestroyImage(xi);              /* frees xi->data too */
    return p;
}

#endif
