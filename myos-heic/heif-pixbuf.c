/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * heif-pixbuf.c — libheif → GdkPixbuf (primary image, orientation applied).
 */
#include <libheif/heif.h>
#include <math.h>
#include <string.h>

#include "heif-pixbuf.h"

static void set_err(GError **err, struct heif_error e)
{
    if (!err || *err)
        return;
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                e.message && e.message[0] ? e.message : "HEIF decode failed");
}

static gboolean brand_match(const guchar *p)
{
    static const char *const brands[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "hvc1",
                                         "avif", "avifs", "mif1", "msf1", NULL};
    for (int i = 0; brands[i]; i++)
        if (!memcmp(p, brands[i], 4))
            return TRUE;
    return FALSE;
}

gboolean myos_heif_sniff(const guchar *head, gsize n)
{
    if (n < 12 || memcmp(head + 4, "ftyp", 4))
        return FALSE;
    if (brand_match(head + 8))
        return TRUE;
    /* Compatible brands follow the major brand in the ftyp box. */
    for (gsize off = 16; off + 4 <= n; off += 4)
        if (brand_match(head + off))
            return TRUE;
    return FALSE;
}

gboolean myos_heif_path(const char *path)
{
    if (!path)
        return FALSE;
    return g_str_has_suffix(path, ".heic") || g_str_has_suffix(path, ".HEIC") ||
           g_str_has_suffix(path, ".heif") || g_str_has_suffix(path, ".HEIF") ||
           g_str_has_suffix(path, ".avif") || g_str_has_suffix(path, ".AVIF") ||
           g_str_has_suffix(path, ".hif") || g_str_has_suffix(path, ".HIF");
}

static GdkPixbuf *from_heif_image(struct heif_image *img, GError **err)
{
    int w = heif_image_get_width(img, heif_channel_interleaved);
    int h = heif_image_get_height(img, heif_channel_interleaved);
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) {
        if (err && !*err)
            g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "invalid HEIF image size");
        return NULL;
    }
    int stride = 0;
    const uint8_t *src = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    if (!src) {
        if (err && !*err)
            g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "HEIF has no pixel data");
        return NULL;
    }
    enum heif_chroma chroma = heif_image_get_chroma_format(img);
    gboolean alpha = chroma == heif_chroma_interleaved_RGBA;
    int spp = alpha ? 4 : 3;
    GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if (!p)
        return NULL;
    guchar *dst = gdk_pixbuf_get_pixels(p);
    int drs = gdk_pixbuf_get_rowstride(p);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        guchar *drow = dst + (size_t)y * drs;
        for (int x = 0; x < w; x++) {
            drow[x * 4] = row[x * spp];
            drow[x * 4 + 1] = row[x * spp + 1];
            drow[x * 4 + 2] = row[x * spp + 2];
            drow[x * 4 + 3] = alpha ? row[x * spp + 3] : 255;
        }
    }
    return p;
}

GdkPixbuf *myos_heif_pixbuf_from_file(const char *path, GError **err)
{
    struct heif_context *ctx = heif_context_alloc();
    struct heif_image_handle *handle = NULL;
    struct heif_image *img = NULL;
    struct heif_error e = heif_context_read_from_file(ctx, path, NULL);
    if (e.code)
        goto fail;
    e = heif_context_get_primary_image_handle(ctx, &handle);
    if (e.code)
        goto fail;
    e = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
    if (e.code)
        e = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, NULL);
    if (e.code)
        goto fail;
    /* heif_decode_image applies irot/imir from the file (see libheif docs). */
    GdkPixbuf *raw = from_heif_image(img, err);
    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return raw;
fail:
    set_err(err, e);
    if (img)
        heif_image_release(img);
    if (handle)
        heif_image_handle_release(handle);
    heif_context_free(ctx);
    return NULL;
}

GdkPixbuf *myos_heif_pixbuf_from_file_at_scale(const char *path, int width, int height,
                                               gboolean preserve_aspect_ratio, GError **err)
{
    GdkPixbuf *full = myos_heif_pixbuf_from_file(path, err);
    if (!full)
        return NULL;
    int w = gdk_pixbuf_get_width(full), h = gdk_pixbuf_get_height(full);
    if (width <= 0)
        width = w;
    if (height <= 0)
        height = h;
    double sx = (double)width / w, sy = (double)height / h;
    double s = preserve_aspect_ratio ? fmin(sx, sy) : fmax(sx, sy);
    if (s >= 0.999 && s <= 1.001) {
        return full;
    }
    int nw = MAX(1, (int)(w * s + 0.5)), nh = MAX(1, (int)(h * s + 0.5));
    GdkPixbuf *sm = gdk_pixbuf_scale_simple(full, nw, nh, GDK_INTERP_BILINEAR);
    g_object_unref(full);
    if (!sm && err && !*err)
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "scaling HEIF failed");
    return sm;
}
