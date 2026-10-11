/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * doc.c — a document: opening, the edit history, saving and exporting.
 *
 * Opening looks at the file's first bytes, not its name: PDF goes to
 * poppler; TIFF, JPEG 2000, OpenEXR and Photoshop to codecs.c; animated
 * GIFs become one page per frame; HEIC/HEIF/AVIF to libheif (heif-pixbuf.c);
 * everything else to GTK's image loaders (PNG, JPEG, JPEG XL, WebP, BMP, ICO,
 * ICNS, TGA, PNM, XBM,
 * XPM, SVG, camera RAW).
 *
 * Saving a PDF is done in up to three passes, each by the tool that does
 * it without losing anything:
 *   1. pages deleted, moved, inserted or merged from other PDFs: qpdf
 *      copies the pages (links, forms and fonts intact) into a new file;
 *   2. markup: poppler opens that file, adds the annotations, writes it;
 *   3. rotation: qpdf turns the pages (after the markup, so the markup turns
 *      with its page).
 * The result replaces the file in one rename, so a crash never leaves half
 * a PDF.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <cairo-pdf.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../myos-heic/heif-pixbuf.h"
#include "preview.h"

#define ERR(err, ...) g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, __VA_ARGS__)

const char *const export_names[EX_COUNT] = {
    [EX_HEIC] = "HEIC", [EX_JPEG] = "JPEG", [EX_JP2] = "JPEG-2000", [EX_EXR] = "OpenEXR",
    [EX_PDF] = "PDF",   [EX_PNG] = "PNG",   [EX_TIFF] = "TIFF",     [EX_BMP] = "Microsoft BMP",
    [EX_GIF] = "GIF",   [EX_ICO] = "Microsoft Icon", [EX_TGA] = "TGA", [EX_WEBP] = "WebP",
};
const char *const export_exts[EX_COUNT] = {
    [EX_HEIC] = "heic", [EX_JPEG] = "jpg", [EX_JP2] = "jp2", [EX_EXR] = "exr", [EX_PDF] = "pdf",
    [EX_PNG] = "png",   [EX_TIFF] = "tiff", [EX_BMP] = "bmp", [EX_GIF] = "gif", [EX_ICO] = "ico",
    [EX_TGA] = "tga",   [EX_WEBP] = "webp",
};

/* --- temporary files -------------------------------------------------------------- */

static GPtrArray *temps;

static char *temp_path(const char *ext)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "myos-preview", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *tmpl = g_strdup_printf("%s/p-XXXXXX.%s", dir, ext);
    g_free(dir);
    int fd = g_mkstemp_full(tmpl, O_RDWR, 0600);
    if (fd < 0) {
        g_free(tmpl);
        return NULL;
    }
    close(fd);
    if (!temps)
        temps = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(temps, g_strdup(tmpl));
    return tmpl;
}

void doc_cleanup(void)
{
    for (guint i = 0; temps && i < temps->len; i++)
        g_unlink(g_ptr_array_index(temps, i));
}

/* --- pages ------------------------------------------------------------------------ */

unsigned page_new_id(void)
{
    static unsigned next = 1;
    return next++;
}

static struct page *page_new(void)
{
    struct page *p = g_new0(struct page, 1);
    p->refs = 1;
    p->id = page_new_id();
    return p;
}

struct page *page_ref(struct page *p)
{
    p->refs++;
    return p;
}

void page_unref(struct page *p)
{
    if (!p || --p->refs)
        return;
    if (p->pdf)
        g_object_unref(p->pdf);
    if (p->pix)
        g_object_unref(p->pix);
    g_free(p->src);
    g_free(p);
}

struct page *page_copy(const struct page *p)
{
    struct page *c = g_new(struct page, 1);
    *c = *p;
    c->refs = 1;
    c->src = g_strdup(p->src);
    if (c->pdf)
        g_object_ref(c->pdf);
    if (c->pix)
        g_object_ref(c->pix);
    return c;
}

void page_size(const struct page *p, double *w, double *h)
{
    gboolean side = p->rotation == 90 || p->rotation == 270;
    *w = side ? p->h : p->w;
    *h = side ? p->w : p->h;
}

/* Turning clockwise by r about the top left, then moving the page back into
   view: (x, y) -> (h - y, x) for 90, (w - x, h - y) for 180, (y, w - x) for 270. */
void page_transform(cairo_t *cr, const struct page *p)
{
    switch (p->rotation) {
    case 90:
        cairo_translate(cr, p->h, 0);
        break;
    case 180:
        cairo_translate(cr, p->w, p->h);
        break;
    case 270:
        cairo_translate(cr, 0, p->w);
        break;
    }
    cairo_rotate(cr, p->rotation * M_PI / 180);
}

void page_render(cairo_t *cr, const struct page *p, GPtrArray *marks)
{
    cairo_save(cr);
    page_transform(cr, p);
    if (p->pdf) {
        cairo_rectangle(cr, 0, 0, p->w, p->h);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
        if (pp) {
            poppler_page_render(pp, cr);
            g_object_unref(pp);
        }
    } else {
        gdk_cairo_set_source_pixbuf(cr, p->pix, 0, 0);
        cairo_paint(cr);
    }
    for (guint i = 0; marks && i < marks->len; i++) {
        struct mark *m = g_ptr_array_index(marks, i);
        if (m->page == p->id)
            mark_draw(cr, m, p, NULL);
    }
    cairo_restore(cr);
}

GdkPixbuf *page_pixbuf(const struct page *p, GPtrArray *marks, double scale)
{
    if (!p->pdf && scale == 1 && p->rotation == 0)
        return marks ? marks_flatten(marks, p) : g_object_ref(p->pix);
    double w, h;
    page_size(p, &w, &h);
    int pw = MAX(1, (int)ceil(w * scale)), ph = MAX(1, (int)ceil(h * scale));
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, scale, scale);
    page_render(cr, p, marks);
    cairo_destroy(cr);
    GdkPixbuf *out = gdk_pixbuf_get_from_surface(s, 0, 0, pw, ph);
    cairo_surface_destroy(s);
    return out;
}

static PopplerDocument *open_pdf(const char *path, const char *password, GError **err)
{
    char *uri = g_filename_to_uri(path, NULL, err);
    if (!uri)
        return NULL;
    PopplerDocument *d = poppler_document_new_from_file(uri, password, err);
    g_free(uri);
    return d;
}

static struct page *pdf_page(PopplerDocument *pdf, const char *src, int i)
{
    PopplerPage *pp = poppler_document_get_page(pdf, i);
    if (!pp)
        return NULL;
    struct page *p = page_new();
    p->pdf = g_object_ref(pdf);
    p->src = g_strdup(src);
    p->index = i;
    poppler_page_get_size(pp, &p->w, &p->h);
    g_object_unref(pp);
    return p;
}

/* A one-page PDF of its own, drawn by `paint`. */
static struct page *page_from_drawing(double w, double h, void (*paint)(cairo_t *, void *), void *arg)
{
    char *path = temp_path("pdf");
    if (!path)
        return NULL;
    cairo_surface_t *s = cairo_pdf_surface_create(path, w, h);
    cairo_t *cr = cairo_create(s);
    paint(cr, arg);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    PopplerDocument *pdf = open_pdf(path, NULL, NULL);
    struct page *p = pdf ? pdf_page(pdf, path, 0) : NULL;
    if (pdf)
        g_object_unref(pdf);
    g_free(path);
    return p;
}

static void paint_nothing(cairo_t *cr, void *arg)
{
    (void)cr, (void)arg;
}

struct page *page_blank_pdf(double w, double h)
{
    return page_from_drawing(w, h, paint_nothing, NULL);
}

struct pix_paint { GdkPixbuf *pix; double scale; };

static void paint_pix(cairo_t *cr, void *arg)
{
    struct pix_paint *pp = arg;
    cairo_scale(cr, pp->scale, pp->scale);
    gdk_cairo_set_source_pixbuf(cr, pp->pix, 0, 0);
    cairo_paint(cr);
}

struct page *page_pdf_from_pixbuf(GdkPixbuf *pix, double dpi)
{
    struct pix_paint pp = {pix, 72 / (dpi > 0 ? dpi : 72)};
    return page_from_drawing(gdk_pixbuf_get_width(pix) * pp.scale,
                             gdk_pixbuf_get_height(pix) * pp.scale, paint_pix, &pp);
}

/* --- states and the edit history -------------------------------------------------- */

static struct state *state_new(void)
{
    struct state *s = g_new(struct state, 1);
    s->refs = 1;
    s->pages = g_ptr_array_new_with_free_func((GDestroyNotify)page_unref);
    s->marks = g_ptr_array_new_with_free_func((GDestroyNotify)mark_unref);
    return s;
}

struct state *state_copy(const struct state *s)
{
    struct state *c = state_new();
    for (guint i = 0; i < s->pages->len; i++)
        g_ptr_array_add(c->pages, page_ref(g_ptr_array_index(s->pages, i)));
    for (guint i = 0; i < s->marks->len; i++)
        g_ptr_array_add(c->marks, mark_ref(g_ptr_array_index(s->marks, i)));
    return c;
}

void state_unref(struct state *s)
{
    if (!s || --s->refs)
        return;
    g_ptr_array_unref(s->pages);
    g_ptr_array_unref(s->marks);
    g_free(s);
}

/* Undo goes back this many edits; older states are let go. */
#define UNDO_MAX 100

void doc_commit(struct doc *d, struct state *st)
{
    g_ptr_array_add(d->undo, d->st);
    if (d->undo->len > UNDO_MAX)
        g_ptr_array_remove_index(d->undo, 0);
    d->st = st;
    g_ptr_array_set_size(d->redo, 0);
    d->edits++;
}

gboolean doc_undo(struct doc *d)
{
    if (!d->undo->len)
        return FALSE;
    g_ptr_array_add(d->redo, d->st);
    d->st = g_ptr_array_steal_index(d->undo, d->undo->len - 1);
    d->edits--;
    return TRUE;
}

gboolean doc_redo(struct doc *d)
{
    if (!d->redo->len)
        return FALSE;
    g_ptr_array_add(d->undo, d->st);
    d->st = g_ptr_array_steal_index(d->redo, d->redo->len - 1);
    d->edits++;
    return TRUE;
}

static struct doc *doc_alloc(enum doc_kind kind)
{
    struct doc *d = g_new0(struct doc, 1);
    d->kind = kind;
    d->st = state_new();
    d->undo = g_ptr_array_new_with_free_func((GDestroyNotify)state_unref);
    d->redo = g_ptr_array_new_with_free_func((GDestroyNotify)state_unref);
    d->dpi = 72;
    return d;
}

void doc_free(struct doc *d)
{
    if (!d)
        return;
    state_unref(d->st);
    g_ptr_array_unref(d->undo);
    g_ptr_array_unref(d->redo);
    if (d->pdf)
        g_object_unref(d->pdf);
    if (d->backup) {
        g_unlink(d->backup);
        g_free(d->backup);
    }
    g_free(d->path), g_free(d->name), g_free(d->format), g_free(d->password);
    g_free(d);
}

/* --- opening ------------------------------------------------------------------------ */

/* Frames in a GIF: count its image descriptors (0x2C), skipping the colour
   tables and every block of sub-blocks. */
static int gif_frames(const guchar *b, gsize n)
{
    if (n < 13 || memcmp(b, "GIF8", 4))
        return 1;
    gsize i = 13;
    if (b[10] & 0x80)
        i += 3u << ((b[10] & 7) + 1);
    int frames = 0;
    while (i < n) {
        if (b[i] == 0x21) {                 /* extension: label, sub-blocks */
            i += 2;
        } else if (b[i] == 0x2c) {          /* image: descriptor, table, LZW size, sub-blocks */
            if (i + 10 > n)
                break;
            frames++;
            guchar fl = b[i + 9];
            i += 10;
            if (fl & 0x80)
                i += 3u << ((fl & 7) + 1);
            i++;
        } else {
            break;                          /* 0x3B: the end (or junk) */
        }
        while (i < n && b[i])
            i += b[i] + 1u;
        i++;
    }
    return MAX(frames, 1);
}

static double pix_dpi(GdkPixbuf *p)
{
    const char *x = gdk_pixbuf_get_option(p, "x-dpi");
    double v = x ? g_ascii_strtod(x, NULL) : 0;
    return v > 0 ? v : 72;
}

/* Formats Save can write back in place. The rest are opened read-only and
   Save offers Export, as on the Mac. */
static gboolean can_write(const char *fmt)
{
    static const char *const ours[] = {"tiff", "jp2", "exr", "heif", "tga", "png", "jpeg",
                                       "bmp", "ico", "webp", NULL};
    return g_strv_contains(ours, fmt);
}

static void add_pages_from_pixbufs(struct doc *d, GPtrArray *pixbufs)
{
    for (guint i = 0; i < pixbufs->len; i++) {
        struct page *p = page_new();
        p->pix = g_object_ref(g_ptr_array_index(pixbufs, i));
        p->w = gdk_pixbuf_get_width(p->pix);
        p->h = gdk_pixbuf_get_height(p->pix);
        g_ptr_array_add(d->st->pages, p);
    }
}

gboolean doc_needs_password(const char *path)
{
    GError *e = NULL;
    PopplerDocument *pdf = open_pdf(path, NULL, &e);
    if (pdf) {
        g_object_unref(pdf);
        return FALSE;
    }
    gboolean enc = g_error_matches(e, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED);
    g_error_free(e);
    return enc;
}

struct doc *doc_open(const char *path, const char *password, GError **err)
{
    guchar head[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno), "%s", g_strerror(errno));
        return NULL;
    }
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);

    struct doc *d;
    gboolean pdf = n >= 5 && g_strstr_len((const char *)head, n, "%PDF-") != NULL;
    if (pdf) {
        PopplerDocument *pd = open_pdf(path, password, err);
        if (!pd)
            return NULL;
        d = doc_alloc(DOC_PDF);
        d->pdf = pd;
        d->format = g_strdup("pdf");
        d->password = g_strdup(password);
        for (int i = 0; i < poppler_document_get_n_pages(pd); i++) {
            struct page *p = pdf_page(pd, path, i);
            if (p)
                g_ptr_array_add(d->st->pages, p);
        }
    } else {
        GPtrArray *pix = g_ptr_array_new_with_free_func(g_object_unref);
        double dpi = 72;
        const char *fmt = NULL;
        GdkPixbuf *one = NULL;
        if (n >= 4 && (!memcmp(head, "II*\0", 4) || !memcmp(head, "MM\0*", 4))) {
            GPtrArray *t = codec_read_tiff(path, &dpi, err);
            if (t) {
                g_ptr_array_extend_and_steal(pix, t);
                fmt = "tiff";
            }
        } else if (n >= 12 && (!memcmp(head, "\0\0\0\x0cjP  ", 8) || !memcmp(head, "\xff\x4f\xff\x51", 4))) {
            one = codec_read_jp2(path, err), fmt = "jp2";
        } else if (n >= 4 && !memcmp(head, "\x76\x2f\x31\x01", 4)) {
            one = codec_read_exr(path, err), fmt = "exr";
        } else if (n >= 4 && !memcmp(head, "8BPS", 4)) {
            one = codec_read_psd(path, err), fmt = "psd";
        } else if (myos_heif_sniff(head, n) || myos_heif_path(path)) {
            one = codec_read_heic(path, err), fmt = "heif";
        } else if (n >= 4 && !memcmp(head, "GIF8", 4)) {
            gchar *buf;
            gsize len;
            int frames = 1;
            if (g_file_get_contents(path, &buf, &len, NULL)) {
                frames = gif_frames((guchar *)buf, len);
                g_free(buf);
            }
            GdkPixbufAnimation *an = gdk_pixbuf_animation_new_from_file(path, err);
            if (an) {
                /* Step through the frames by each one's own delay. */
                GTimeVal t = {0, 0};
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                GdkPixbufAnimationIter *it = gdk_pixbuf_animation_get_iter(an, &t);
                for (int i = 0; i < frames; i++) {
                    g_ptr_array_add(pix, gdk_pixbuf_copy(gdk_pixbuf_animation_iter_get_pixbuf(it)));
                    int delay = gdk_pixbuf_animation_iter_get_delay_time(it);
                    if (delay < 0)
                        break;
                    g_time_val_add(&t, (glong)delay * 1000);
                    gdk_pixbuf_animation_iter_advance(it, &t);
                }
                G_GNUC_END_IGNORE_DEPRECATIONS
                g_object_unref(it);
                g_object_unref(an);
                fmt = "gif";
            }
        } else {
            one = gdk_pixbuf_new_from_file(path, err);
            if (one) {
                GdkPixbufFormat *pf = gdk_pixbuf_get_file_info(path, NULL, NULL);
                fmt = pf ? gdk_pixbuf_format_get_name(pf) : "png";
                dpi = pix_dpi(one);
                /* Cameras store the picture sideways and say which way is up. */
                GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(one);
                g_object_unref(one);
                one = o;
            }
        }
        if (one)
            g_ptr_array_add(pix, one);
        if (!pix->len) {
            g_ptr_array_unref(pix);
            if (err && !*err)
                ERR(err, "Preview doesn't know this kind of file");
            return NULL;
        }
        d = doc_alloc(DOC_IMAGE);
        d->format = g_strdup(fmt);
        d->dpi = dpi;
        d->frames = fmt && !strcmp(fmt, "gif") ? (int)pix->len : 0;
        add_pages_from_pixbufs(d, pix);
        g_ptr_array_unref(pix);
    }
    d->path = g_strdup(path);
    d->name = g_filename_display_basename(path);
    d->writable = g_access(path, W_OK) == 0 && (d->kind == DOC_PDF || can_write(d->format));
    return d;
}

struct doc *doc_new_image(GdkPixbuf *pix, const char *name)
{
    struct doc *d = doc_alloc(DOC_IMAGE);
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, pix);
    add_pages_from_pixbufs(d, a);
    g_ptr_array_unref(a);
    d->name = g_strdup(name);
    d->format = g_strdup("png");
    return d;
}

struct doc *doc_duplicate(struct doc *s)
{
    struct doc *d = doc_alloc(s->kind);
    state_unref(d->st);
    d->st = state_copy(s->st);
    if (s->pdf)
        d->pdf = g_object_ref(s->pdf);
    d->password = g_strdup(s->password);
    d->format = g_strdup(s->format);
    d->dpi = s->dpi;
    d->frames = s->frames;
    char *stem = g_strdup(s->name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = 0;
    d->name = g_strdup_printf("%s copy", stem);
    g_free(stem);
    d->edits = 1;               /* never saved */
    return d;
}

/* --- writing ------------------------------------------------------------------------ */

/* qpdf exits 0 (fine) or 3 (fine, with warnings about the input). */
static gboolean run_qpdf(GPtrArray *argv, GError **err)
{
    g_ptr_array_add(argv, NULL);
    char *errs = NULL;
    int status = 0;
    gboolean ok = g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &errs, &status, err);
    if (ok && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 3)) {
        g_free(errs);
        return TRUE;
    }
    if (ok)
        ERR(err, "qpdf: %s", errs && *errs ? g_strstrip(errs) : "failed");
    g_free(errs);
    return FALSE;
}

static void argv_add(GPtrArray *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void argv_add(GPtrArray *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_ptr_array_add(a, g_strdup_vprintf(fmt, ap));
    va_end(ap);
}

/* Are the pages still exactly the file's own, in order? */
static gboolean same_pages(const struct doc *d)
{
    if (!d->path || !d->pdf)
        return FALSE;
    if (NPAGES(d) != poppler_document_get_n_pages(d->pdf))
        return FALSE;
    for (int i = 0; i < NPAGES(d); i++) {
        struct page *p = PAGE(d, i);
        if (p->pdf != d->pdf || p->index != i)
            return FALSE;
    }
    return TRUE;
}

/* Write the PDF as it is now — pages, markup, rotation — to `out`. */
static gboolean pdf_write(struct doc *d, const char *out, GError **err)
{
    char *cur = g_strdup(d->path);   /* the file the next pass reads */
    const char *pw = d->password;
    gboolean ok = TRUE;

    /* 1. the pages */
    if (!same_pages(d)) {
        char *t = temp_path("pdf");
        GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
        argv_add(a, "qpdf");
        if (pw)
            argv_add(a, "--password=%s", pw);
        if (d->path)
            argv_add(a, "%s", d->path);
        else
            argv_add(a, "--empty");
        argv_add(a, "--pages");
        /* Runs of consecutive pages of one file become one range. */
        for (int i = 0; i < NPAGES(d);) {
            struct page *p = PAGE(d, i);
            int j = i + 1;
            while (j < NPAGES(d) && PAGE(d, j)->pdf == p->pdf &&
                   PAGE(d, j)->index == p->index + (j - i))
                j++;
            argv_add(a, "%s", p->src);
            if (pw && d->path && !strcmp(p->src, d->path))
                argv_add(a, "--password=%s", pw);
            argv_add(a, j - i > 1 ? "%d-%d" : "%d", p->index + 1, p->index + (j - i));
            i = j;
        }
        argv_add(a, "--");
        argv_add(a, "%s", t);
        ok = run_qpdf(a, err);
        g_ptr_array_unref(a);
        g_free(cur);
        cur = t;
        if (!ok)
            goto out;
    }

    /* 2. the markup */
    if (d->st->marks->len) {
        PopplerDocument *pd = open_pdf(cur, pw, err);
        if (!pd) {
            ok = FALSE;
            goto out;
        }
        for (int i = 0; i < NPAGES(d); i++) {
            PopplerPage *pp = poppler_document_get_page(pd, i);
            if (pp) {
                marks_to_pdf(d->st->marks, PAGE(d, i), pp);
                g_object_unref(pp);
            }
        }
        char *t = temp_path("pdf");
        char *uri = g_filename_to_uri(t, NULL, NULL);
        ok = poppler_document_save(pd, uri, err);
        g_free(uri);
        g_object_unref(pd);
        g_free(cur);
        cur = t;
        if (!ok)
            goto out;
    }

    /* 3. the rotation (qpdf adds it to each page's own /Rotate) */
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    argv_add(a, "qpdf");
    if (pw)
        argv_add(a, "--password=%s", pw);
    argv_add(a, "%s", cur);
    for (int i = 0; i < NPAGES(d); i++)
        if (PAGE(d, i)->rotation)
            argv_add(a, "--rotate=+%d:%d", PAGE(d, i)->rotation, i + 1);
    argv_add(a, "%s", out);
    ok = run_qpdf(a, err);
    g_ptr_array_unref(a);
out:
    g_free(cur);
    return ok;
}

/* Write to a new file next to `path`, then rename it over: either the old
   file or the whole new one is there, never half of one. */
static char *sibling_temp(const char *path)
{
    char *t = g_strdup_printf("%s.preview-XXXXXX", path);
    int fd = g_mkstemp_full(t, O_RDWR, 0644);
    if (fd < 0) {
        g_free(t);
        return NULL;
    }
    close(fd);
    return t;
}

static gboolean replace(const char *tmp, const char *path, GError **err)
{
    struct stat st;
    if (g_stat(path, &st) == 0)
        g_chmod(tmp, st.st_mode & 07777);
    if (g_rename(tmp, path) != 0) {
        ERR(err, "can't replace %s: %s", path, g_strerror(errno));
        g_unlink(tmp);
        return FALSE;
    }
    return TRUE;
}

static gboolean copy_file(const char *from, const char *to, GError **err)
{
    GFile *a = g_file_new_for_path(from), *b = g_file_new_for_path(to);
    gboolean ok = g_file_copy(a, b, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, err);
    g_object_unref(a), g_object_unref(b);
    return ok;
}

static gboolean save_pixbuf(GdkPixbuf *p, const char *fmt, const char *path, double quality, GError **err)
{
    char q[16];
    g_snprintf(q, sizeof q, "%d", (int)CLAMP(quality * 100 + .5, 0, 100));
    if (!strcmp(fmt, "jpeg"))
        return gdk_pixbuf_save(p, path, "jpeg", err, "quality", q, NULL);
    if (!strcmp(fmt, "webp"))
        return gdk_pixbuf_save(p, path, "webp", err, "quality", q, NULL);
    if (!strcmp(fmt, "ico") && (gdk_pixbuf_get_width(p) > 256 || gdk_pixbuf_get_height(p) > 256)) {
        double s = 256.0 / MAX(gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p));
        GdkPixbuf *sm = gdk_pixbuf_scale_simple(p, MAX(1, (int)(gdk_pixbuf_get_width(p) * s)),
                                                MAX(1, (int)(gdk_pixbuf_get_height(p) * s)), GDK_INTERP_HYPER);
        gboolean ok = gdk_pixbuf_save(sm, path, "ico", err, NULL);
        g_object_unref(sm);
        return ok;
    }
    return gdk_pixbuf_save(p, path, fmt, err, NULL);
}

/* Write image pages in the format `fmt` (one of can_write's). */
static gboolean image_write(GPtrArray *pix, const char *fmt, const char *path, double dpi, GError **err)
{
    GdkPixbuf *first = g_ptr_array_index(pix, 0);
    if (!strcmp(fmt, "tiff"))
        return codec_write_tiff(pix, path, 1, dpi, TRUE, err);
    if (!strcmp(fmt, "jp2"))
        return codec_write_jp2(first, path, .9, err);
    if (!strcmp(fmt, "exr"))
        return codec_write_exr(first, path, err);
    if (!strcmp(fmt, "heif"))
        return codec_write_heic(first, path, .9, TRUE, err);
    if (!strcmp(fmt, "tga"))
        return codec_write_tga(first, path, err);
    if (!strcmp(fmt, "gif"))
        return codec_write_gif(first, path, err);
    return save_pixbuf(first, fmt, path, .9, err);
}

/* After saving, the file is what we show: markup is now part of it. */
static void reload_after_save(struct doc *d)
{
    if (d->kind == DOC_PDF) {
        PopplerDocument *pd = open_pdf(d->path, d->password, NULL);
        if (!pd)
            return;
        struct state *st = state_new();
        for (int i = 0; i < poppler_document_get_n_pages(pd); i++) {
            struct page *p = pdf_page(pd, d->path, i);
            if (p)
                g_ptr_array_add(st->pages, p);
        }
        if (d->pdf)
            g_object_unref(d->pdf);
        d->pdf = pd;
        /* The state before the save stays on Undo: it still points at the
           old pages, which still render. */
        g_ptr_array_add(d->undo, d->st);
        d->st = st;
    }
}

gboolean doc_save(struct doc *d, GError **err)
{
    if (!d->path || !d->writable) {
        ERR(err, "this document can't be saved in place");
        return FALSE;
    }
    if (!d->backup) {
        char *b = temp_path("orig");
        if (b && copy_file(d->path, b, NULL))
            d->backup = b;
        else
            g_free(b);
    }
    char *tmp = sibling_temp(d->path);
    if (!tmp) {
        ERR(err, "can't write in the folder of %s", d->path);
        return FALSE;
    }
    gboolean ok;
    if (d->kind == DOC_PDF) {
        ok = pdf_write(d, tmp, err);
    } else {
        GPtrArray *pix = g_ptr_array_new_with_free_func(g_object_unref);
        for (int i = 0; i < NPAGES(d); i++)
            g_ptr_array_add(pix, page_pixbuf(PAGE(d, i), d->st->marks, 1));
        ok = image_write(pix, d->format, tmp, d->dpi, err);
        if (ok && d->st->marks->len) {
            /* The markup is in the pixels now. */
            struct state *st = state_new();
            for (guint i = 0; i < pix->len; i++) {
                struct page *p = page_copy(PAGE(d, i));
                g_object_unref(p->pix);
                p->pix = g_object_ref(g_ptr_array_index(pix, i));
                g_ptr_array_add(st->pages, p);
            }
            g_ptr_array_add(d->undo, d->st);
            d->st = st;
        }
        g_ptr_array_unref(pix);
    }
    if (ok)
        ok = replace(tmp, d->path, err);
    else
        g_unlink(tmp);
    g_free(tmp);
    if (ok) {
        reload_after_save(d);
        d->edits = 0;
    }
    return ok;
}

gboolean doc_revert(struct doc *d, GError **err)
{
    if (!d->path) {
        ERR(err, "this document has never been saved");
        return FALSE;
    }
    if (d->backup && !copy_file(d->backup, d->path, err))
        return FALSE;
    struct doc *n = doc_open(d->path, d->password, err);
    if (!n)
        return FALSE;
    /* Take the fresh document's contents; keep our identity. */
    state_unref(d->st);
    d->st = n->st, n->st = NULL;
    g_ptr_array_set_size(d->undo, 0);
    g_ptr_array_set_size(d->redo, 0);
    if (d->pdf)
        g_object_unref(d->pdf);
    d->pdf = n->pdf, n->pdf = NULL;
    d->dpi = n->dpi, d->frames = n->frames;
    d->edits = 0;
    n->st = state_new();
    doc_free(n);
    return TRUE;
}

/* --- export ------------------------------------------------------------------------ */

static GdkPixbuf *no_alpha(GdkPixbuf *p)
{
    if (!gdk_pixbuf_get_has_alpha(p))
        return g_object_ref(p);
    int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    GdkPixbuf *o = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_composite_color(p, o, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_NEAREST, 255, 0, 0, 1,
                               0xffffff, 0xffffff);
    return o;
}

static gboolean pdf_encrypt(const char *in, const char *out, const char *password, GError **err)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    argv_add(a, "qpdf");
    argv_add(a, "%s", in);
    argv_add(a, "--encrypt");
    argv_add(a, "%s", password);
    argv_add(a, "%s", password);
    argv_add(a, "256");
    argv_add(a, "--");
    argv_add(a, "%s", out);
    gboolean ok = run_qpdf(a, err);
    g_ptr_array_unref(a);
    return ok;
}

/* Pictures (or a picture document) into a PDF: one page each, sized by the
   picture's resolution. */
static gboolean images_to_pdf(struct doc *d, const char *out, GError **err)
{
    double s = 72 / (d->dpi > 0 ? d->dpi : 72);
    cairo_surface_t *surf = cairo_pdf_surface_create(out, 1, 1);
    for (int i = 0; i < NPAGES(d); i++) {
        double w, h;
        page_size(PAGE(d, i), &w, &h);
        cairo_pdf_surface_set_size(surf, w * s, h * s);
        cairo_t *cr = cairo_create(surf);
        cairo_scale(cr, s, s);
        page_render(cr, PAGE(d, i), d->st->marks);
        cairo_show_page(cr);
        cairo_destroy(cr);
    }
    cairo_surface_finish(surf);
    cairo_status_t cs = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (cs != CAIRO_STATUS_SUCCESS) {
        ERR(err, "writing %s failed: %s", out, cairo_status_to_string(cs));
        return FALSE;
    }
    return TRUE;
}

gboolean doc_export(struct doc *d, int page, const char *path, const struct export_opts *o, GError **err)
{
    gboolean ok;
    char *tmp = sibling_temp(path);
    if (!tmp) {
        ERR(err, "can't write in the folder of %s", path);
        return FALSE;
    }
    if (o->fmt == EX_PDF) {
        char *plain = o->password && *o->password ? temp_path("pdf") : g_strdup(tmp);
        ok = d->kind == DOC_PDF ? pdf_write(d, plain, err) : images_to_pdf(d, plain, err);
        if (ok && o->password && *o->password)
            ok = pdf_encrypt(plain, tmp, o->password, err);
        g_free(plain);
    } else {
        struct page *p = PAGE(d, CLAMP(page, 0, NPAGES(d) - 1));
        double scale = p->pdf ? (o->dpi > 0 ? o->dpi : 72) / 72 : 1;
        GdkPixbuf *pix = page_pixbuf(p, d->st->marks, scale);
        gboolean alpha = o->alpha && o->fmt != EX_JPEG && o->fmt != EX_BMP;
        if (!alpha) {
            GdkPixbuf *x = no_alpha(pix);
            g_object_unref(pix);
            pix = x;
        }
        switch (o->fmt) {
        case EX_HEIC:
            ok = codec_write_heic(pix, tmp, o->quality, alpha, err);
            break;
        case EX_JP2:
            ok = codec_write_jp2(pix, tmp, o->quality, err);
            break;
        case EX_EXR:
            ok = codec_write_exr(pix, tmp, err);
            break;
        case EX_TIFF: {
            GPtrArray *one = g_ptr_array_new();
            g_ptr_array_add(one, pix);
            ok = codec_write_tiff(one, tmp, o->tiff_compression, p->pdf ? o->dpi : d->dpi, alpha, err);
            g_ptr_array_unref(one);
            break;
        }
        case EX_TGA:
            ok = codec_write_tga(pix, tmp, err);
            break;
        case EX_GIF:
            ok = codec_write_gif(pix, tmp, err);
            break;
        default: {
            static const char *const fmt[EX_COUNT] = {[EX_JPEG] = "jpeg", [EX_PNG] = "png",
                                                      [EX_BMP] = "bmp", [EX_ICO] = "ico",
                                                      [EX_WEBP] = "webp"};
            ok = save_pixbuf(pix, fmt[o->fmt], tmp, o->quality, err);
            break;
        }
        }
        g_object_unref(pix);
    }
    if (ok)
        ok = replace(tmp, path, err);
    else
        g_unlink(tmp);
    g_free(tmp);
    return ok;
}

/* --- pixel edits ------------------------------------------------------------------- */

GdkPixbuf *doc_page_pixels(struct doc *d, int i)
{
    return marks_flatten(d->st->marks, PAGE(d, i));
}

/* One undoable step: page i gets new pixels. Its marks were painted into
   them (doc_page_pixels), so they go. */
void doc_set_pixels(struct doc *d, int i, GdkPixbuf *pix)
{
    struct state *st = state_copy(d->st);
    struct page *p = page_copy(PAGE(d, i));
    g_object_unref(p->pix);
    p->pix = g_object_ref(pix);
    p->w = gdk_pixbuf_get_width(pix);
    p->h = gdk_pixbuf_get_height(pix);
    g_ptr_array_remove_index(st->pages, i);
    g_ptr_array_insert(st->pages, i, p);
    for (guint j = st->marks->len; j-- > 0;)
        if (((struct mark *)g_ptr_array_index(st->marks, j))->page == p->id)
            g_ptr_array_remove_index(st->marks, j);
    doc_commit(d, st);
}
