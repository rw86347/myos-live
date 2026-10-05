/*
 * view.c — the pages on screen.
 *
 * The view is a GtkLayout (a canvas that scrolls, and that can hold the
 * text editor of a text box). Each page has a rectangle on it, worked out
 * by relayout() for the display mode: Continuous Scroll, Single Page or
 * Two Pages.
 *
 * Rendering a PDF page is slow (a big page can take a good part of a second
 * on old hardware), so drawing never waits for it: draw() shows what's in
 * the cache — a page rendered at another zoom is stretched for now, a page
 * never seen is white — and asks for an idle callback, which renders ONE
 * missing page and comes back for the next. Scrolling stays smooth and the
 * picture sharpens a moment later. The cache keeps the last few renders (by
 * page and zoom) within a memory budget; a page too big to render whole at
 * a high zoom is rendered only where it's visible.
 *
 * Coordinates: a page's own space is unrotated, in points (PDF) or pixels
 * (pictures). page_matrix() maps it onto the canvas: the page's place,
 * the zoom, and its rotation.
 */
#include <math.h>
#include <string.h>

#include "preview.h"

#define MARGIN S(14)
#define GAP S(10)

/* Rendered pages: whole, or a piece (x, y, w, h in canvas pixels of the
   page at that scale). */
struct cached {
    struct page *pg;            /* a reference: the address can't be reused */
    double scale;               /* 0: a picture's own pixels, any zoom */
    int x, y, w, h;
    cairo_surface_t *s;
    guint64 used;
};
#define NCACHE 24
#define CACHE_PIXELS (8 * 1024 * 1024)      /* about 32 MB: the P3 has little RAM */
#define RENDER_EDGE  800                    /* first paint: long edge, then we refine */

struct rect { double x, y, w, h; };

enum drag {
    D_NONE, D_TEXT, D_AREA_NEW, D_AREA_MOVE, D_LASSO, D_MARK_MOVE, D_MARK_SIZE,
    D_SHAPE_NEW, D_SKETCH, D_ALPHA, D_MARKUP,
};

struct view {
    struct win *w;
    GtkWidget *scroll, *layout;
    GtkAdjustment *ha, *va;
    double zoom;
    gboolean fit;               /* stay zoomed to fit as the window changes */
    gboolean zoom_set;          /* the first allocation picks a zoom */
    enum display_mode mode;
    struct rect *rects;
    int nrects;
    double lw, lh;
    struct cached cache[NCACHE];
    guint64 clock;
    guint idle;
    enum tool tool;

    /* text selection, on one page */
    int ts_page;
    PopplerRectangle ts;        /* from (x1, y1) to (x2, y2) */
    PopplerSelectionStyle ts_style;
    gboolean ts_on;

    /* area selection */
    gboolean area_on, inverted;
    int area_page, shape;       /* 0 rectangle, 1 ellipse, 2 lasso */
    double ar[4];
    GArray *lasso;

    /* marks */
    struct mark *sel;           /* selected (in the current state) */
    struct mark *drag_mark;     /* its new version while being dragged */
    int handle;

    enum drag drag;
    int drag_page;
    double px0, py0, lx0, ly0;  /* press: page and canvas coordinates */
    gboolean moved;
    PopplerLinkMapping *press_link;
    GList *links;               /* link mapping of links_page */
    struct page *links_page;

    double mx, my;
    gboolean pointer_in, magnifier;

    GPtrArray *hits;            /* struct hit* */
    int cur_hit;

    GtkWidget *editor;
    struct mark *editing;
};

static double base(struct view *v)
{
    struct doc *d = v->w->doc;
    if (d->kind == DOC_PDF)
        return px_per_pt;
    return prefs.image_pixels_100 ? 1 : px_per_pt * 72 / (d->dpi > 0 ? d->dpi : 72);
}

static double scale_of(struct view *v)
{
    return v->zoom * base(v);
}

static struct page *pg_at(struct view *v, int i)
{
    return i >= 0 && i < NPAGES(v->w->doc) ? PAGE(v->w->doc, i) : NULL;
}

/* --- geometry ----------------------------------------------------------------------- */

static void vp_size(struct view *v, double *w, double *h)
{
    *w = MAX(gtk_widget_get_allocated_width(v->layout), 1);
    *h = MAX(gtk_widget_get_allocated_height(v->layout), 1);
}

static void relayout(struct view *v)
{
    struct doc *d = v->w->doc;
    int n = NPAGES(d);
    double s = scale_of(v), vw, vh;
    vp_size(v, &vw, &vh);
    g_free(v->rects);
    v->rects = g_new0(struct rect, n);
    v->nrects = n;
    double y = MARGIN, maxw = 0;
    int cur = CLAMP(v->w->page, 0, MAX(n - 1, 0));
    for (int i = 0; i < n; i++) {
        double pw, ph;
        page_size(PAGE(d, i), &pw, &ph);
        v->rects[i].w = pw * s, v->rects[i].h = ph * s;
    }
    if (v->mode == DM_TWO) {
        for (int i = 0; i < n; i += 2) {
            double rw = v->rects[i].w + (i + 1 < n ? GAP + v->rects[i + 1].w : 0);
            double rh = MAX(v->rects[i].h, i + 1 < n ? v->rects[i + 1].h : 0);
            maxw = MAX(maxw, rw);
            v->rects[i].y = y;
            if (i + 1 < n)
                v->rects[i + 1].y = y;
            y += rh + GAP;
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (v->mode == DM_SINGLE && i != cur)
                continue;
            maxw = MAX(maxw, v->rects[i].w);
            v->rects[i].y = y;
            y += v->rects[i].h + GAP;
        }
    }
    v->lw = MAX(vw, maxw + 2 * MARGIN);
    v->lh = MAX(vh, y - GAP + MARGIN);
    /* Centre across; a short document is centred down too. */
    double extra = MAX(0, vh - (y - GAP + MARGIN)) / 2;
    for (int i = 0; i < n; i++) {
        if (v->mode == DM_TWO) {
            if (i % 2)
                continue;
            double rw = v->rects[i].w + (i + 1 < n ? GAP + v->rects[i + 1].w : 0);
            v->rects[i].x = (v->lw - rw) / 2;
            if (i + 1 < n)
                v->rects[i + 1].x = v->rects[i].x + v->rects[i].w + GAP, v->rects[i + 1].y += extra;
        } else {
            v->rects[i].x = (v->lw - v->rects[i].w) / 2;
        }
        v->rects[i].y += extra;
        if (v->mode == DM_SINGLE && i != cur)
            v->rects[i].w = 0;          /* not shown */
    }
    gtk_layout_set_size(GTK_LAYOUT(v->layout), (guint)ceil(v->lw), (guint)ceil(v->lh));
}

/* A page's unrotated space onto the canvas. */
static void page_matrix(struct view *v, int i, cairo_matrix_t *m)
{
    struct page *p = PAGE(v->w->doc, i);
    double s = scale_of(v);
    cairo_matrix_init_translate(m, v->rects[i].x, v->rects[i].y);
    cairo_matrix_scale(m, s, s);
    switch (p->rotation) {
    case 90: cairo_matrix_translate(m, p->h, 0); break;
    case 180: cairo_matrix_translate(m, p->w, p->h); break;
    case 270: cairo_matrix_translate(m, 0, p->w); break;
    }
    cairo_matrix_rotate(m, p->rotation * M_PI / 180);
}

static void to_page(struct view *v, int i, double lx, double ly, double *px, double *py)
{
    cairo_matrix_t m;
    page_matrix(v, i, &m);
    cairo_matrix_invert(&m);
    *px = lx, *py = ly;
    cairo_matrix_transform_point(&m, px, py);
}

static void to_canvas(struct view *v, int i, double px, double py, double *lx, double *ly)
{
    cairo_matrix_t m;
    page_matrix(v, i, &m);
    *lx = px, *ly = py;
    cairo_matrix_transform_point(&m, lx, ly);
}

/* A box in page space, as a canvas rectangle (its bounds, if turned). */
static struct rect box_to_canvas(struct view *v, int i, double x, double y, double w, double h)
{
    double xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
    for (int j = 0; j < 4; j++) {
        double cx, cy;
        to_canvas(v, i, xs[j], ys[j], &cx, &cy);
        x1 = MIN(x1, cx), y1 = MIN(y1, cy), x2 = MAX(x2, cx), y2 = MAX(y2, cy);
    }
    return (struct rect){x1, y1, x2 - x1, y2 - y1};
}

static int page_at(struct view *v, double lx, double ly)
{
    for (int i = 0; i < v->nrects; i++) {
        struct rect *r = &v->rects[i];
        if (r->w > 0 && lx >= r->x && lx < r->x + r->w && ly >= r->y && ly < r->y + r->h)
            return i;
    }
    return -1;
}

/* The page nearest to (lx, ly), for drags that leave the page. */
static int page_near(struct view *v, double lx, double ly)
{
    int best = -1;
    double bd = G_MAXDOUBLE;
    for (int i = 0; i < v->nrects; i++) {
        struct rect *r = &v->rects[i];
        if (r->w <= 0)
            continue;
        double dx = MAX(0, MAX(r->x - lx, lx - (r->x + r->w)));
        double dy = MAX(0, MAX(r->y - ly, ly - (r->y + r->h)));
        if (dx * dx + dy * dy < bd)
            bd = dx * dx + dy * dy, best = i;
    }
    return best;
}

static void visible(struct view *v, double *x, double *y, double *w, double *h)
{
    *x = gtk_adjustment_get_value(v->ha);
    *y = gtk_adjustment_get_value(v->va);
    vp_size(v, w, h);
}

/* --- the render cache ---------------------------------------------------------------- */

static void cache_drop(struct cached *c)
{
    if (c->s)
        cairo_surface_destroy(c->s);
    if (c->pg)
        page_unref(c->pg);
    memset(c, 0, sizeof *c);
}

static void cache_clear(struct view *v)
{
    for (int i = 0; i < NCACHE; i++)
        cache_drop(&v->cache[i]);
}

/* The best render of `p` for drawing the canvas piece (x, y, w, h) at scale
   s: an exact one if there is one (*exact = TRUE), else any. */
static struct cached *cache_find(struct view *v, struct page *p, double s, struct rect want, gboolean *exact)
{
    struct cached *any = NULL;
    *exact = FALSE;
    for (int i = 0; i < NCACHE; i++) {
        struct cached *c = &v->cache[i];
        if (c->pg != p)
            continue;
        if (c->scale == 0) {            /* a picture: good at every zoom */
            *exact = TRUE;
            return c;
        }
        if (fabs(c->scale - s) < 1e-6 && c->x <= want.x && c->y <= want.y &&
            c->x + c->w >= want.x + want.w && c->y + c->h >= want.y + want.h) {
            *exact = TRUE;
            return c;
        }
        if (!any || fabs(c->scale - s) < fabs(any->scale - s))
            any = c;
    }
    return any;
}

static struct cached *cache_slot(struct view *v, size_t pixels)
{
    /* Drop the least recently used until there is room. */
    for (;;) {
        size_t total = 0;
        struct cached *lru = NULL, *empty = NULL;
        for (int i = 0; i < NCACHE; i++) {
            struct cached *c = &v->cache[i];
            if (!c->s) {
                empty = empty ? empty : c;
                continue;
            }
            total += (size_t)c->w * c->h;
            if (!lru || c->used < lru->used)
                lru = c;
        }
        if (empty && total + pixels <= CACHE_PIXELS)
            return empty;
        if (!lru)
            return empty;
        cache_drop(lru);
    }
}

/* What part of page i the canvas shows now, plus a screen more around it, at
   scale s, in the page's canvas pixels (from its corner). */
static struct rect want_of(struct view *v, int i)
{
    double x, y, w, h;
    visible(v, &x, &y, &w, &h);
    struct rect *r = &v->rects[i];
    double x1 = MAX(r->x, x - w / 2), y1 = MAX(r->y, y - h), x2 = MIN(r->x + r->w, x + w * 1.5),
           y2 = MIN(r->y + r->h, y + h * 2);
    return (struct rect){floor(x1 - r->x), floor(y1 - r->y), ceil(x2 - x1) + 1, ceil(y2 - y1) + 1};
}

static void render_page(struct view *v, int i)
{
    struct page *p = PAGE(v->w->doc, i);
    struct cached *c;
    if (!p->pdf) {
        c = cache_slot(v, (size_t)p->w * p->h);
        c->s = gdk_cairo_surface_create_from_pixbuf(p->pix, 1, NULL);
        c->scale = 0;
        c->w = p->w, c->h = p->h;
    } else {
        /* A letter page fitted to a 1920 screen is ~1.5 megapixels.
           poppler on a Pentium III (and on TCG) takes seconds for that.
           Render at most RENDER_EDGE on the long side and stretch: the
           page is on screen in a moment, a bit soft. */
        double s = scale_of(v);
        if (s * MAX(p->w, p->h) > RENDER_EDGE)
            s = RENDER_EDGE / MAX(p->w, p->h);
        int f = gtk_widget_get_scale_factor(v->layout);
        struct rect t = {0, 0, ceil(p->w * s), ceil(p->h * s)};
        c = cache_slot(v, (size_t)(t.w * t.h * f * f));
        c->s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, MAX(1, (int)t.w * f), MAX(1, (int)t.h * f));
        cairo_surface_set_device_scale(c->s, f, f);
        cairo_t *cr = cairo_create(c->s);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, s, s);
        page_transform(cr, p);
        PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
        if (pp) {
            poppler_page_render(pp, cr);
            g_object_unref(pp);
        }
        cairo_destroy(cr);
        c->scale = s;
        c->x = 0, c->y = 0, c->w = t.w, c->h = t.h;
    }
    c->pg = page_ref(p);
    c->used = ++v->clock;
}

/* Render the visible pages that have no exact render, one per call. */
static gboolean idle_render(gpointer data)
{
    struct view *v = data;
    double x, y, w, h;
    visible(v, &x, &y, &w, &h);
    double s = scale_of(v);
    for (int i = 0; i < v->nrects; i++) {
        struct rect *r = &v->rects[i];
        if (r->w <= 0 || r->x + r->w < x || r->x > x + w || r->y + r->h < y || r->y > y + h)
            continue;
        gboolean exact;
        /* Any cached paint of this page is shown (stretched if the zoom
           moved). Re-rendering at every zoom is what made Preview crawl. */
        if (cache_find(v, PAGE(v->w->doc, i), s, want_of(v, i), &exact))
            continue;
        render_page(v, i);
        gtk_widget_queue_draw(v->layout);
        return G_SOURCE_CONTINUE;
    }
    v->idle = 0;
    return G_SOURCE_REMOVE;
}

static void want_render(struct view *v)
{
    if (!v->idle)
        v->idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, idle_render, v, NULL);
}

/* --- drawing ------------------------------------------------------------------------ */

static const GdkRGBA sel_blue = {.25, .5, 1, .35};

static void draw_handles(cairo_t *cr, struct view *v, int i, const struct mark *m)
{
    double pts[8][2];
    int n = 0;
    if (m->kind == MK_LINE || m->kind == MK_ARROW) {
        pts[n][0] = m->x + m->ax * m->w, pts[n++][1] = m->y + m->ay * m->h;
        pts[n][0] = m->x + m->bx * m->w, pts[n++][1] = m->y + m->by * m->h;
    } else {
        for (int k = 0; k < 8; k++) {
            static const double fx[8] = {0, .5, 1, 1, 1, .5, 0, 0}, fy[8] = {0, 0, 0, .5, 1, 1, 1, .5};
            pts[n][0] = m->x + fx[k] * m->w, pts[n++][1] = m->y + fy[k] * m->h;
        }
        struct rect b = box_to_canvas(v, i, m->x, m->y, m->w, m->h);
        cairo_rectangle(cr, b.x + .5, b.y + .5, b.w, b.h);
        cairo_set_source_rgba(cr, .3, .55, 1, .9);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    for (int k = 0; k < n; k++) {
        double cx, cy;
        to_canvas(v, i, pts[k][0], pts[k][1], &cx, &cy);
        cairo_arc(cr, cx, cy, S(4.5), 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, .3, .55, 1);
        cairo_set_line_width(cr, S(1.5));
        cairo_stroke(cr);
    }
}

static void draw_area(cairo_t *cr, struct view *v)
{
    if (!v->area_on || v->area_page >= v->nrects)
        return;
    cairo_save(cr);
    cairo_matrix_t m;
    page_matrix(v, v->area_page, &m);
    cairo_transform(cr, &m);
    if (v->shape == 2 && v->lasso && v->lasso->len >= 4) {
        double *p = (double *)v->lasso->data;
        cairo_move_to(cr, p[0], p[1]);
        for (guint j = 2; j < v->lasso->len; j += 2)
            cairo_line_to(cr, p[j], p[j + 1]);
        if (v->drag != D_LASSO)
            cairo_close_path(cr);
    } else if (v->shape == 1) {
        cairo_save(cr);
        cairo_translate(cr, v->ar[0] + v->ar[2] / 2, v->ar[1] + v->ar[3] / 2);
        cairo_scale(cr, MAX(v->ar[2], 1e-3) / 2, MAX(v->ar[3], 1e-3) / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
    } else {
        cairo_rectangle(cr, v->ar[0], v->ar[1], v->ar[2], v->ar[3]);
    }
    cairo_identity_matrix(cr);
    /* "Marching ants": white under black dashes. */
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_stroke_preserve(cr);
    double dash[] = {S(4), S(4)};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_pages(cairo_t *cr, struct view *v, gboolean for_magnifier)
{
    struct doc *d = v->w->doc;
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
    double s = scale_of(v);
    gboolean missing = FALSE;
    for (int i = 0; i < v->nrects; i++) {
        struct rect *r = &v->rects[i];
        if (r->w <= 0 || r->x > cx2 || r->x + r->w < cx1 || r->y > cy2 || r->y + r->h < cy1)
            continue;
        struct page *p = PAGE(d, i);
        if (d->kind == DOC_PDF) {           /* a sheet of paper: a soft shadow */
            for (int k = 3; k >= 1; k--) {
                cairo_rectangle(cr, r->x - k + S(1), r->y - k + S(2), r->w + 2 * k, r->h + 2 * k);
                cairo_set_source_rgba(cr, 0, 0, 0, .06);
                cairo_fill(cr);
            }
        }
        gboolean exact;
        struct cached *c = cache_find(v, p, s, want_of(v, i), &exact);
        cairo_save(cr);
        cairo_rectangle(cr, r->x, r->y, r->w, r->h);
        cairo_clip(cr);
        if (c) {
            c->used = ++v->clock;
            if (c->scale == 0) {
                cairo_matrix_t m;
                page_matrix(v, i, &m);
                cairo_transform(cr, &m);
                cairo_set_source_surface(cr, c->s, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr),
                                         s >= 3 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
            } else {
                double f = s / c->scale;
                if (!exact) {           /* stale: white under it, in case it's a piece */
                    cairo_set_source_rgb(cr, 1, 1, 1);
                    cairo_paint(cr);
                }
                cairo_translate(cr, r->x, r->y);
                cairo_scale(cr, f, f);
                cairo_set_source_surface(cr, c->s, c->x, c->y);
            }
            cairo_paint(cr);
        } else if (p->pdf) {
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
        }
        cairo_restore(cr);
        if (!exact || !c)
            missing = TRUE;

        /* On the page, in its own space: search hits, the text selection, marks. */
        cairo_save(cr);
        cairo_matrix_t m;
        page_matrix(v, i, &m);
        cairo_transform(cr, &m);
        for (guint h = 0; v->hits && h < v->hits->len; h++) {
            struct hit *hit = g_ptr_array_index(v->hits, h);
            if (hit->page != i)
                continue;
            gboolean cur = (int)h == v->cur_hit;
            double pad = cur ? 1.5 : 0;
            cairo_rectangle(cr, hit->r.x1 - pad, hit->r.y1 - pad, hit->r.x2 - hit->r.x1 + 2 * pad,
                            hit->r.y2 - hit->r.y1 + 2 * pad);
            if (cur)
                cairo_set_source_rgba(cr, 1, .85, 0, .6);
            else
                cairo_set_source_rgba(cr, 1, 1, 0, .35);
            cairo_fill(cr);
        }
        if (v->ts_on && v->ts_page == i && p->pdf) {
            PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
            if (pp) {
                cairo_region_t *rg = poppler_page_get_selected_region(pp, 8, v->ts_style, &v->ts);
                cairo_save(cr);
                cairo_scale(cr, 1 / 8.0, 1 / 8.0);
                for (int k = 0; k < cairo_region_num_rectangles(rg); k++) {
                    cairo_rectangle_int_t rr;
                    cairo_region_get_rectangle(rg, k, &rr);
                    cairo_rectangle(cr, rr.x, rr.y, rr.width, rr.height);
                }
                cairo_restore(cr);
                gdk_cairo_set_source_rgba(cr, &sel_blue);
                cairo_fill(cr);
                cairo_region_destroy(rg);
                g_object_unref(pp);
            }
        }
        for (guint k = 0; k < d->st->marks->len; k++) {
            struct mark *mk = MARK(d, k);
            if (mk->page != p->id || (mk == v->editing && mk->kind == MK_TEXT))
                continue;
            mark_draw(cr, mk == v->sel && v->drag_mark ? v->drag_mark : mk, p, NULL);
        }
        if (v->drag == D_SHAPE_NEW || v->drag == D_SKETCH)
            if (v->drag_mark && !v->sel && v->drag_page == i)
                mark_draw(cr, v->drag_mark, p, NULL);
        cairo_restore(cr);

        if (!for_magnifier && v->sel && v->sel->page == p->id && !v->editing)
            draw_handles(cr, v, i, v->drag_mark ? v->drag_mark : v->sel);
    }
    if (!for_magnifier)
        draw_area(cr, v);
    if (missing)
        want_render(v);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    struct view *v = data;
    GdkWindow *bin = gtk_layout_get_bin_window(GTK_LAYOUT(widget));
    if (!gtk_cairo_should_draw_window(cr, bin))
        return FALSE;
    cairo_save(cr);
    gtk_cairo_transform_to_window(cr, widget, bin);
    gdk_cairo_set_source_rgba(cr, &prefs.background);
    cairo_paint(cr);
    draw_pages(cr, v, FALSE);

    if (v->magnifier && v->pointer_in) {
        /* A round lens following the pointer: the canvas again, twice the size. */
        double r = S(90);
        cairo_save(cr);
        cairo_arc(cr, v->mx, v->my, r, 0, 2 * M_PI);
        cairo_clip(cr);
        gdk_cairo_set_source_rgba(cr, &prefs.background);
        cairo_paint(cr);
        cairo_translate(cr, v->mx, v->my);
        cairo_scale(cr, 2, 2);
        cairo_translate(cr, -v->mx, -v->my);
        draw_pages(cr, v, TRUE);
        cairo_restore(cr);
        cairo_arc(cr, v->mx, v->my, r, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, .45);
        cairo_set_line_width(cr, S(3));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    return FALSE;       /* GtkLayout draws its children (the text editor) on top */
}

/* --- zoom and moving around ---------------------------------------------------------- */

static double fit_zoom(struct view *v)
{
    struct doc *d = v->w->doc;
    if (!NPAGES(d))
        return 1;
    double vw, vh, pw, ph;
    vp_size(v, &vw, &vh);
    int i = CLAMP(v->w->page, 0, NPAGES(d) - 1);
    page_size(PAGE(d, i), &pw, &ph);
    if (v->mode == DM_TWO && (i ^ 1) < NPAGES(d)) {
        double qw, qh;
        page_size(PAGE(d, i ^ 1), &qw, &qh);
        pw += qw + GAP / base(v), ph = MAX(ph, qh);
    }
    double b = base(v);
    return MAX(.02, MIN((vw - 2 * MARGIN) / (pw * b), (vh - 2 * MARGIN) / (ph * b)));
}

/* Change the zoom, keeping the canvas point (fx, fy) — in the viewport —
   over the same spot of the same page. */
static void set_zoom(struct view *v, double z, double fx, double fy)
{
    z = CLAMP(z, .02, 64);
    double x, y, w, h;
    visible(v, &x, &y, &w, &h);
    int i = page_near(v, x + fx, y + fy);
    double px = 0, py = 0;
    if (i >= 0)
        to_page(v, i, x + fx, y + fy, &px, &py);
    v->zoom = z;
    relayout(v);
    if (i >= 0) {
        double lx, ly;
        to_canvas(v, i, px, py, &lx, &ly);
        gtk_adjustment_set_upper(v->ha, v->lw);
        gtk_adjustment_set_upper(v->va, v->lh);
        gtk_adjustment_set_value(v->ha, lx - fx);
        gtk_adjustment_set_value(v->va, ly - fy);
    }
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_zoom(struct view *v, double factor)
{
    double w, h;
    vp_size(v, &w, &h);
    v->fit = FALSE;
    set_zoom(v, v->zoom * factor, w / 2, h / 2);
}

void view_zoom_fit(struct view *v)
{
    v->fit = TRUE;
    v->zoom = fit_zoom(v);
    relayout(v);
    view_go_page(v, v->w->page);
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_zoom_actual(struct view *v)
{
    double w, h;
    vp_size(v, &w, &h);
    v->fit = FALSE;
    set_zoom(v, 1, w / 2, h / 2);
}

double view_get_zoom(struct view *v)
{
    return v->zoom;
}

static void scroll_to(struct view *v, double lx, double ly)
{
    gtk_adjustment_set_upper(v->ha, v->lw);
    gtk_adjustment_set_upper(v->va, v->lh);
    gtk_adjustment_set_value(v->ha, lx);
    gtk_adjustment_set_value(v->va, ly);
}

/* Show the canvas rectangle r, centred, unless it's already in view. */
static void reveal(struct view *v, struct rect r)
{
    double x, y, w, h;
    visible(v, &x, &y, &w, &h);
    if (r.x >= x && r.y >= y && r.x + r.w <= x + w && r.y + r.h <= y + h)
        return;
    scroll_to(v, r.x + r.w / 2 - w / 2, r.y + r.h / 2 - h / 2);
}

void view_zoom_selection(struct view *v)
{
    if (!v->area_on)
        return;
    double w, h;
    vp_size(v, &w, &h);
    double pw = v->ar[2] * base(v), ph = v->ar[3] * base(v);
    if (pw < 1 || ph < 1)
        return;
    v->fit = FALSE;
    v->zoom = CLAMP(MIN(w * .9 / pw, h * .9 / ph), .02, 64);
    relayout(v);
    struct rect r = box_to_canvas(v, v->area_page, v->ar[0], v->ar[1], v->ar[2], v->ar[3]);
    scroll_to(v, r.x + r.w / 2 - w / 2, r.y + r.h / 2 - h / 2);
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_go_page(struct view *v, int page)
{
    struct doc *d = v->w->doc;
    if (!NPAGES(d))
        return;
    page = CLAMP(page, 0, NPAGES(d) - 1);
    v->w->page = page;
    if (v->mode == DM_SINGLE)
        relayout(v);
    if (!v->rects || page >= v->nrects)
        return;
    double y = v->rects[page].y - MARGIN;
    scroll_to(v, gtk_adjustment_get_value(v->ha), v->mode == DM_SINGLE ? 0 : y);
    gtk_widget_queue_draw(v->layout);
}

int view_current_page(struct view *v)
{
    return v->w->page;
}

/* Which page is in view: the one under the viewport's upper third. */
static void track_page(struct view *v)
{
    if (v->mode == DM_SINGLE || !v->nrects)
        return;
    double x, y, w, h;
    visible(v, &x, &y, &w, &h);
    double line = y + h / 3;
    int best = v->w->page;
    for (int i = 0; i < v->nrects; i++)
        if (v->rects[i].y <= line && v->rects[i].y + v->rects[i].h + GAP > line) {
            best = i;
            break;
        }
    if (y + h >= v->lh - 1 && v->nrects)    /* scrolled to the end: the last page */
        best = v->mode == DM_TWO ? (v->nrects - 1) & ~1 : v->nrects - 1;
    if (best != v->w->page) {
        v->w->page = best;
        win_update(v->w);
    }
}

static void on_scrolled(GtkAdjustment *a, gpointer data)
{
    (void)a;
    struct view *v = data;
    track_page(v);
    want_render(v);
}

void view_go_dest(struct view *v, PopplerDest *dest)
{
    struct doc *d = v->w->doc;
    PopplerDest *own = NULL;
    if (dest->type == POPPLER_DEST_NAMED) {
        own = poppler_document_find_dest(d->pdf, dest->named_dest);
        if (!own)
            return;
        dest = own;
    }
    /* Pages may have moved: find the one that is that PDF page. */
    for (int i = 0; i < NPAGES(d); i++) {
        struct page *p = PAGE(d, i);
        if (p->pdf == d->pdf && p->index == dest->page_num - 1) {
            view_go_page(v, i);
            if (dest->change_top && v->mode != DM_SINGLE) {
                double lx, ly;
                to_canvas(v, i, dest->left, p->h - dest->top, &lx, &ly);
                scroll_to(v, gtk_adjustment_get_value(v->ha), ly - S(8));
            }
            win_update(v->w);
            break;
        }
    }
    if (own)
        poppler_dest_free(own);
}

void view_set_mode(struct view *v, enum display_mode m)
{
    int page = v->w->page;
    v->mode = m;
    if (v->fit)
        v->zoom = fit_zoom(v);
    relayout(v);
    view_go_page(v, page);
    win_update(v->w);
}

enum display_mode view_mode(struct view *v)
{
    return v->mode;
}

static void on_size(GtkWidget *wd, GdkRectangle *a, gpointer data)
{
    (void)wd, (void)a;
    struct view *v = data;
    if (!v->zoom_set) {
        /* Opening: PDFs as Settings say; pictures at 100% unless that's
           bigger than the window. */
        v->zoom_set = TRUE;
        double f = fit_zoom(v);
        if (v->w->doc->kind == DOC_PDF)
            v->fit = prefs.pdf_fit, v->zoom = prefs.pdf_fit ? f : 1;
        else
            v->fit = f < 1, v->zoom = MIN(f, 1);
    } else if (v->fit) {
        v->zoom = fit_zoom(v);
    }
    relayout(v);
}

void view_reload(struct view *v)
{
    struct doc *d = v->w->doc;
    if (v->sel && !g_ptr_array_find(d->st->marks, v->sel, NULL))
        v->sel = NULL;
    if (v->area_on && v->area_page >= NPAGES(d))
        v->area_on = FALSE;
    if (v->ts_on && v->ts_page >= NPAGES(d))
        v->ts_on = FALSE;
    v->w->page = CLAMP(v->w->page, 0, MAX(NPAGES(d) - 1, 0));
    if (v->fit)
        v->zoom = fit_zoom(v);
    relayout(v);
    gtk_widget_queue_draw(v->layout);
}

void view_redraw(struct view *v)
{
    gtk_widget_queue_draw(v->layout);
}

/* --- selection ------------------------------------------------------------------------ */

void view_set_tool(struct view *v, enum tool t)
{
    v->tool = t;
    static const char *const cursors[] = {
        [T_TEXT] = "text", [T_RECT_SEL] = "crosshair", [T_ELLIPSE_SEL] = "crosshair",
        [T_LASSO_SEL] = "crosshair", [T_ALPHA] = "crosshair", [T_SKETCH] = "crosshair",
        [T_DRAW] = "crosshair", [T_SHAPE] = "crosshair", [T_TEXTBOX] = "text",
        [T_NOTE] = "default", [T_SIGN] = "copy", [T_HIGHLIGHT] = "text", [T_UNDERLINE] = "text",
        [T_STRIKE] = "text",
    };
    GdkWindow *bin = gtk_layout_get_bin_window(GTK_LAYOUT(v->layout));
    if (bin) {
        GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(bin), cursors[t]);
        gdk_window_set_cursor(bin, c);
        if (c)
            g_object_unref(c);
    }
}

enum tool view_tool(struct view *v)
{
    return v->tool;
}

gboolean view_has_selection(struct view *v)
{
    return v->ts_on || v->area_on;
}

gboolean view_has_area(struct view *v)
{
    return v->area_on;
}

gboolean view_has_mark(struct view *v)
{
    return v->sel != NULL;
}

char *view_selected_text(struct view *v)
{
    struct page *p = pg_at(v, v->ts_page);
    if (!v->ts_on || !p || !p->pdf)
        return NULL;
    PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
    char *t = pp ? poppler_page_get_selected_text(pp, v->ts_style, &v->ts) : NULL;
    if (pp)
        g_object_unref(pp);
    return t;
}

GdkPixbuf *view_selected_image(struct view *v)
{
    struct page *p = pg_at(v, v->area_page);
    if (!v->area_on || !p)
        return NULL;
    if (!p->pdf) {
        GdkPixbuf *full = doc_page_pixels(v->w->doc, v->area_page);
        GdkPixbuf *out = img_crop(full, (int)v->ar[0], (int)v->ar[1], (int)ceil(v->ar[2]), (int)ceil(v->ar[3]),
                                  v->shape, v->lasso);
        g_object_unref(full);
        return out;
    }
    /* A PDF area: a picture of it at twice the screen's 100%. */
    double s = 2 * px_per_pt;
    int w = MAX(1, (int)(v->ar[2] * s)), h = MAX(1, (int)(v->ar[3] * s));
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, s, s);
    cairo_translate(cr, -v->ar[0], -v->ar[1]);
    struct page flat = *p;
    flat.rotation = 0;
    page_render(cr, &flat, v->w->doc->st->marks);
    cairo_destroy(cr);
    GdkPixbuf *out = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);
    cairo_surface_destroy(surf);
    return out;
}

gboolean view_area(struct view *v, int *page, double r[4], int *shape, GArray **lasso)
{
    if (!v->area_on)
        return FALSE;
    *page = v->area_page;
    memcpy(r, v->ar, sizeof v->ar);
    *shape = v->shape | (v->inverted ? 4 : 0);
    if (lasso)
        *lasso = v->lasso;
    return TRUE;
}

void view_deselect(struct view *v)
{
    v->ts_on = v->area_on = v->inverted = FALSE;
    v->sel = NULL;
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_select_all(struct view *v)
{
    struct page *p = pg_at(v, v->w->page);
    if (!p)
        return;
    view_deselect(v);
    if (p->pdf) {
        v->ts_on = TRUE;
        v->ts_page = v->w->page;
        v->ts = (PopplerRectangle){0, 0, p->w, p->h};
        v->ts_style = POPPLER_SELECTION_GLYPH;
    } else {
        v->area_on = TRUE;
        v->area_page = v->w->page;
        v->shape = 0;
        v->ar[0] = v->ar[1] = 0, v->ar[2] = p->w, v->ar[3] = p->h;
    }
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_invert_selection(struct view *v)
{
    if (!v->area_on)
        return;
    v->inverted = !v->inverted;
    gtk_widget_queue_draw(v->layout);
}

/* --- editing marks ----------------------------------------------------------------- */

/* One undoable step: `old` becomes `new` (either may be NULL: add, delete). */
static void marks_edit(struct view *v, struct mark *old, struct mark *new)
{
    struct doc *d = v->w->doc;
    struct state *st = state_copy(d->st);
    guint i;
    if (old && g_ptr_array_find(st->marks, old, &i)) {
        g_ptr_array_remove_index(st->marks, i);
        if (new)
            g_ptr_array_insert(st->marks, i, new);
    } else if (new) {
        g_ptr_array_add(st->marks, new);
    }
    doc_commit(d, st);
    v->sel = new;
    win_doc_changed(v->w);
}

void view_delete_selected_mark(struct view *v)
{
    if (v->sel)
        marks_edit(v, v->sel, NULL);
}

static void apply_style(struct view *v, struct mark *m)
{
    struct win *w = v->w;
    if (m->kind == MK_HIGHLIGHT) {
        m->fill = w->highlight;
        return;
    }
    if (m->kind == MK_UNDERLINE || m->kind == MK_STRIKE) {
        m->stroke = w->highlight;
        m->stroke.alpha = 1;
        return;
    }
    m->stroke = w->stroke;
    m->width = w->line_width;
    if (m->kind != MK_LINE && m->kind != MK_ARROW && m->kind != MK_SKETCH && m->kind != MK_SIGNATURE)
        m->fill = w->fill;
    m->text_color = w->text_color;
    g_free(m->font);
    m->font = g_strdup(w->font);
}

void view_style_changed(struct view *v)
{
    if (!v->sel)
        return;
    struct mark *m = mark_copy(v->sel);
    apply_style(v, m);
    marks_edit(v, v->sel, m);
}

static void finish_editing(struct view *v, gboolean keep)
{
    if (!v->editor)
        return;
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(v->editor));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char *text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    struct mark *old = v->editing;
    GtkWidget *ed = v->editor;
    v->editor = NULL;
    v->editing = NULL;
    gtk_widget_destroy(ed);
    if (keep && old && g_ptr_array_find(v->w->doc->st->marks, old, NULL) && g_strcmp0(text, old->text)) {
        if (!*text && old->kind == MK_TEXT) {
            marks_edit(v, old, NULL);
        } else {
            struct mark *m = mark_copy(old);
            g_free(m->text);
            m->text = text, text = NULL;
            marks_edit(v, old, m);
        }
    }
    g_free(text);
    gtk_widget_grab_focus(v->layout);
    gtk_widget_queue_draw(v->layout);
}

static gboolean editor_key(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd;
    if (e->keyval == GDK_KEY_Escape) {
        finish_editing(data, TRUE);
        return TRUE;
    }
    return FALSE;
}

static gboolean editor_unfocus(GtkWidget *wd, GdkEvent *e, gpointer data)
{
    (void)wd, (void)e;
    finish_editing(data, TRUE);
    return FALSE;
}

/* Type into a text box, speech bubble or note: a text view on the canvas. */
static void edit_text(struct view *v, struct mark *m)
{
    finish_editing(v, TRUE);
    int i = -1;
    for (int k = 0; k < NPAGES(v->w->doc); k++)
        if (PAGE(v->w->doc, k)->id == m->page)
            i = k;
    if (i < 0 || i >= v->nrects)
        return;
    double s = scale_of(v);
    struct rect r;
    if (m->kind == MK_NOTE) {
        struct rect icon = box_to_canvas(v, i, m->x, m->y, m->w, m->h);
        r = (struct rect){icon.x + icon.w + S(4), icon.y, S(220), S(140)};
    } else {
        r = box_to_canvas(v, i, m->x, m->y, m->w, m->h);
    }
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), m->text ? m->text : "", -1);
    GtkCssProvider *css = gtk_css_provider_new();
    PangoFontDescription *fd = pango_font_description_from_string(m->font ? m->font : "DejaVu Sans 14");
    double pts = pango_font_description_get_size(fd) / (double)PANGO_SCALE;
    char *rule = g_strdup_printf("textview, textview text { font-family: \"%s\"; font-size: %.1fpx; "
                                 "color: rgba(%d,%d,%d,%.2f); background: %s; }",
                                 pango_font_description_get_family(fd) ? pango_font_description_get_family(fd) : "Sans",
                                 m->kind == MK_NOTE ? S(13) : pts * s, (int)(m->text_color.red * 255),
                                 (int)(m->text_color.green * 255), (int)(m->text_color.blue * 255),
                                 m->text_color.alpha, m->kind == MK_NOTE ? "#fff3a8" : "rgba(255,255,255,0.6)");
    gtk_css_provider_load_from_data(css, rule, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(tv), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(rule);
    g_object_unref(css);
    pango_font_description_free(fd);
    gtk_widget_set_size_request(tv, MAX((int)r.w, S(40)), MAX((int)r.h, S(20)));
    gtk_layout_put(GTK_LAYOUT(v->layout), tv, (int)r.x, (int)r.y);
    g_signal_connect(tv, "key-press-event", G_CALLBACK(editor_key), v);
    g_signal_connect(tv, "focus-out-event", G_CALLBACK(editor_unfocus), v);
    gtk_widget_show(tv);
    gtk_widget_grab_focus(tv);
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter s0, e0;
    gtk_text_buffer_get_bounds(b, &s0, &e0);
    gtk_text_buffer_select_range(b, &s0, &e0);
    v->editor = tv;
    v->editing = m;
    gtk_widget_queue_draw(v->layout);
}

static void center_on_page(struct view *v, int i, struct mark *m)
{
    double x, y, w, h, px, py;
    visible(v, &x, &y, &w, &h);
    struct rect *r = &v->rects[i];
    double cx = CLAMP(x + w / 2, r->x, r->x + r->w), cy = CLAMP(y + h / 2, r->y, r->y + r->h);
    to_page(v, i, cx, cy, &px, &py);
    m->x = px - m->w / 2, m->y = py - m->h / 2;
}

void view_add_mark(struct view *v, struct mark *m)
{
    int i = CLAMP(v->w->page, 0, NPAGES(v->w->doc) - 1);
    m->page = PAGE(v->w->doc, i)->id;
    center_on_page(v, i, m);
    marks_edit(v, NULL, m);
    if (m->kind == MK_TEXT || m->kind == MK_NOTE)
        edit_text(v, m);
}

/* Text markup from the current text selection. */
void view_markup_text(struct view *v, enum mark_kind k)
{
    struct page *p = pg_at(v, v->ts_page);
    if (!v->ts_on || !p || !p->pdf)
        return;
    PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
    if (!pp)
        return;
    cairo_region_t *rg = poppler_page_get_selected_region(pp, 8, v->ts_style, &v->ts);
    int n = cairo_region_num_rectangles(rg);
    if (n) {
        struct mark *m = mark_new(k, p->id);
        m->quads = g_array_new(FALSE, FALSE, sizeof(double));
        double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
        for (int j = 0; j < n; j++) {
            cairo_rectangle_int_t rr;
            cairo_region_get_rectangle(rg, j, &rr);
            double q[4] = {rr.x / 8.0, rr.y / 8.0, rr.width / 8.0, rr.height / 8.0};
            g_array_append_vals(m->quads, q, 4);
            x1 = MIN(x1, q[0]), y1 = MIN(y1, q[1]), x2 = MAX(x2, q[0] + q[2]), y2 = MAX(y2, q[1] + q[3]);
        }
        m->x = x1, m->y = y1, m->w = x2 - x1, m->h = y2 - y1;
        m->text = poppler_page_get_selected_text(pp, v->ts_style, &v->ts);
        apply_style(v, m);
        v->ts_on = FALSE;
        marks_edit(v, NULL, m);
    }
    cairo_region_destroy(rg);
    g_object_unref(pp);
}

/* Which of the selected mark's handles is at (px, py)? -1: none. */
static int handle_at(struct view *v, int i, double lx, double ly)
{
    struct mark *m = v->sel;
    if (!m || m->page != PAGE(v->w->doc, i)->id || m->kind == MK_HIGHLIGHT || m->kind == MK_UNDERLINE ||
        m->kind == MK_STRIKE || m->kind == MK_NOTE)
        return -1;
    double pts[8][2];
    int n = 0;
    if (m->kind == MK_LINE || m->kind == MK_ARROW) {
        pts[n][0] = m->x + m->ax * m->w, pts[n++][1] = m->y + m->ay * m->h;
        pts[n][0] = m->x + m->bx * m->w, pts[n++][1] = m->y + m->by * m->h;
    } else {
        static const double fx[8] = {0, .5, 1, 1, 1, .5, 0, 0}, fy[8] = {0, 0, 0, .5, 1, 1, 1, .5};
        for (int k = 0; k < 8; k++)
            pts[n][0] = m->x + fx[k] * m->w, pts[n++][1] = m->y + fy[k] * m->h;
    }
    for (int k = 0; k < n; k++) {
        double cx, cy;
        to_canvas(v, i, pts[k][0], pts[k][1], &cx, &cy);
        if (hypot(cx - lx, cy - ly) <= S(7))
            return k;
    }
    return -1;
}

static struct mark *mark_at(struct view *v, int i, double px, double py)
{
    struct doc *d = v->w->doc;
    double slop = S(4) / scale_of(v);
    unsigned id = PAGE(d, i)->id;
    for (guint k = d->st->marks->len; k-- > 0;) {
        struct mark *m = MARK(d, k);
        if (m->page == id && mark_hit(m, px, py, slop))
            return m;
    }
    return NULL;
}

/* A line's ends, in page space, back into its box. */
static void line_set(struct mark *m, double x1, double y1, double x2, double y2)
{
    m->x = MIN(x1, x2), m->y = MIN(y1, y2);
    m->w = MAX(fabs(x2 - x1), 1e-3), m->h = MAX(fabs(y2 - y1), 1e-3);
    m->ax = (x1 - m->x) / m->w, m->ay = (y1 - m->y) / m->h;
    m->bx = (x2 - m->x) / m->w, m->by = (y2 - m->y) / m->h;
}

/* --- the mouse ---------------------------------------------------------------------- */

static GList *links_of(struct view *v, struct page *p)
{
    if (v->links_page != p) {
        if (v->links)
            poppler_page_free_link_mapping(v->links);
        if (v->links_page)
            page_unref(v->links_page);
        v->links = NULL;
        v->links_page = page_ref(p);
        PopplerPage *pp = p->pdf ? poppler_document_get_page(p->pdf, p->index) : NULL;
        if (pp) {
            v->links = poppler_page_get_link_mapping(pp);
            g_object_unref(pp);
        }
    }
    return v->links;
}

static PopplerLinkMapping *link_at(struct view *v, int i, double px, double py)
{
    struct page *p = PAGE(v->w->doc, i);
    for (GList *l = links_of(v, p); l; l = l->next) {
        PopplerLinkMapping *lm = l->data;
        /* Link areas count up from the bottom of the page. */
        double y1 = p->h - lm->area.y2, y2 = p->h - lm->area.y1;
        if (px >= lm->area.x1 && px <= lm->area.x2 && py >= y1 && py <= y2)
            return lm;
    }
    return NULL;
}

static void follow(struct view *v, PopplerAction *a)
{
    struct doc *d = v->w->doc;
    switch (a->type) {
    case POPPLER_ACTION_GOTO_DEST:
        view_go_dest(v, a->goto_dest.dest);
        break;
    case POPPLER_ACTION_URI:
        gtk_show_uri_on_window(GTK_WINDOW(v->w->window), a->uri.uri, GDK_CURRENT_TIME, NULL);
        break;
    case POPPLER_ACTION_NAMED: {
        const char *n = a->named.named_dest;
        int p = v->w->page;
        if (!g_strcmp0(n, "NextPage"))
            p++;
        else if (!g_strcmp0(n, "PrevPage"))
            p--;
        else if (!g_strcmp0(n, "FirstPage"))
            p = 0;
        else if (!g_strcmp0(n, "LastPage"))
            p = NPAGES(d) - 1;
        win_set_page(v->w, p);
        break;
    }
    case POPPLER_ACTION_GOTO_REMOTE:
        if (a->goto_remote.file_name && d->path) {
            char *dir = g_path_get_dirname(d->path);
            char *f = g_build_filename(dir, a->goto_remote.file_name, NULL);
            win_open_file(f);
            g_free(f), g_free(dir);
        }
        break;
    default:
        break;
    }
}

static gboolean on_press(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    struct view *v = data;
    struct doc *d = v->w->doc;
    gtk_widget_grab_focus(wd);
    if (e->button != 1)
        return FALSE;
    if (v->editor) {
        finish_editing(v, TRUE);
        return TRUE;
    }
    int i = page_at(v, e->x, e->y);
    v->lx0 = e->x, v->ly0 = e->y;
    v->moved = FALSE;
    v->press_link = NULL;
    if (i < 0) {
        if (e->type == GDK_BUTTON_PRESS)
            view_deselect(v);
        return TRUE;
    }
    double px, py;
    to_page(v, i, e->x, e->y, &px, &py);
    v->px0 = px, v->py0 = py;
    v->drag_page = i;
    struct page *p = PAGE(d, i);

    /* Marks first, whatever the tool: their handles, then the marks. */
    int h = handle_at(v, i, e->x, e->y);
    if (h >= 0) {
        v->drag = D_MARK_SIZE;
        v->handle = h;
        v->drag_mark = mark_copy(v->sel);
        return TRUE;
    }
    struct mark *m = mark_at(v, i, px, py);
    if (m && v->tool != T_HIGHLIGHT && v->tool != T_UNDERLINE && v->tool != T_STRIKE) {
        v->sel = m;
        v->ts_on = v->area_on = FALSE;
        if (e->type == GDK_2BUTTON_PRESS &&
            (m->kind == MK_TEXT || m->kind == MK_BUBBLE || m->kind == MK_NOTE || m->kind == MK_RECT ||
             m->kind == MK_RRECT || m->kind == MK_OVAL)) {
            edit_text(v, m);
            return TRUE;
        }
        if (m->kind == MK_NOTE && e->type == GDK_BUTTON_PRESS) {
            /* A note opens on a click, as on the Mac; dragging moves it. */
        }
        v->drag = D_MARK_MOVE;
        v->drag_mark = mark_copy(m);
        gtk_widget_queue_draw(v->layout);
        win_update(v->w);
        return TRUE;
    }
    v->sel = NULL;

    switch (v->tool) {
    case T_TEXT:
    case T_HIGHLIGHT:
    case T_UNDERLINE:
    case T_STRIKE:
        if (!p->pdf)
            goto area;
        if (v->tool == T_TEXT)
            v->press_link = link_at(v, i, px, py);
        v->ts_page = i;
        v->ts = (PopplerRectangle){px, py, px, py};
        v->ts_style = e->type == GDK_3BUTTON_PRESS   ? POPPLER_SELECTION_LINE
                      : e->type == GDK_2BUTTON_PRESS ? POPPLER_SELECTION_WORD
                                                     : POPPLER_SELECTION_GLYPH;
        v->ts_on = e->type != GDK_BUTTON_PRESS;
        v->area_on = FALSE;
        v->drag = v->tool == T_TEXT ? D_TEXT : D_MARKUP;
        break;
    case T_RECT_SEL:
    case T_ELLIPSE_SEL:
    area:
        v->ts_on = FALSE;
        if (v->area_on && v->area_page == i && v->shape != 2 && px >= v->ar[0] && px <= v->ar[0] + v->ar[2] &&
            py >= v->ar[1] && py <= v->ar[1] + v->ar[3]) {
            v->drag = D_AREA_MOVE;
            break;
        }
        v->area_on = FALSE;
        v->inverted = FALSE;
        v->area_page = i;
        v->shape = v->tool == T_ELLIPSE_SEL ? 1 : 0;
        v->drag = D_AREA_NEW;
        break;
    case T_LASSO_SEL:
        v->ts_on = FALSE;
        v->area_on = FALSE;
        v->inverted = FALSE;
        v->area_page = i;
        v->shape = 2;
        if (v->lasso)
            g_array_unref(v->lasso);
        v->lasso = g_array_new(FALSE, FALSE, sizeof(double));
        g_array_append_val(v->lasso, px);
        g_array_append_val(v->lasso, py);
        v->drag = D_LASSO;
        break;
    case T_ALPHA:
        if (!p->pdf)
            v->drag = D_ALPHA;
        break;
    case T_SKETCH:
    case T_DRAW: {
        struct mark *n = mark_new(MK_SKETCH, p->id);
        apply_style(v, n);
        n->fill.alpha = 0;
        n->pts = g_array_new(FALSE, FALSE, sizeof(double));
        g_array_append_val(n->pts, px);
        g_array_append_val(n->pts, py);
        v->drag_mark = n;
        v->drag = D_SKETCH;
        break;
    }
    case T_SHAPE: {
        struct mark *n = mark_new(v->w->shape, p->id);
        apply_style(v, n);
        if (n->kind == MK_LINE || n->kind == MK_ARROW)
            line_set(n, px, py, px, py);
        else
            n->x = px, n->y = py, n->w = n->h = 0;
        v->drag_mark = n;
        v->drag = D_SHAPE_NEW;
        break;
    }
    case T_TEXTBOX: {
        struct mark *n = mark_new(MK_TEXT, p->id);
        apply_style(v, n);
        n->fill.alpha = 0;
        n->stroke.alpha = 0;
        n->text = g_strdup("Text");
        n->w = S(140) / scale_of(v), n->h = S(28) / scale_of(v);
        n->x = px, n->y = py - n->h / 2;
        marks_edit(v, NULL, n);
        edit_text(v, n);
        break;
    }
    case T_NOTE: {
        struct mark *n = mark_new(MK_NOTE, p->id);
        n->w = n->h = S(22) / scale_of(v);
        n->x = px - n->w / 2, n->y = py - n->h / 2;
        marks_edit(v, NULL, n);
        edit_text(v, n);
        break;
    }
    case T_SIGN: {
        GPtrArray *sigs = signatures_load();
        if (sigs && v->w->sign_index >= 0 && v->w->sign_index < (int)sigs->len) {
            GArray *pts = g_ptr_array_index(sigs, v->w->sign_index);
            struct mark *n = mark_new(MK_SIGNATURE, p->id);
            n->stroke = (GdkRGBA){0, 0, 0, 1};
            n->width = 1.5;
            n->pts = g_array_sized_new(FALSE, FALSE, sizeof(double), pts->len);
            g_array_append_vals(n->pts, pts->data, pts->len);
            n->w = S(180) / scale_of(v), n->h = n->w / 3;
            n->x = px - n->w / 2, n->y = py - n->h / 2;
            marks_edit(v, NULL, n);
        }
        if (sigs)
            g_ptr_array_unref(sigs);
        win_set_tool(v->w, d->kind == DOC_PDF ? T_TEXT : T_RECT_SEL);
        break;
    }
    }
    gtk_widget_queue_draw(v->layout);
    return TRUE;
}

static void cursor_for(struct view *v, double lx, double ly)
{
    if (v->tool != T_TEXT)
        return;
    int i = page_at(v, lx, ly);
    gboolean on_link = FALSE;
    if (i >= 0 && PAGE(v->w->doc, i)->pdf) {
        double px, py;
        to_page(v, i, lx, ly, &px, &py);
        on_link = link_at(v, i, px, py) != NULL;
    }
    GdkWindow *bin = gtk_layout_get_bin_window(GTK_LAYOUT(v->layout));
    GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(bin), on_link ? "pointer" : "text");
    gdk_window_set_cursor(bin, c);
    if (c)
        g_object_unref(c);
}

static gboolean on_motion(GtkWidget *wd, GdkEventMotion *e, gpointer data)
{
    (void)wd;
    struct view *v = data;
    v->mx = e->x, v->my = e->y;
    v->pointer_in = TRUE;
    if (v->magnifier)
        gtk_widget_queue_draw(v->layout);
    if (v->drag == D_NONE) {
        cursor_for(v, e->x, e->y);
        return FALSE;
    }
    if (hypot(e->x - v->lx0, e->y - v->ly0) > S(3))
        v->moved = TRUE;
    int i = v->drag_page;
    double px, py;
    to_page(v, i, e->x, e->y, &px, &py);
    struct page *p = PAGE(v->w->doc, i);
    double dx = px - v->px0, dy = py - v->py0;
    struct mark *m = v->drag_mark;
    switch (v->drag) {
    case D_TEXT:
    case D_MARKUP:
        if (v->moved)
            v->ts_on = TRUE;
        v->ts.x2 = CLAMP(px, 0, p->w), v->ts.y2 = CLAMP(py, 0, p->h);
        break;
    case D_AREA_NEW: {
        double x1 = CLAMP(MIN(v->px0, px), 0, p->w), y1 = CLAMP(MIN(v->py0, py), 0, p->h);
        double x2 = CLAMP(MAX(v->px0, px), 0, p->w), y2 = CLAMP(MAX(v->py0, py), 0, p->h);
        if (e->state & GDK_SHIFT_MASK) {        /* Shift: a square (or a circle) */
            double s = MAX(x2 - x1, y2 - y1);
            x2 = MIN(x1 + s, p->w), y2 = MIN(y1 + s, p->h);
        }
        v->ar[0] = x1, v->ar[1] = y1, v->ar[2] = x2 - x1, v->ar[3] = y2 - y1;
        v->area_on = v->moved;
        break;
    }
    case D_AREA_MOVE:
        v->ar[0] = CLAMP(v->ar[0] + dx, 0, p->w - v->ar[2]);
        v->ar[1] = CLAMP(v->ar[1] + dy, 0, p->h - v->ar[3]);
        v->px0 = px, v->py0 = py;
        break;
    case D_LASSO:
        px = CLAMP(px, 0, p->w), py = CLAMP(py, 0, p->h);
        g_array_append_val(v->lasso, px);
        g_array_append_val(v->lasso, py);
        v->area_on = TRUE;
        break;
    case D_MARK_MOVE:
        m->x = v->sel->x + dx, m->y = v->sel->y + dy;
        if (m->quads) {
            for (guint k = 0; k < m->quads->len; k += 4) {
                g_array_index(m->quads, double, k) = g_array_index(v->sel->quads, double, k) + dx;
                g_array_index(m->quads, double, k + 1) = g_array_index(v->sel->quads, double, k + 1) + dy;
            }
        }
        break;
    case D_MARK_SIZE: {
        struct mark *o = v->sel;
        if (m->kind == MK_LINE || m->kind == MK_ARROW) {
            double x1 = o->x + o->ax * o->w, y1 = o->y + o->ay * o->h;
            double x2 = o->x + o->bx * o->w, y2 = o->y + o->by * o->h;
            if (v->handle == 0)
                x1 = px, y1 = py;
            else
                x2 = px, y2 = py;
            line_set(m, x1, y1, x2, y2);
            break;
        }
        static const int hx[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, hy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
        double x1 = o->x, y1 = o->y, x2 = o->x + o->w, y2 = o->y + o->h;
        if (hx[v->handle] < 0) x1 = MIN(px, x2 - 1);
        if (hx[v->handle] > 0) x2 = MAX(px, x1 + 1);
        if (hy[v->handle] < 0) y1 = MIN(py, y2 - 1);
        if (hy[v->handle] > 0) y2 = MAX(py, y1 + 1);
        if ((e->state & GDK_SHIFT_MASK) && hx[v->handle] && hy[v->handle]) {  /* keep the shape */
            double r = o->w / MAX(o->h, 1e-3);
            if ((x2 - x1) / r > y2 - y1) {
                if (hy[v->handle] < 0) y1 = y2 - (x2 - x1) / r; else y2 = y1 + (x2 - x1) / r;
            } else {
                if (hx[v->handle] < 0) x1 = x2 - (y2 - y1) * r; else x2 = x1 + (y2 - y1) * r;
            }
        }
        m->x = x1, m->y = y1, m->w = x2 - x1, m->h = y2 - y1;
        break;
    }
    case D_SHAPE_NEW:
        if (m->kind == MK_LINE || m->kind == MK_ARROW) {
            line_set(m, v->px0, v->py0, px, py);
        } else {
            double x2 = px, y2 = py;
            if (e->state & GDK_SHIFT_MASK) {
                double s = MAX(fabs(px - v->px0), fabs(py - v->py0));
                x2 = v->px0 + (px < v->px0 ? -s : s), y2 = v->py0 + (py < v->py0 ? -s : s);
            }
            m->x = MIN(v->px0, x2), m->y = MIN(v->py0, y2);
            m->w = fabs(x2 - v->px0), m->h = fabs(y2 - v->py0);
        }
        break;
    case D_SKETCH:
        g_array_append_val(m->pts, px);
        g_array_append_val(m->pts, py);
        break;
    default:
        break;
    }
    gtk_widget_queue_draw(v->layout);
    return TRUE;
}

/* A sketch's absolute points into 0..1 of its bounding box. */
static void sketch_normalise(struct mark *m)
{
    double *p = (double *)m->pts->data;
    guint n = m->pts->len;
    double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
    for (guint i = 0; i < n; i += 2)
        x1 = MIN(x1, p[i]), y1 = MIN(y1, p[i + 1]), x2 = MAX(x2, p[i]), y2 = MAX(y2, p[i + 1]);
    m->x = x1, m->y = y1, m->w = MAX(x2 - x1, 1), m->h = MAX(y2 - y1, 1);
    for (guint i = 0; i < n; i += 2)
        p[i] = (p[i] - x1) / m->w, p[i + 1] = (p[i + 1] - y1) / m->h;
}

static gboolean on_release(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    (void)wd;
    struct view *v = data;
    if (e->button != 1)
        return FALSE;
    enum drag dr = v->drag;
    v->drag = D_NONE;
    struct mark *m = v->drag_mark;
    v->drag_mark = NULL;
    switch (dr) {
    case D_TEXT:
        if (!v->moved && v->press_link && e->type == GDK_BUTTON_RELEASE && v->ts_style == POPPLER_SELECTION_GLYPH) {
            v->ts_on = FALSE;
            follow(v, v->press_link->action);
        }
        break;
    case D_MARKUP:
        if (v->ts_on) {
            static const enum mark_kind k[] = {[T_HIGHLIGHT] = MK_HIGHLIGHT, [T_UNDERLINE] = MK_UNDERLINE,
                                               [T_STRIKE] = MK_STRIKE};
            view_markup_text(v, k[v->tool]);
        }
        break;
    case D_LASSO:
        if (v->lasso->len < 6) {
            v->area_on = FALSE;
            break;
        }
        {
            double *p = (double *)v->lasso->data;
            double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
            for (guint i = 0; i < v->lasso->len; i += 2)
                x1 = MIN(x1, p[i]), y1 = MIN(y1, p[i + 1]), x2 = MAX(x2, p[i]), y2 = MAX(y2, p[i + 1]);
            v->ar[0] = x1, v->ar[1] = y1, v->ar[2] = x2 - x1, v->ar[3] = y2 - y1;
        }
        break;
    case D_MARK_MOVE:
    case D_MARK_SIZE:
        if (v->moved && v->sel) {
            marks_edit(v, v->sel, m);
            m = NULL;
        } else if (v->sel && v->sel->kind == MK_NOTE && !v->moved) {
            edit_text(v, v->sel);
        }
        break;
    case D_SHAPE_NEW:
        if (!v->moved) {        /* a click: the shape at its usual size */
            if (m->kind == MK_LINE || m->kind == MK_ARROW)
                line_set(m, v->px0, v->py0, v->px0 + 100, v->py0);
            else
                m->w = m->h = 80;
        }
        marks_edit(v, NULL, m);
        m = NULL;
        break;
    case D_SKETCH:
        if (m->pts->len >= 4) {
            sketch_normalise(m);
            marks_edit(v, NULL, m);
            m = NULL;
        }
        break;
    case D_ALPHA: {
        /* Instant Alpha: the further the drag, the more alike colours go. */
        double t = CLAMP(.06 + hypot(e->x - v->lx0, e->y - v->ly0) / S(250), 0, 1);
        int i = v->drag_page;
        GdkPixbuf *src = doc_page_pixels(v->w->doc, i);
        GdkPixbuf *out = img_instant_alpha(src, (int)v->px0, (int)v->py0, t);
        g_object_unref(src);
        if (out) {
            doc_set_pixels(v->w->doc, i, out);
            g_object_unref(out);
            win_doc_changed(v->w);
        }
        break;
    }
    default:
        break;
    }
    if (m)
        mark_unref(m);
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
    return TRUE;
}

static gboolean on_leave(GtkWidget *wd, GdkEventCrossing *e, gpointer data)
{
    (void)wd, (void)e;
    struct view *v = data;
    v->pointer_in = FALSE;
    if (v->magnifier)
        gtk_widget_queue_draw(v->layout);
    return FALSE;
}

static gboolean on_scroll(GtkWidget *wd, GdkEventScroll *e, gpointer data)
{
    (void)wd;
    struct view *v = data;
    double dy = 0;
    if (e->direction == GDK_SCROLL_UP)
        dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN)
        dy = 1;
    else if (e->direction == GDK_SCROLL_SMOOTH)
        dy = e->delta_y;
    if (e->state & GDK_CONTROL_MASK) {      /* Ctrl + wheel: zoom about the pointer */
        v->fit = FALSE;
        set_zoom(v, v->zoom * pow(1.1, -dy), e->x - gtk_adjustment_get_value(v->ha),
                 e->y - gtk_adjustment_get_value(v->va));
        return TRUE;
    }
    if (v->mode == DM_SINGLE && dy != 0) {
        /* Single Page: scrolling past an end turns the page. */
        double val = gtk_adjustment_get_value(v->va);
        double max = gtk_adjustment_get_upper(v->va) - gtk_adjustment_get_page_size(v->va);
        if (dy > 0 && val >= max - 1 && v->w->page + 1 < NPAGES(v->w->doc)) {
            win_set_page(v->w, v->w->page + 1);
            return TRUE;
        }
        if (dy < 0 && val <= 0 && v->w->page > 0) {
            win_set_page(v->w, v->w->page - 1);
            scroll_to(v, gtk_adjustment_get_value(v->ha), v->lh);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_key(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd;
    struct view *v = data;
    double step = e->state & GDK_SHIFT_MASK ? 10 : 1;
    switch (e->keyval) {
    case GDK_KEY_Delete:
    case GDK_KEY_BackSpace:
        if (v->sel) {
            view_delete_selected_mark(v);
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_Escape:
        if (v->drag != D_NONE) {
            v->drag = D_NONE;
            if (v->drag_mark)
                mark_unref(v->drag_mark);
            v->drag_mark = NULL;
        }
        view_deselect(v);
        return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Up:
    case GDK_KEY_Down:
        if (v->sel) {           /* nudge the selected mark */
            struct mark *m = mark_copy(v->sel);
            double dx = e->keyval == GDK_KEY_Left ? -step : e->keyval == GDK_KEY_Right ? step : 0;
            double dy = e->keyval == GDK_KEY_Up ? -step : e->keyval == GDK_KEY_Down ? step : 0;
            m->x += dx, m->y += dy;
            for (guint k = 0; m->quads && k < m->quads->len; k += 4)
                g_array_index(m->quads, double, k) += dx, g_array_index(m->quads, double, k + 1) += dy;
            marks_edit(v, v->sel, m);
            return TRUE;
        }
        if ((e->keyval == GDK_KEY_Left || e->keyval == GDK_KEY_Right) &&
            gtk_adjustment_get_upper(v->ha) <= gtk_adjustment_get_page_size(v->ha) + 1) {
            win_set_page(v->w, v->w->page + (e->keyval == GDK_KEY_Left ? -1 : 1));
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_space:
    case GDK_KEY_Page_Down:
    case GDK_KEY_Page_Up: {
        gboolean back = e->keyval == GDK_KEY_Page_Up || (e->state & GDK_SHIFT_MASK);
        if (v->mode == DM_SINGLE) {
            win_set_page(v->w, v->w->page + (back ? -1 : 1));
            return TRUE;
        }
        double ps = gtk_adjustment_get_page_size(v->va);
        gtk_adjustment_set_value(v->va, gtk_adjustment_get_value(v->va) + (back ? -ps : ps) * .9);
        return TRUE;
    }
    case GDK_KEY_grave:         /* Tools > Show Magnifier: ` (here, so text fields keep the key) */
        view_set_magnifier(v, !v->magnifier);
        win_update(v->w);
        return TRUE;
    case GDK_KEY_Home:
        win_set_page(v->w, 0);
        return TRUE;
    case GDK_KEY_End:
        win_set_page(v->w, NPAGES(v->w->doc) - 1);
        return TRUE;
    }
    return FALSE;
}

void view_set_magnifier(struct view *v, gboolean on)
{
    v->magnifier = on;
    gtk_widget_queue_draw(v->layout);
}

gboolean view_magnifier(struct view *v)
{
    return v->magnifier;
}

/* --- search ------------------------------------------------------------------------ */

static void hit_free(gpointer p)
{
    struct hit *h = p;
    g_free(h->context);
    g_free(h);
}

void view_search_clear(struct view *v)
{
    if (v->hits)
        g_ptr_array_unref(v->hits);
    v->hits = NULL;
    v->cur_hit = -1;
    gtk_widget_queue_draw(v->layout);
}

/* The line around a hit, for the sidebar's list of results. */
static char *context_of(PopplerPage *pp, double pw, const PopplerRectangle *r)
{
    PopplerRectangle line = {0, r->y1 - 1, pw, r->y2 + 1};
    char *t = poppler_page_get_text_for_area(pp, &line);
    if (!t)
        return g_strdup("");
    for (char *c = t; *c; c++)
        if (*c == '\n' || *c == '\t')
            *c = ' ';
    g_strstrip(t);
    if (g_utf8_strlen(t, -1) > 90) {
        char *cut = g_utf8_offset_to_pointer(t, 90);
        *cut = 0;
        char *e = g_strconcat(t, "…", NULL);
        g_free(t);
        t = e;
    }
    return t;
}

int view_search(struct view *v, const char *text, gboolean whole_words, gboolean case_sensitive)
{
    view_search_clear(v);
    struct doc *d = v->w->doc;
    if (!text || !*text)
        return 0;
    v->hits = g_ptr_array_new_with_free_func(hit_free);
    PopplerFindFlags fl = POPPLER_FIND_DEFAULT | (whole_words ? POPPLER_FIND_WHOLE_WORDS_ONLY : 0) |
                          (case_sensitive ? POPPLER_FIND_CASE_SENSITIVE : 0) |
                          POPPLER_FIND_IGNORE_DIACRITICS | POPPLER_FIND_MULTILINE;
    for (int i = 0; i < NPAGES(d); i++) {
        struct page *p = PAGE(d, i);
        if (!p->pdf)
            continue;
        PopplerPage *pp = poppler_document_get_page(p->pdf, p->index);
        if (!pp)
            continue;
        GList *l = poppler_page_find_text_with_options(pp, text, fl);
        for (GList *it = l; it; it = it->next) {
            PopplerRectangle *r = it->data;
            struct hit *h = g_new(struct hit, 1);
            h->page = i;
            /* Results count up from the bottom of the page; ours go down. */
            h->r = (PopplerRectangle){r->x1, p->h - r->y2, r->x2, p->h - r->y1};
            h->context = context_of(pp, p->w, &h->r);
            g_ptr_array_add(v->hits, h);
            poppler_rectangle_free(r);
        }
        g_list_free(l);
        g_object_unref(pp);
    }
    if (v->hits->len) {
        /* Start at the first hit on or after the page being read. */
        v->cur_hit = 0;
        for (guint k = 0; k < v->hits->len; k++)
            if (((struct hit *)g_ptr_array_index(v->hits, k))->page >= v->w->page) {
                v->cur_hit = k;
                break;
            }
        view_search_select(v, v->cur_hit);
    }
    gtk_widget_queue_draw(v->layout);
    return v->hits->len;
}

void view_search_select(struct view *v, int i)
{
    if (!v->hits || i < 0 || i >= (int)v->hits->len)
        return;
    v->cur_hit = i;
    struct hit *h = g_ptr_array_index(v->hits, i);
    if (v->mode == DM_SINGLE && h->page != v->w->page)
        view_go_page(v, h->page);
    v->w->page = h->page;
    reveal(v, box_to_canvas(v, h->page, h->r.x1, h->r.y1, h->r.x2 - h->r.x1, h->r.y2 - h->r.y1));
    gtk_widget_queue_draw(v->layout);
    win_update(v->w);
}

void view_search_next(struct view *v, int dir)
{
    if (!v->hits || !v->hits->len)
        return;
    int n = v->hits->len;
    view_search_select(v, ((v->cur_hit + dir) % n + n) % n);
}

int view_search_count(struct view *v)
{
    return v->hits ? (int)v->hits->len : 0;
}

int view_search_current(struct view *v)
{
    return v->cur_hit;
}

GList *view_search_results(struct view *v)
{
    GList *l = NULL;
    for (guint i = 0; v->hits && i < v->hits->len; i++)
        l = g_list_prepend(l, g_ptr_array_index(v->hits, i));
    return g_list_reverse(l);
}

/* --- making it ---------------------------------------------------------------------- */

static void on_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct view *v = data;
    if (v->idle)
        g_source_remove(v->idle);
    cache_clear(v);
    if (v->links)
        poppler_page_free_link_mapping(v->links);
    if (v->links_page)
        page_unref(v->links_page);
    if (v->hits)
        g_ptr_array_unref(v->hits);
    if (v->lasso)
        g_array_unref(v->lasso);
    g_free(v->rects);
    g_free(v);
}

static void on_realize(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct view *v = data;
    view_set_tool(v, v->tool);
}

struct view *view_new(struct win *w)
{
    struct view *v = g_new0(struct view, 1);
    v->w = w;
    v->zoom = 1;
    v->cur_hit = -1;
    v->mode = w->doc->kind == DOC_PDF ? prefs.pdf_mode : DM_CONTINUOUS;
    v->tool = w->doc->kind == DOC_PDF ? T_TEXT : T_RECT_SEL;
    v->layout = gtk_layout_new(NULL, NULL);
    gtk_widget_set_can_focus(v->layout, TRUE);
    gtk_widget_add_events(v->layout, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                         GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK |
                                         GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK);
    v->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(v->scroll), v->layout);
    v->ha = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(v->layout));
    v->va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(v->layout));
    g_signal_connect(v->layout, "draw", G_CALLBACK(on_draw), v);
    g_signal_connect(v->layout, "size-allocate", G_CALLBACK(on_size), v);
    g_signal_connect(v->layout, "button-press-event", G_CALLBACK(on_press), v);
    g_signal_connect(v->layout, "button-release-event", G_CALLBACK(on_release), v);
    g_signal_connect(v->layout, "motion-notify-event", G_CALLBACK(on_motion), v);
    g_signal_connect(v->layout, "leave-notify-event", G_CALLBACK(on_leave), v);
    g_signal_connect(v->layout, "scroll-event", G_CALLBACK(on_scroll), v);
    g_signal_connect(v->layout, "key-press-event", G_CALLBACK(on_key), v);
    g_signal_connect(v->layout, "realize", G_CALLBACK(on_realize), v);
    g_signal_connect(v->layout, "destroy", G_CALLBACK(on_destroy), v);
    g_signal_connect(v->va, "value-changed", G_CALLBACK(on_scrolled), v);
    g_signal_connect(v->ha, "value-changed", G_CALLBACK(on_scrolled), v);
    return v;
}

GtkWidget *view_widget(struct view *v)
{
    return v->scroll;
}

/* --- the slideshow ----------------------------------------------------------------- */

struct show {
    struct win *w;
    int page;
    GtkWidget *win;
    cairo_surface_t *s;         /* the page, fitted to the screen */
    int sw, sh;
};

static void show_render(struct show *sh, int width, int height)
{
    struct doc *d = sh->w->doc;
    if (sh->s)
        cairo_surface_destroy(sh->s);
    struct page *p = PAGE(d, sh->page);
    double pw, ph;
    page_size(p, &pw, &ph);
    double s = MIN(width / pw, height / ph);
    sh->sw = MAX(1, (int)(pw * s)), sh->sh = MAX(1, (int)(ph * s));
    sh->s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sh->sw, sh->sh);
    cairo_t *cr = cairo_create(sh->s);
    cairo_scale(cr, s, s);
    page_render(cr, p, d->st->marks);
    cairo_destroy(cr);
}

static gboolean show_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    struct show *sh = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (!sh->s || (sh->sw != w && sh->sh != h))
        show_render(sh, w, h);
    cairo_set_source_surface(cr, sh->s, (w - sh->sw) / 2, (h - sh->sh) / 2);
    cairo_paint(cr);
    return TRUE;
}

static void show_go(struct show *sh, int page)
{
    int n = NPAGES(sh->w->doc);
    if (page >= n) {
        gtk_widget_destroy(sh->win);    /* past the last page: done */
        return;
    }
    sh->page = MAX(page, 0);
    if (sh->s)
        cairo_surface_destroy(sh->s);
    sh->s = NULL;
    gtk_widget_queue_draw(sh->win);
}

static gboolean show_key(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd;
    struct show *sh = data;
    switch (e->keyval) {
    case GDK_KEY_Escape:
        gtk_widget_destroy(sh->win);
        break;
    case GDK_KEY_Right: case GDK_KEY_Down: case GDK_KEY_space: case GDK_KEY_Page_Down: case GDK_KEY_Return:
        show_go(sh, sh->page + 1);
        break;
    case GDK_KEY_Left: case GDK_KEY_Up: case GDK_KEY_BackSpace: case GDK_KEY_Page_Up:
        show_go(sh, sh->page - 1);
        break;
    case GDK_KEY_Home:
        show_go(sh, 0);
        break;
    case GDK_KEY_End:
        show_go(sh, NPAGES(sh->w->doc) - 1);
        break;
    }
    return TRUE;
}

static gboolean show_click(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    (void)wd;
    struct show *sh = data;
    show_go(sh, sh->page + (e->button == 3 ? -1 : 1));
    return TRUE;
}

static void show_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct show *sh = data;
    win_set_page(sh->w, MIN(sh->page, NPAGES(sh->w->doc) - 1));
    if (sh->s)
        cairo_surface_destroy(sh->s);
    g_free(sh);
}

void view_slideshow(struct win *w)
{
    if (!NPAGES(w->doc))
        return;
    struct show *sh = g_new0(struct show, 1);
    sh->w = w;
    sh->page = w->page;
    sh->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(sh->win), da);
    gtk_widget_add_events(sh->win, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(da, "draw", G_CALLBACK(show_draw), sh);
    g_signal_connect(sh->win, "key-press-event", G_CALLBACK(show_key), sh);
    g_signal_connect(sh->win, "button-press-event", G_CALLBACK(show_click), sh);
    g_signal_connect(sh->win, "destroy", G_CALLBACK(show_destroy), sh);
    gtk_window_fullscreen(GTK_WINDOW(sh->win));
    gtk_widget_show_all(sh->win);
    /* Hide the pointer while showing. */
    GdkWindow *gw = gtk_widget_get_window(sh->win);
    GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(gw), "none");
    gdk_window_set_cursor(gw, c);
    if (c)
        g_object_unref(c);
}

/* --- thumbnails ----------------------------------------------------------------------- */

#define THUMB_EDGE 96           /* long side; enough to read a title page */

struct thumb { struct page *pg; int size; cairo_surface_t *s; };
static GHashTable *thumbs;      /* struct page* -> struct thumb* */
static GQueue thumb_queue = G_QUEUE_INIT;
static guint thumb_idle;

static void thumb_free(gpointer p)
{
    struct thumb *t = p;
    if (t->s)
        cairo_surface_destroy(t->s);
    page_unref(t->pg);
    g_free(t);
}

void thumbs_forget(void)
{
    if (thumbs)
        g_hash_table_remove_all(thumbs);
}

static void thumbs_redraw(void)
{
    for (GList *l = win_list(); l; l = l->next) {
        struct win *w = l->data;
        if (w->thumbs)
            gtk_widget_queue_draw(w->thumbs);
        if (w->contact)
            gtk_widget_queue_draw(w->contact);
    }
}

static void thumb_paint(struct thumb *t)
{
    double pw, ph;
    page_size(t->pg, &pw, &ph);
    int edge = MIN(t->size, THUMB_EDGE);
    double s = edge / MAX(pw, ph);
    int w = MAX(1, (int)(pw * s)), h = MAX(1, (int)(ph * s));
    t->s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(t->s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, s, s);
    if (!t->pg->pdf && s < 1) {
        GdkPixbuf *sm = gdk_pixbuf_scale_simple(t->pg->pix, w, h, GDK_INTERP_BILINEAR);
        cairo_identity_matrix(cr);
        gdk_cairo_set_source_pixbuf(cr, sm, 0, 0);
        cairo_paint(cr);
        g_object_unref(sm);
    } else {
        page_render(cr, t->pg, NULL);
    }
    cairo_destroy(cr);
}

static gboolean thumb_work(gpointer data)
{
    (void)data;
    struct thumb *t = g_queue_pop_head(&thumb_queue);
    if (!t) {
        thumb_idle = 0;
        thumbs_redraw();
        return G_SOURCE_REMOVE;
    }
    thumb_paint(t);
    g_hash_table_replace(thumbs, t->pg, t);
    thumbs_redraw();            /* one page at a time: they appear as they land */
    return G_SOURCE_CONTINUE;
}

static void thumb_ensure_idle(void)
{
    if (!thumb_idle)
        thumb_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, thumb_work, NULL, NULL);
}

cairo_surface_t *thumb_get(struct win *w, struct page *p, int size, gboolean *pending)
{
    (void)w;
    if (!thumbs)
        thumbs = g_hash_table_new_full(NULL, NULL, NULL, thumb_free);
    if (g_hash_table_size(thumbs) > 800)
        g_hash_table_remove_all(thumbs);
    if (size > THUMB_EDGE)
        size = THUMB_EDGE;
    struct thumb *t = g_hash_table_lookup(thumbs, p);
    *pending = FALSE;
    if (t && t->s)
        return t->s;
    for (GList *l = thumb_queue.head; l; l = l->next)
        if (((struct thumb *)l->data)->pg == p) {
            *pending = TRUE;
            return NULL;
        }
    struct thumb *n = g_new0(struct thumb, 1);
    n->pg = page_ref(p);
    n->size = size;
    /* The page on screen first, then the rest of the sidebar. */
    if (w && PAGE(w->doc, w->page) == p)
        g_queue_push_head(&thumb_queue, n);
    else
        g_queue_push_tail(&thumb_queue, n);
    thumb_ensure_idle();
    *pending = TRUE;
    return NULL;
}

/* Paint the first few now so the sidebar is not a column of empty boxes
   while the idle queue works through the rest. */
void thumbs_fill_now(struct win *w, int from, int n)
{
    if (!thumbs)
        thumbs = g_hash_table_new_full(NULL, NULL, NULL, thumb_free);
    for (int i = 0; i < n; i++) {
        int pg = from + i;
        if (pg < 0 || pg >= NPAGES(w->doc))
            continue;
        struct page *p = PAGE(w->doc, pg);
        struct thumb *have = g_hash_table_lookup(thumbs, p);
        if (have && have->s)
            continue;
        for (GList *l = thumb_queue.head; l; l = l->next) {
            struct thumb *q = l->data;
            if (q->pg == p) {
                g_queue_remove(&thumb_queue, q);
                thumb_free(q);
                break;
            }
        }
        struct thumb *t = g_new0(struct thumb, 1);
        t->pg = page_ref(p);
        t->size = THUMB_EDGE;
        thumb_paint(t);
        g_hash_table_replace(thumbs, t->pg, t);
    }
}

void view_select_mark(struct view *v, struct mark *m)
{
    struct doc *d = v->w->doc;
    for (int i = 0; i < NPAGES(d); i++)
        if (PAGE(d, i)->id == m->page) {
            win_set_page(v->w, i);
            v->sel = m;
            v->ts_on = v->area_on = FALSE;
            reveal(v, box_to_canvas(v, i, m->x, m->y, m->w, m->h));
            gtk_widget_queue_draw(v->layout);
            win_update(v->w);
            return;
        }
}

struct mark *view_selected_mark(struct view *v)
{
    return v->sel;
}

double view_screen_scale(struct view *v)
{
    return scale_of(v);
}
