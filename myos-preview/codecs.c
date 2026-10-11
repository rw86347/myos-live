/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * codecs.c — the formats GTK's image loaders don't cover the way Preview
 * needs them:
 *
 *   TIFF        every page of a multi-page TIFF (GTK reads the first), and
 *               writing with a choice of compression (libtiff)
 *   JPEG 2000   .jp2 and raw .j2k codestreams, both ways (openjpeg)
 *   OpenEXR     high dynamic range: read through a simple tone map, written
 *               as linear half floats (OpenEXR's C interface)
 *   Photoshop   the merged picture every .psd carries for programs that
 *               don't understand layers (read only, as on the Mac)
 *   HEIC        read and write (libheif; shared heif-pixbuf.c with My Files)
 *   TGA         writing (GTK reads it but can't write it)
 *
 * Everything comes in and goes out as 8-bit RGBA GdkPixbufs.
 */
#include <OpenEXR/ImfCRgbaFile.h>
#include <libheif/heif.h>
#include <math.h>
#include <openjpeg.h>
#include <stdio.h>
#include <string.h>
#include <tiffio.h>

#include "../myos-heic/heif-pixbuf.h"
#include "preview.h"

#define ERR(err, ...) g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, __VA_ARGS__)

static GdkPixbuf *rgba_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
        return NULL;
    return gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
}

/* The byte at (x, y) of a pixbuf with or without alpha, as RGBA. */
static void px_get(GdkPixbuf *p, int x, int y, guchar out[4])
{
    int n = gdk_pixbuf_get_n_channels(p);
    const guchar *s = gdk_pixbuf_read_pixels(p) + y * gdk_pixbuf_get_rowstride(p) + x * n;
    out[0] = s[0];
    out[1] = s[1];
    out[2] = s[2];
    out[3] = n == 4 ? s[3] : 255;
}

/* --- TIFF --------------------------------------------------------------------------- */

static void tiff_quiet(const char *m, const char *f, va_list ap)
{
    (void)m, (void)f, (void)ap;
}

GPtrArray *codec_read_tiff(const char *path, double *dpi, GError **err)
{
    TIFFSetWarningHandler(tiff_quiet);
    TIFF *t = TIFFOpen(path, "r");
    if (!t) {
        ERR(err, "not a TIFF file");
        return NULL;
    }
    GPtrArray *pages = g_ptr_array_new_with_free_func(g_object_unref);
    *dpi = 72;
    do {
        uint32_t w = 0, h = 0;
        TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
        GdkPixbuf *p = rgba_new(w, h);
        if (!p)
            continue;
        /* libtiff turns every kind of TIFF (palette, CMYK, 16-bit, YCbCr,
           JPEG-compressed...) into 8-bit ABGR words for us. */
        uint32_t *raster = g_try_malloc((size_t)w * h * 4);
        if (raster && TIFFReadRGBAImageOriented(t, w, h, raster, ORIENTATION_TOPLEFT, 0)) {
            guchar *d = gdk_pixbuf_get_pixels(p);
            int rs = gdk_pixbuf_get_rowstride(p);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = raster[y * w + x];
                    guchar *o = d + y * rs + x * 4;
                    o[0] = TIFFGetR(v);
                    o[1] = TIFFGetG(v);
                    o[2] = TIFFGetB(v);
                    o[3] = TIFFGetA(v);
                }
            g_ptr_array_add(pages, p);
        } else {
            g_object_unref(p);
        }
        g_free(raster);
        if (pages->len == 1) {
            float xres = 0;
            uint16_t unit = RESUNIT_INCH;
            TIFFGetField(t, TIFFTAG_RESOLUTIONUNIT, &unit);
            if (TIFFGetField(t, TIFFTAG_XRESOLUTION, &xres) && xres > 1)
                *dpi = unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres;
        }
    } while (TIFFReadDirectory(t));
    TIFFClose(t);
    if (!pages->len) {
        g_ptr_array_unref(pages);
        ERR(err, "the TIFF file has no picture that can be read");
        return NULL;
    }
    return pages;
}

gboolean codec_write_tiff(GPtrArray *pixbufs, const char *path, int compression, double dpi,
                          gboolean alpha, GError **err)
{
    static const uint16_t comp[] = {COMPRESSION_NONE, COMPRESSION_LZW, COMPRESSION_PACKBITS,
                                    COMPRESSION_ADOBE_DEFLATE};
    TIFF *t = TIFFOpen(path, "w");
    if (!t) {
        ERR(err, "can't write %s", path);
        return FALSE;
    }
    gboolean ok = TRUE;
    for (guint i = 0; i < pixbufs->len && ok; i++) {
        GdkPixbuf *p = g_ptr_array_index(pixbufs, i);
        int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
        int spp = alpha && gdk_pixbuf_get_has_alpha(p) ? 4 : 3;
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, spp);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(t, TIFFTAG_COMPRESSION, comp[CLAMP(compression, 0, 3)]);
        TIFFSetField(t, TIFFTAG_XRESOLUTION, (float)dpi);
        TIFFSetField(t, TIFFTAG_YRESOLUTION, (float)dpi);
        TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
        if (spp == 4) {
            uint16_t extra = EXTRASAMPLE_UNASSALPHA;
            TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
        }
        if (pixbufs->len > 1) {
            TIFFSetField(t, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
            TIFFSetField(t, TIFFTAG_PAGENUMBER, (uint16_t)i, (uint16_t)pixbufs->len);
        }
        guchar *row = g_malloc((size_t)w * spp);
        for (int y = 0; y < h && ok; y++) {
            for (int x = 0; x < w; x++) {
                guchar c[4];
                px_get(p, x, y, c);
                memcpy(row + x * spp, c, spp);
            }
            ok = TIFFWriteScanline(t, row, y, 0) >= 0;
        }
        g_free(row);
        ok = ok && TIFFWriteDirectory(t);
    }
    TIFFClose(t);
    if (!ok)
        ERR(err, "writing the TIFF file failed");
    return ok;
}

/* --- JPEG 2000 ---------------------------------------------------------------------- */

static void opj_quiet(const char *msg, void *data)
{
    (void)msg, (void)data;
}

GdkPixbuf *codec_read_jp2(const char *path, GError **err)
{
    /* A .jp2 file starts with the JP2 signature box; a raw codestream (.j2k,
       .j2c) with the SOC and SIZ markers. */
    guchar magic[12] = {0};
    FILE *f = fopen(path, "rb");
    if (!f || fread(magic, 1, sizeof magic, f) < 4) {
        if (f)
            fclose(f);
        ERR(err, "can't read the file");
        return NULL;
    }
    fclose(f);
    OPJ_CODEC_FORMAT fmt = memcmp(magic, "\xff\x4f\xff\x51", 4) == 0 ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;
    opj_stream_t *s = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    opj_codec_t *c = opj_create_decompress(fmt);
    opj_set_warning_handler(c, opj_quiet, NULL);
    opj_set_error_handler(c, opj_quiet, NULL);
    opj_dparameters_t par;
    opj_set_default_decoder_parameters(&par);
    opj_image_t *im = NULL;
    GdkPixbuf *p = NULL;
    if (!s || !opj_setup_decoder(c, &par) || !opj_read_header(s, c, &im) || !opj_decode(c, s, im) ||
        !opj_end_decompress(c, s)) {
        ERR(err, "the JPEG 2000 picture is damaged or uses something openjpeg doesn't support");
        goto out;
    }
    int nc = im->numcomps;
    int w = im->comps[0].w, h = im->comps[0].h;
    if (nc < 1 || !(p = rgba_new(w, h))) {
        ERR(err, "the JPEG 2000 picture has no pixels");
        goto out;
    }
    guchar *d = gdk_pixbuf_get_pixels(p);
    int rs = gdk_pixbuf_get_rowstride(p);
    gboolean ycc = im->color_space == OPJ_CLRSPC_SYCC && nc >= 3;
    gboolean gray = nc < 3;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double v[4] = {0, 0, 0, 255};
            for (int i = 0; i < nc && i < 4; i++) {
                opj_image_comp_t *cp = &im->comps[i];
                /* Colour planes may be smaller (4:2:0): take their pixel. */
                int cx = MIN(x / (int)MAX(cp->dx / im->comps[0].dx, 1u), (int)cp->w - 1);
                int cy = MIN(y / (int)MAX(cp->dy / im->comps[0].dy, 1u), (int)cp->h - 1);
                double val = cp->data[cy * cp->w + cx];
                if (cp->sgnd)
                    val += 1 << (cp->prec - 1);
                v[i] = val * 255.0 / ((1 << cp->prec) - 1);
            }
            guchar *o = d + y * rs + x * 4;
            if (gray) {
                o[0] = o[1] = o[2] = CLAMP(v[0], 0, 255);
                o[3] = nc == 2 ? CLAMP(v[1], 0, 255) : 255;
                continue;
            }
            if (ycc) {
                double Y = v[0], cb = v[1] - 128, cr = v[2] - 128;
                v[0] = Y + 1.402 * cr;
                v[1] = Y - 0.344136 * cb - 0.714136 * cr;
                v[2] = Y + 1.772 * cb;
            }
            for (int i = 0; i < 3; i++)
                o[i] = CLAMP(v[i], 0, 255);
            o[3] = nc >= 4 ? CLAMP(v[3], 0, 255) : 255;
        }
out:
    if (im)
        opj_image_destroy(im);
    opj_destroy_codec(c);
    if (s)
        opj_stream_destroy(s);
    return p;
}

gboolean codec_write_jp2(GdkPixbuf *pix, const char *path, double quality, GError **err)
{
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
    int nc = gdk_pixbuf_get_has_alpha(pix) ? 4 : 3;
    opj_image_cmptparm_t cp[4];
    memset(cp, 0, sizeof cp);
    for (int i = 0; i < nc; i++) {
        cp[i].dx = cp[i].dy = 1;
        cp[i].w = w;
        cp[i].h = h;
        cp[i].prec = 8;
        cp[i].sgnd = 0;
    }
    opj_image_t *im = opj_image_create(nc, cp, OPJ_CLRSPC_SRGB);
    if (!im) {
        ERR(err, "out of memory");
        return FALSE;
    }
    im->x0 = im->y0 = 0;
    im->x1 = w;
    im->y1 = h;
    if (nc == 4)
        im->comps[3].alpha = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guchar c[4];
            px_get(pix, x, y, c);
            for (int i = 0; i < nc; i++)
                im->comps[i].data[y * w + x] = c[i];
        }
    opj_cparameters_t par;
    opj_set_default_encoder_parameters(&par);
    par.tcp_numlayers = 1;
    par.cp_disto_alloc = 1;
    if (quality >= 0.995) {
        par.tcp_rates[0] = 0;           /* lossless (the reversible 5/3 wavelet) */
    } else {
        par.irreversible = 1;           /* lossy 9/7; 1:1 at best, 1:60 at worst */
        par.tcp_rates[0] = 1 + (1 - quality) * 59;
    }
    par.tcp_mct = 1;                    /* the colour transform, as JPEG does */
    gboolean raw = g_str_has_suffix(path, ".j2k") || g_str_has_suffix(path, ".j2c");
    opj_codec_t *c = opj_create_compress(raw ? OPJ_CODEC_J2K : OPJ_CODEC_JP2);
    opj_set_warning_handler(c, opj_quiet, NULL);
    opj_set_error_handler(c, opj_quiet, NULL);
    opj_stream_t *s = NULL;
    gboolean ok = opj_setup_encoder(c, &par, im) &&
                  (s = opj_stream_create_default_file_stream(path, OPJ_FALSE)) &&
                  opj_start_compress(c, im, s) && opj_encode(c, s) && opj_end_compress(c, s);
    if (s)
        opj_stream_destroy(s);
    opj_destroy_codec(c);
    opj_image_destroy(im);
    if (!ok)
        ERR(err, "writing the JPEG 2000 file failed");
    return ok;
}

/* --- OpenEXR -------------------------------------------------------------------------- */

/* Linear light -> the screen's sRGB curve, and back. */
static double to_srgb(double v)
{
    v = CLAMP(v, 0, 1);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

static double from_srgb(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

GdkPixbuf *codec_read_exr(const char *path, GError **err)
{
    ImfInputFile *f = ImfOpenInputFile(path);
    if (!f) {
        ERR(err, "%s", ImfErrorMessage());
        return NULL;
    }
    int x0, y0, x1, y1;
    ImfHeaderDataWindow(ImfInputHeader(f), &x0, &y0, &x1, &y1);
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    GdkPixbuf *p = rgba_new(w, h);
    ImfRgba *px = p ? g_try_malloc((size_t)w * h * sizeof *px) : NULL;
    if (!px) {
        if (p)
            g_object_unref(p);
        ImfCloseInputFile(f);
        ERR(err, "the picture is too big");
        return NULL;
    }
    /* The frame buffer is addressed with the data window's coordinates. */
    ImfInputSetFrameBuffer(f, px - x0 - (ptrdiff_t)y0 * w, 1, w);
    if (!ImfInputReadPixels(f, y0, y1)) {
        g_free(px);
        g_object_unref(p);
        ERR(err, "%s", ImfErrorMessage());
        ImfCloseInputFile(f);
        return NULL;
    }
    ImfCloseInputFile(f);
    /* A simple tone map, as Preview shows an EXR on a normal screen:
       exposure so that bright highlights (the 99th percentile of the
       brightest channel, roughly) land at white, then the sRGB curve. */
    double peak = 1;
    for (int i = 0; i < w * h; i += 7) {
        double m = fmax(ImfHalfToFloat(px[i].r), fmax(ImfHalfToFloat(px[i].g), ImfHalfToFloat(px[i].b)));
        if (isfinite(m) && m > peak)
            peak = m;
    }
    double gain = peak > 4 ? 4 / peak : 1;      /* only very bright files are pulled down */
    guchar *d = gdk_pixbuf_get_pixels(p);
    int rs = gdk_pixbuf_get_rowstride(p);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            ImfRgba *s = &px[y * w + x];
            guchar *o = d + y * rs + x * 4;
            double c[3] = {ImfHalfToFloat(s->r), ImfHalfToFloat(s->g), ImfHalfToFloat(s->b)};
            for (int i = 0; i < 3; i++) {
                double v = isfinite(c[i]) ? c[i] * gain : 0;
                v = v / (1 + v * (gain < 1 ? 0.25 : 0));       /* a soft shoulder */
                o[i] = (guchar)(to_srgb(v) * 255 + 0.5);
            }
            double a = ImfHalfToFloat(s->a);
            o[3] = (guchar)(CLAMP(isfinite(a) ? a : 1, 0, 1) * 255 + 0.5);
        }
    g_free(px);
    return p;
}

gboolean codec_write_exr(GdkPixbuf *pix, const char *path, GError **err)
{
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
    ImfRgba *px = g_try_malloc((size_t)w * h * sizeof *px);
    if (!px) {
        ERR(err, "out of memory");
        return FALSE;
    }
    double lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = from_srgb(i / 255.0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guchar c[4];
            px_get(pix, x, y, c);
            ImfRgba *o = &px[y * w + x];
            ImfFloatToHalf(lut[c[0]], &o->r);
            ImfFloatToHalf(lut[c[1]], &o->g);
            ImfFloatToHalf(lut[c[2]], &o->b);
            ImfFloatToHalf(c[3] / 255.0, &o->a);
        }
    ImfHeader *hd = ImfNewHeader();
    ImfHeaderSetDataWindow(hd, 0, 0, w - 1, h - 1);
    ImfHeaderSetDisplayWindow(hd, 0, 0, w - 1, h - 1);
    ImfHeaderSetCompression(hd, IMF_ZIP_COMPRESSION);
    ImfOutputFile *o = ImfOpenOutputFile(path, hd, IMF_WRITE_RGBA);
    gboolean ok = o && ImfOutputSetFrameBuffer(o, px, 1, w) && ImfOutputWritePixels(o, h);
    if (!ok)
        ERR(err, "%s", ImfErrorMessage());
    if (o && !ImfCloseOutputFile(o) && ok) {
        ERR(err, "%s", ImfErrorMessage());
        ok = FALSE;
    }
    ImfDeleteHeader(hd);
    g_free(px);
    return ok;
}

/* --- Photoshop -------------------------------------------------------------------------- */

/* A .psd file: a 26-byte header, three sections we skip (colour table,
   image resources, layers), then the merged image: every channel's plane in
   turn, raw or PackBits-compressed row by row. */
static guint32 be32(const guchar *p)
{
    return (guint32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static guint16 be16(const guchar *p)
{
    return p[0] << 8 | p[1];
}

GdkPixbuf *codec_read_psd(const char *path, GError **err)
{
    gchar *data;
    gsize len;
    if (!g_file_get_contents(path, &data, &len, err))
        return NULL;
    const guchar *b = (const guchar *)data, *end = b + len;
    GdkPixbuf *p = NULL;
    guchar *planes = NULL;
    if (len < 26 || memcmp(b, "8BPS", 4) != 0 || be16(b + 4) != 1) {
        ERR(err, "not a Photoshop file (or a large-document .psb, which isn't supported)");
        goto out;
    }
    int nch = be16(b + 12), h = be32(b + 14), w = be32(b + 18), depth = be16(b + 22),
        mode = be16(b + 24);
    if (depth != 8 && depth != 16) {
        ERR(err, "Photoshop files of %d bits per channel aren't supported", depth);
        goto out;
    }
    if (mode != 1 && mode != 2 && mode != 3 && mode != 4) {
        ERR(err, "this Photoshop colour mode isn't supported");
        goto out;
    }
    const guchar *q = b + 26;
    const guchar *palette = NULL;
    for (int sec = 0; sec < 3; sec++) {         /* colour data, resources, layers */
        if (q + 4 > end)
            goto damaged;
        guint32 n = be32(q);
        if (sec == 0 && mode == 2 && n >= 768)
            palette = q + 4;
        q += 4;
        if (n > (guint32)(end - q))
            goto damaged;
        q += n;
    }
    if (q + 2 > end || !(p = rgba_new(w, h)))
        goto damaged;
    int comp = be16(q);
    q += 2;
    int bpc = depth / 8;
    int use = MIN(nch, mode == 4 ? 5 : mode == 3 ? 4 : 2);  /* colour channels + alpha */
    planes = g_malloc0((size_t)w * h * use);
    if (comp == 0) {
        for (int c = 0; c < use; c++)
            for (int i = 0; i < w * h; i++) {
                const guchar *s = q + ((size_t)c * w * h + i) * bpc;
                if (s >= end)
                    goto damaged;
                planes[(size_t)c * w * h + i] = s[0];   /* 16-bit: the high byte */
            }
    } else if (comp == 1) {
        const guchar *counts = q;
        const guchar *r = q + (size_t)nch * h * 2;
        for (int c = 0; c < nch; c++)
            for (int y = 0; y < h; y++) {
                if (counts + 2 > end)
                    goto damaged;
                int n = be16(counts);
                counts += 2;
                const guchar *rend = r + n;
                if (rend > end)
                    goto damaged;
                guchar *o = c < use ? planes + (size_t)c * w * h + (size_t)y * w : NULL;
                int x = 0;
                while (r < rend && x < w * bpc) {           /* PackBits */
                    int k = (signed char)*r++;
                    if (k >= 0) {
                        for (int i = 0; i <= k && r < rend; i++, x++, r++)
                            if (o && x % bpc == 0)
                                o[x / bpc] = *r;
                    } else if (k != -128 && r < rend) {
                        for (int i = 0; i <= -k; i++, x++)
                            if (o && x % bpc == 0)
                                o[x / bpc] = *r;
                        r++;
                    }
                }
                r = rend;
            }
    } else {
        ERR(err, "this Photoshop compression (ZIP) isn't supported");
        goto out;
    }
    guchar *d = gdk_pixbuf_get_pixels(p);
    int rs = gdk_pixbuf_get_rowstride(p);
    size_t pl = (size_t)w * h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            guchar *o = d + y * rs + x * 4;
            switch (mode) {
            case 1:                             /* greyscale */
                o[0] = o[1] = o[2] = planes[i];
                o[3] = use > 1 ? planes[pl + i] : 255;
                break;
            case 2:                             /* indexed */
                o[0] = palette ? palette[planes[i]] : planes[i];
                o[1] = palette ? palette[256 + planes[i]] : planes[i];
                o[2] = palette ? palette[512 + planes[i]] : planes[i];
                o[3] = 255;
                break;
            case 3:                             /* RGB */
                o[0] = planes[i];
                o[1] = planes[pl + i];
                o[2] = planes[2 * pl + i];
                o[3] = use > 3 ? planes[3 * pl + i] : 255;
                break;
            case 4: {                           /* CMYK, stored as 255 - ink */
                int kk = planes[3 * pl + i];
                o[0] = planes[i] * kk / 255;
                o[1] = planes[pl + i] * kk / 255;
                o[2] = planes[2 * pl + i] * kk / 255;
                o[3] = use > 4 ? planes[4 * pl + i] : 255;
                break;
            }
            }
        }
    goto out;
damaged:
    ERR(err, "the Photoshop file is damaged");
    if (p)
        g_object_unref(p);
    p = NULL;
out:
    g_free(planes);
    g_free(data);
    return p;
}

/* --- HEIC ------------------------------------------------------------------------------ */

GdkPixbuf *codec_read_heic(const char *path, GError **err)
{
    return myos_heif_pixbuf_from_file(path, err);
}

gboolean codec_write_heic(GdkPixbuf *pix, const char *path, double quality, gboolean alpha,
                          GError **err)
{
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
    gboolean a = alpha && gdk_pixbuf_get_has_alpha(pix);
    struct heif_context *ctx = heif_context_alloc();
    struct heif_encoder *enc = NULL;
    struct heif_image *img = NULL;
    struct heif_error e = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &enc);
    if (e.code)
        goto fail;
    heif_encoder_set_lossy_quality(enc, (int)(quality * 100 + 0.5));
    e = heif_image_create(w, h, heif_colorspace_RGB,
                          a ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, &img);
    if (e.code)
        goto fail;
    e = heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
    if (e.code)
        goto fail;
    int stride;
    uint8_t *d = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    int n = a ? 4 : 3;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guchar c[4];
            px_get(pix, x, y, c);
            memcpy(d + y * stride + x * n, c, n);
        }
    e = heif_context_encode_image(ctx, img, enc, NULL, NULL);
    if (e.code)
        goto fail;
    e = heif_context_write_to_file(ctx, path);
    if (e.code)
        goto fail;
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return TRUE;
fail:
    ERR(err, "HEIC: %s", e.message ? e.message : "encoding failed");
    if (img)
        heif_image_release(img);
    if (enc)
        heif_encoder_release(enc);
    heif_context_free(ctx);
    return FALSE;
}

/* --- TGA (writing) --------------------------------------------------------------------- */

gboolean codec_write_tga(GdkPixbuf *pix, const char *path, GError **err)
{
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ERR(err, "can't write %s", path);
        return FALSE;
    }
    /* Uncompressed true colour, 32 bits, rows from the top (descriptor bit 5). */
    guchar hd[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, w & 255, w >> 8, h & 255, h >> 8, 32, 0x28};
    fwrite(hd, 1, sizeof hd, f);
    guchar *row = g_malloc((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            guchar c[4];
            px_get(pix, x, y, c);
            row[x * 4] = c[2];
            row[x * 4 + 1] = c[1];
            row[x * 4 + 2] = c[0];
            row[x * 4 + 3] = c[3];
        }
        fwrite(row, 1, (size_t)w * 4, f);
    }
    g_free(row);
    if (fclose(f) != 0) {
        ERR(err, "writing %s failed", path);
        return FALSE;
    }
    return TRUE;
}

/* --- GIF (write): GTK reads GIFs but has no writer ------------------------------------- */

/* A GIF has at most 256 colours, so a picture first gets a palette: "median
   cut" — put every colour used (to 5 bits a channel) in one box, keep
   cutting the box with the widest spread in half at its median, and give
   each final box the average of its colours. */
struct box { int lo, hi; };             /* a range of `cols` */
static guint32 *cols;                   /* used 15-bit colours */
static guint32 *hist;                   /* pixels per 15-bit colour */
static int cut_axis;

static int chan(guint32 c, int a) { return (c >> (10 - 5 * a)) & 31; }

static int by_axis(const void *a, const void *b)
{
    return chan(*(const guint32 *)a, cut_axis) - chan(*(const guint32 *)b, cut_axis);
}

static int spread(const struct box *b, int *axis)
{
    int best = -1;
    for (int a = 0; a < 3; a++) {
        int lo = 31, hi = 0;
        for (int i = b->lo; i < b->hi; i++) {
            int v = chan(cols[i], a);
            lo = MIN(lo, v), hi = MAX(hi, v);
        }
        if (hi - lo > best)
            best = hi - lo, *axis = a;
    }
    return best;
}

/* Code words of 3..12 bits, packed low bit first into 255-byte blocks. */
struct lzw_out { FILE *f; guchar blk[256]; int n; guint32 acc; int bits; };

static void put_code(struct lzw_out *o, int code, int size)
{
    o->acc |= (guint32)code << o->bits;
    o->bits += size;
    while (o->bits >= 8) {
        o->blk[1 + o->n++] = o->acc & 255;
        o->acc >>= 8, o->bits -= 8;
        if (o->n == 255) {
            o->blk[0] = 255;
            fwrite(o->blk, 1, 256, o->f);
            o->n = 0;
        }
    }
}

gboolean codec_write_gif(GdkPixbuf *pix, const char *path, GError **err)
{
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix);
    if (w > 65535 || h > 65535) {
        ERR(err, "the picture is too big for GIF");
        return FALSE;
    }
    hist = g_new0(guint32, 32768);
    guint16 *key = g_new(guint16, (size_t)w * h);
    gboolean clear = FALSE;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guchar c[4];
            px_get(pix, x, y, c);
            guint16 kk = c[3] < 128 ? 0xffff : (guint16)((c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3);
            if (kk == 0xffff)
                clear = TRUE;
            else
                hist[kk]++;
            key[(size_t)y * w + x] = kk;
        }
    int ncols = 0, maxc = clear ? 255 : 256;
    cols = g_new(guint32, 32768);
    for (int i = 0; i < 32768; i++)
        if (hist[i])
            cols[ncols++] = i;
    struct box boxes[256] = {{0, ncols}};
    int nb = ncols ? 1 : 0;
    while (nb < maxc) {
        int pick = -1, axis = 0, best = 0;
        for (int i = 0; i < nb; i++) {
            int a = 0, s = boxes[i].hi - boxes[i].lo > 1 ? spread(&boxes[i], &a) : 0;
            if (s > best)
                best = s, pick = i, axis = a;
        }
        if (pick < 0)
            break;
        cut_axis = axis;
        struct box *b = &boxes[pick];
        qsort(cols + b->lo, b->hi - b->lo, sizeof *cols, by_axis);
        guint64 total = 0, half = 0;
        for (int i = b->lo; i < b->hi; i++)
            total += hist[cols[i]];
        int mid = b->lo;
        while (mid < b->hi - 1 && (half += hist[cols[mid]]) < total / 2)
            mid++;
        mid = CLAMP(mid + 1, b->lo + 1, b->hi - 1);
        boxes[nb++] = (struct box){mid, b->hi};
        b->hi = mid;
    }
    guchar pal[256][3] = {{0}};
    for (int i = 0; i < nb; i++) {
        guint64 s[3] = {0}, n = 0;
        for (int j = boxes[i].lo; j < boxes[i].hi; j++) {
            guint32 c = cols[j], cnt = hist[c];
            for (int a = 0; a < 3; a++)
                s[a] += (guint64)(chan(c, a) * 255 / 31) * cnt;
            n += cnt;
        }
        for (int a = 0; a < 3; a++)
            pal[i][a] = n ? s[a] / n : 0;
    }
    /* Each 15-bit colour's nearest palette entry, found once. */
    gint16 *near = g_new(gint16, 32768);
    memset(near, 0xff, 32768 * sizeof *near);
    guchar *idx = g_malloc((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        guint16 kk = key[i];
        if (kk == 0xffff) {
            idx[i] = 255;
            continue;
        }
        if (near[kk] < 0) {
            int r = chan(kk, 0) * 255 / 31, g = chan(kk, 1) * 255 / 31, b = chan(kk, 2) * 255 / 31;
            int bi = 0, bd = INT_MAX;
            for (int j = 0; j < nb; j++) {
                int d = (r - pal[j][0]) * (r - pal[j][0]) + (g - pal[j][1]) * (g - pal[j][1]) +
                        (b - pal[j][2]) * (b - pal[j][2]);
                if (d < bd)
                    bd = d, bi = j;
            }
            near[kk] = bi;
        }
        idx[i] = near[kk];
    }
    g_free(near), g_free(key), g_free(cols), g_free(hist);

    FILE *f = fopen(path, "wb");
    if (!f) {
        g_free(idx);
        ERR(err, "can't write %s", path);
        return FALSE;
    }
    guchar hd[13] = {'G', 'I', 'F', '8', '9', 'a', w & 255, w >> 8, h & 255, h >> 8, 0xf7, 0, 0};
    fwrite(hd, 1, 13, f);
    fwrite(pal, 1, sizeof pal, f);
    if (clear) {        /* graphic control extension: index 255 is see-through */
        guchar gce[8] = {0x21, 0xf9, 4, 1, 0, 0, 255, 0};
        fwrite(gce, 1, 8, f);
    }
    guchar desc[11] = {0x2c, 0, 0, 0, 0, w & 255, w >> 8, h & 255, h >> 8, 0, 8};
    fwrite(desc, 1, 11, f);

    /* LZW: strings seen so far get codes; the table is a hash on (prefix,
       next byte), as in Unix compress. Full at 4096 codes: send a clear. */
    enum { HSIZE = 5003 };
    gint32 *hkey = g_new(gint32, HSIZE);
    gint16 *hcode = g_new(gint16, HSIZE);
    memset(hkey, 0xff, HSIZE * sizeof *hkey);
    struct lzw_out o = {.f = f};
    int size = 9, next = 258, prefix = idx[0];
    put_code(&o, 256, size);
    for (size_t i = 1; i < (size_t)w * h; i++) {
        int c = idx[i];
        gint32 kk = prefix << 8 | c;
        int hpos = (int)(((guint32)kk * 2654435761u) % HSIZE);
        while (hkey[hpos] >= 0 && hkey[hpos] != kk)
            hpos = (hpos + 1) % HSIZE;
        if (hkey[hpos] == kk) {
            prefix = hcode[hpos];
            continue;
        }
        put_code(&o, prefix, size);
        if (next < 4096) {
            hkey[hpos] = kk, hcode[hpos] = next++;
            if (next > (1 << size) && size < 12)
                size++;
        } else {
            put_code(&o, 256, size);
            memset(hkey, 0xff, HSIZE * sizeof *hkey);
            next = 258, size = 9;
        }
        prefix = c;
    }
    put_code(&o, prefix, size);
    put_code(&o, 257, size);
    put_code(&o, 0, 7);                 /* flush the last bits */
    if (o.n) {
        o.blk[0] = o.n;
        fwrite(o.blk, 1, o.n + 1, f);
    }
    guchar tail[2] = {0, 0x3b};
    fwrite(tail, 1, 2, f);
    g_free(hkey), g_free(hcode), g_free(idx);
    if (fclose(f) != 0) {
        ERR(err, "writing %s failed", path);
        return FALSE;
    }
    return TRUE;
}
