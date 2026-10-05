/*
 * marks.c — markup: Preview's shapes, text boxes, notes, sketches and
 * signatures, and the text highlights.
 *
 * A mark is a box on a page plus how to fill it (preview.h). It is drawn
 * here, the same way on screen, into an image when a picture is saved, and
 * into a PDF annotation when a PDF is saved:
 *   - highlight / underline / strike through: PDF text-markup annotations,
 *     which every PDF reader knows and lists;
 *   - notes: PDF "text" annotations (the yellow sticky note);
 *   - everything else: a stamp annotation that carries its own picture,
 *     drawn at three times the page's resolution. poppler can't give a PDF
 *     square a border width or a line an arrow head, and a stamp looks
 *     exactly like what was on the screen in every reader.
 */
#include <math.h>
#include <string.h>

#include "preview.h"

struct mark *mark_new(enum mark_kind kind, unsigned page)
{
    struct mark *m = g_new0(struct mark, 1);
    m->refs = 1;
    m->kind = kind;
    m->page = page;
    m->stroke = (GdkRGBA){.85, .1, .1, 1};
    m->fill = (GdkRGBA){0, 0, 0, 0};
    m->text_color = (GdkRGBA){0, 0, 0, 1};
    m->width = 2;
    m->sides = kind == MK_STAR ? 5 : 6;
    m->magnify = 2;
    m->ax = 0, m->ay = 0, m->bx = 1, m->by = 1;
    m->font = g_strdup("DejaVu Sans 14");
    m->align = PANGO_ALIGN_LEFT;
    return m;
}

struct mark *mark_ref(struct mark *m)
{
    m->refs++;
    return m;
}

void mark_unref(struct mark *m)
{
    if (!m || --m->refs)
        return;
    if (m->pts)
        g_array_unref(m->pts);
    if (m->quads)
        g_array_unref(m->quads);
    g_free(m->text);
    g_free(m->font);
    g_free(m);
}

struct mark *mark_copy(const struct mark *m)
{
    struct mark *c = g_new(struct mark, 1);
    *c = *m;
    c->refs = 1;
    c->text = g_strdup(m->text);
    c->font = g_strdup(m->font);
    if (m->pts) {
        c->pts = g_array_sized_new(FALSE, FALSE, sizeof(double), m->pts->len);
        g_array_append_vals(c->pts, m->pts->data, m->pts->len);
    }
    if (m->quads) {
        c->quads = g_array_sized_new(FALSE, FALSE, sizeof(double), m->quads->len);
        g_array_append_vals(c->quads, m->quads->data, m->quads->len);
    }
    return c;
}

const char *mark_name(enum mark_kind k)
{
    static const char *const n[] = {
        [MK_HIGHLIGHT] = "Highlight", [MK_UNDERLINE] = "Underline", [MK_STRIKE] = "Strikethrough",
        [MK_LINE] = "Line",           [MK_ARROW] = "Arrow",         [MK_RECT] = "Rectangle",
        [MK_RRECT] = "Rounded Rectangle", [MK_OVAL] = "Oval",       [MK_BUBBLE] = "Speech Bubble",
        [MK_STAR] = "Star",           [MK_POLYGON] = "Polygon",     [MK_LOUPE] = "Loupe",
        [MK_TEXT] = "Text",           [MK_NOTE] = "Note",           [MK_SKETCH] = "Sketch",
        [MK_SIGNATURE] = "Signature",
    };
    return n[k];
}

/* --- drawing ---------------------------------------------------------------------------- */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* A rectangle with rounded corners and a tail at the bottom left. */
static void bubble(cairo_t *cr, double x, double y, double w, double h)
{
    double bh = h * .78, r = MIN(w, bh) * .2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + bh - r, r, 0, M_PI / 2);
    cairo_line_to(cr, x + w * .36, y + bh);
    cairo_line_to(cr, x + w * .12, y + h);
    cairo_line_to(cr, x + w * .20, y + bh);
    cairo_arc(cr, x + r, y + bh - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void star(cairo_t *cr, double x, double y, double w, double h, int points, gboolean polygon)
{
    int n = polygon ? points : points * 2;
    for (int i = 0; i < n; i++) {
        double a = -M_PI / 2 + i * 2 * M_PI / n;
        double r = polygon || i % 2 == 0 ? 1 : .42;
        double px = x + w / 2 + cos(a) * r * w / 2, py = y + h / 2 + sin(a) * r * h / 2;
        if (i == 0)
            cairo_move_to(cr, px, py);
        else
            cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
}

/* The page's own content, for the loupe to magnify. */
static void draw_page_content(cairo_t *cr, const struct page *pg)
{
    if (pg->pdf) {
        PopplerPage *pp = poppler_document_get_page(pg->pdf, pg->index);
        if (pp) {
            cairo_save(cr);
            cairo_rectangle(cr, 0, 0, pg->w, pg->h);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
            poppler_page_render(pp, cr);
            cairo_restore(cr);
            g_object_unref(pp);
        }
    } else if (pg->pix) {
        gdk_cairo_set_source_pixbuf(cr, pg->pix, 0, 0);
        cairo_paint(cr);
    }
}

static void path_of(cairo_t *cr, const struct mark *m)
{
    double x = m->x, y = m->y, w = m->w, h = m->h;
    switch (m->kind) {
    case MK_RECT:
        cairo_rectangle(cr, x, y, w, h);
        break;
    case MK_RRECT:
        rrect(cr, x, y, w, h, MIN(w, h) * .18);
        break;
    case MK_OVAL:
    case MK_LOUPE:
        cairo_save(cr);
        cairo_translate(cr, x + w / 2, y + h / 2);
        cairo_scale(cr, MAX(w, 1e-3) / 2, MAX(h, 1e-3) / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
        break;
    case MK_BUBBLE:
        bubble(cr, x, y, w, h);
        break;
    case MK_STAR:
        star(cr, x, y, w, h, m->sides, FALSE);
        break;
    case MK_POLYGON:
        star(cr, x, y, w, h, m->sides, TRUE);
        break;
    case MK_TEXT:
        cairo_rectangle(cr, x, y, w, h);
        break;
    default:
        break;
    }
}

static void stroke_style(cairo_t *cr, const struct mark *m)
{
    cairo_set_line_width(cr, m->width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (m->dash) {
        double d[] = {m->width * 3, m->width * 2};
        cairo_set_dash(cr, d, 2, 0);
    }
}

static void draw_text(cairo_t *cr, const struct mark *m, double pad_x, double pad_y)
{
    if (!m->text || !*m->text)
        return;
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(m->font ? m->font : "DejaVu Sans 14");
    /* Font sizes are points, and our page space is points (PDF) or pixels
       (images): one unit per point, not pango's 96-dpi screen pixels. */
    if (!pango_font_description_get_size_is_absolute(fd))
        pango_font_description_set_absolute_size(fd, pango_font_description_get_size(fd));
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, (int)(MAX(m->w - 2 * pad_x, 1) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(l, m->align);
    pango_layout_set_text(l, m->text, -1);
    gdk_cairo_set_source_rgba(cr, &m->text_color);
    cairo_move_to(cr, m->x + pad_x, m->y + pad_y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* A sketch's strokes: points are 0..1 in the box, NAN starts a new stroke.
   Drawn as smooth curves through the midpoints. */
static void draw_strokes(cairo_t *cr, const struct mark *m)
{
    if (!m->pts)
        return;
    double *v = (double *)m->pts->data;
    guint n = m->pts->len / 2;
    gboolean open = FALSE;
    double px = 0, py = 0;
    for (guint i = 0; i < n; i++) {
        if (isnan(v[2 * i])) {
            open = FALSE;
            continue;
        }
        double x = m->x + v[2 * i] * m->w, y = m->y + v[2 * i + 1] * m->h;
        if (!open) {
            cairo_move_to(cr, x, y);
            cairo_line_to(cr, x + .01, y);      /* a dot shows too */
            open = TRUE;
        } else {
            cairo_curve_to(cr, px, py, px, py, (px + x) / 2, (py + y) / 2);
        }
        px = x, py = y;
    }
    cairo_stroke(cr);
}

static void draw_note(cairo_t *cr, const struct mark *m)
{
    double x = m->x, y = m->y, s = MIN(m->w, m->h), f = s * .28;
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + s, y);
    cairo_line_to(cr, x + s, y + s - f);
    cairo_line_to(cr, x + s - f, y + s);
    cairo_line_to(cr, x, y + s);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 1, .86, .27);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, .55, .42, 0, .8);
    cairo_set_line_width(cr, MAX(s * .04, .5));
    cairo_stroke(cr);
    cairo_set_line_width(cr, MAX(s * .05, .5));
    for (int i = 0; i < 3; i++) {
        cairo_move_to(cr, x + s * .2, y + s * (.3 + i * .18));
        cairo_line_to(cr, x + s * (i == 2 ? .55 : .8), y + s * (.3 + i * .18));
    }
    cairo_stroke(cr);
}

void mark_draw(cairo_t *cr, const struct mark *m, const struct page *pg, cairo_surface_t *under)
{
    (void)under;
    cairo_save(cr);
    double *q = m->quads ? (double *)m->quads->data : NULL;
    guint nq = m->quads ? m->quads->len / 4 : 0;
    switch (m->kind) {
    case MK_HIGHLIGHT:
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        gdk_cairo_set_source_rgba(cr, &m->fill);
        for (guint i = 0; i < nq; i++)
            cairo_rectangle(cr, q[4 * i], q[4 * i + 1], q[4 * i + 2], q[4 * i + 3]);
        cairo_fill(cr);
        break;
    case MK_UNDERLINE:
    case MK_STRIKE:
        gdk_cairo_set_source_rgba(cr, &m->stroke);
        for (guint i = 0; i < nq; i++) {
            double h = q[4 * i + 3];
            double yy = q[4 * i + 1] + (m->kind == MK_STRIKE ? h * .55 : h * .96);
            cairo_move_to(cr, q[4 * i], yy);
            cairo_line_to(cr, q[4 * i] + q[4 * i + 2], yy);
            cairo_set_line_width(cr, MAX(h * .07, .75));
        }
        cairo_stroke(cr);
        break;
    case MK_LINE:
    case MK_ARROW: {
        double x1 = m->x + m->ax * m->w, y1 = m->y + m->ay * m->h;
        double x2 = m->x + m->bx * m->w, y2 = m->y + m->by * m->h;
        stroke_style(cr, m);
        if (m->shadow) {
            cairo_save(cr);
            cairo_translate(cr, m->width * .8, m->width * .8);
            cairo_set_source_rgba(cr, 0, 0, 0, .3);
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        gdk_cairo_set_source_rgba(cr, &m->stroke);
        double a = atan2(y2 - y1, x2 - x1), hl = MAX(m->width * 4, 8);
        double ex = x2, ey = y2;
        if (m->kind == MK_ARROW) {          /* stop the line inside the head */
            ex -= cos(a) * hl * .6;
            ey -= sin(a) * hl * .6;
        }
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, ex, ey);
        cairo_stroke(cr);
        if (m->kind == MK_ARROW) {
            cairo_set_dash(cr, NULL, 0, 0);
            cairo_move_to(cr, x2, y2);
            cairo_line_to(cr, x2 - cos(a - .45) * hl, y2 - sin(a - .45) * hl);
            cairo_line_to(cr, x2 - cos(a + .45) * hl, y2 - sin(a + .45) * hl);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        break;
    }
    case MK_LOUPE: {
        /* Inside the circle: the page under it, magnified about the centre. */
        double cx = m->x + m->w / 2, cy = m->y + m->h / 2;
        cairo_save(cr);
        path_of(cr, m);
        cairo_clip(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, m->magnify, m->magnify);
        cairo_translate(cr, -cx, -cy);
        draw_page_content(cr, pg);
        cairo_restore(cr);
        stroke_style(cr, m);
        path_of(cr, m);
        gdk_cairo_set_source_rgba(cr, &m->stroke);
        cairo_stroke(cr);
        break;
    }
    case MK_NOTE:
        draw_note(cr, m);
        break;
    case MK_SKETCH:
    case MK_SIGNATURE:
        stroke_style(cr, m);
        gdk_cairo_set_source_rgba(cr, &m->stroke);
        draw_strokes(cr, m);
        break;
    default:
        if (m->shadow) {
            cairo_save(cr);
            cairo_translate(cr, 3, 3);
            path_of(cr, m);
            cairo_set_source_rgba(cr, 0, 0, 0, .25);
            if (m->fill.alpha > 0)
                cairo_fill(cr);
            else {
                stroke_style(cr, m);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        }
        path_of(cr, m);
        if (m->fill.alpha > 0) {
            gdk_cairo_set_source_rgba(cr, &m->fill);
            cairo_fill_preserve(cr);
        }
        /* A text box has no border unless one was chosen. */
        if (m->kind != MK_TEXT || m->stroke.alpha > 0) {
            stroke_style(cr, m);
            gdk_cairo_set_source_rgba(cr, &m->stroke);
            if (m->kind == MK_TEXT && m->stroke.alpha == 0)
                cairo_new_path(cr);
            else
                cairo_stroke(cr);
        }
        cairo_new_path(cr);
        if (m->kind == MK_TEXT)
            draw_text(cr, m, 4, 2);
        else if (m->kind == MK_BUBBLE)
            draw_text(cr, m, m->w * .1, m->h * .08);
        else if (m->text && *m->text && (m->kind == MK_RECT || m->kind == MK_RRECT || m->kind == MK_OVAL))
            draw_text(cr, m, m->w * .12, m->h * .3);
        break;
    }
    cairo_restore(cr);
}

/* Is (x, y) on the mark? Lines by their distance, the rest by their box. */
int mark_hit(const struct mark *m, double x, double y, double slop)
{
    if (m->kind == MK_HIGHLIGHT || m->kind == MK_UNDERLINE || m->kind == MK_STRIKE) {
        double *q = (double *)m->quads->data;
        for (guint i = 0; i < m->quads->len / 4; i++)
            if (x >= q[4 * i] && x <= q[4 * i] + q[4 * i + 2] && y >= q[4 * i + 1] &&
                y <= q[4 * i + 1] + q[4 * i + 3])
                return 1;
        return 0;
    }
    if (m->kind == MK_LINE || m->kind == MK_ARROW) {
        double x1 = m->x + m->ax * m->w, y1 = m->y + m->ay * m->h;
        double x2 = m->x + m->bx * m->w, y2 = m->y + m->by * m->h;
        double dx = x2 - x1, dy = y2 - y1, l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? CLAMP(((x - x1) * dx + (y - y1) * dy) / l2, 0, 1) : 0;
        return hypot(x - (x1 + t * dx), y - (y1 + t * dy)) <= slop + m->width;
    }
    return x >= m->x - slop && x <= m->x + m->w + slop && y >= m->y - slop && y <= m->y + m->h + slop;
}

/* --- saving --------------------------------------------------------------------------- */

static PopplerColor pcolor(const GdkRGBA *c)
{
    PopplerColor p = {(guint16)(c->red * 65535), (guint16)(c->green * 65535), (guint16)(c->blue * 65535)};
    return p;
}

/* Our boxes have y going down from the page's top; PDF's go up from the
   bottom. */
static PopplerRectangle prect(const struct page *pg, double x, double y, double w, double h)
{
    PopplerRectangle r = {x, pg->h - (y + h), x + w, pg->h - y};
    return r;
}

/* How far a mark's drawing reaches past its box (line width, arrow heads,
   shadows), so its stamp isn't cut off. */
static double margin(const struct mark *m)
{
    return m->width * 4 + (m->shadow ? 4 : 0) + 8;
}

void marks_to_pdf(GPtrArray *marks, const struct page *pg, PopplerPage *pp)
{
    PopplerDocument *doc = pg->pdf;
    for (guint i = 0; i < marks->len; i++) {
        struct mark *m = g_ptr_array_index(marks, i);
        if (m->page != pg->id)
            continue;
        PopplerAnnot *a = NULL;
        if (m->kind == MK_HIGHLIGHT || m->kind == MK_UNDERLINE || m->kind == MK_STRIKE) {
            GArray *quads = g_array_new(FALSE, FALSE, sizeof(PopplerQuadrilateral));
            double *q = (double *)m->quads->data;
            double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
            for (guint j = 0; j < m->quads->len / 4; j++) {
                PopplerRectangle r = prect(pg, q[4 * j], q[4 * j + 1], q[4 * j + 2], q[4 * j + 3]);
                PopplerQuadrilateral pq = {{r.x1, r.y2}, {r.x2, r.y2}, {r.x1, r.y1}, {r.x2, r.y1}};
                g_array_append_val(quads, pq);
                x1 = MIN(x1, r.x1), y1 = MIN(y1, r.y1), x2 = MAX(x2, r.x2), y2 = MAX(y2, r.y2);
            }
            PopplerRectangle box = {x1, y1, x2, y2};
            a = m->kind == MK_HIGHLIGHT ? poppler_annot_text_markup_new_highlight(doc, &box, quads)
                : m->kind == MK_UNDERLINE ? poppler_annot_text_markup_new_underline(doc, &box, quads)
                                          : poppler_annot_text_markup_new_strikeout(doc, &box, quads);
            g_array_free(quads, TRUE);
            GdkRGBA c = m->kind == MK_HIGHLIGHT ? m->fill : m->stroke;
            c.alpha = 1;
            PopplerColor pc = pcolor(&c);
            poppler_annot_set_color(a, &pc);
            if (m->text)
                poppler_annot_set_contents(a, m->text);
        } else if (m->kind == MK_NOTE) {
            PopplerRectangle r = prect(pg, m->x, m->y, m->w, m->h);
            a = poppler_annot_text_new(doc, &r);
            PopplerColor pc = {65535, 56000, 17000};
            poppler_annot_set_color(a, &pc);
            poppler_annot_set_contents(a, m->text ? m->text : "");
        } else {
            double mg = margin(m);
            double bx = m->x - mg, by = m->y - mg, bw = m->w + 2 * mg, bh = m->h + 2 * mg;
            int scale = 3;
            cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(bw * scale),
                                                            (int)ceil(bh * scale));
            cairo_t *cr = cairo_create(s);
            cairo_scale(cr, scale, scale);
            cairo_translate(cr, -bx, -by);
            mark_draw(cr, m, pg, NULL);
            cairo_destroy(cr);
            PopplerRectangle r = prect(pg, bx, by, bw, bh);
            a = poppler_annot_stamp_new(doc, &r);
            poppler_annot_stamp_set_custom_image(POPPLER_ANNOT_STAMP(a), s, NULL);
            cairo_surface_destroy(s);
            if (m->text)
                poppler_annot_set_contents(a, m->text);
        }
        poppler_annot_set_flags(a, POPPLER_ANNOT_FLAG_PRINT);
        poppler_page_add_annot(pp, a);
        g_object_unref(a);
    }
}

GdkPixbuf *marks_flatten(GPtrArray *marks, const struct page *pg)
{
    int w = gdk_pixbuf_get_width(pg->pix), h = gdk_pixbuf_get_height(pg->pix);
    gboolean any = FALSE;
    for (guint i = 0; i < marks->len; i++)
        any |= ((struct mark *)g_ptr_array_index(marks, i))->page == pg->id;
    if (!any)
        return g_object_ref(pg->pix);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cr, pg->pix, 0, 0);
    cairo_paint(cr);
    for (guint i = 0; i < marks->len; i++) {
        struct mark *m = g_ptr_array_index(marks, i);
        if (m->page == pg->id)
            mark_draw(cr, m, pg, NULL);
    }
    cairo_destroy(cr);
    GdkPixbuf *out = gdk_pixbuf_get_from_surface(s, 0, 0, w, h);
    cairo_surface_destroy(s);
    if (!gdk_pixbuf_get_has_alpha(pg->pix)) {   /* don't add an alpha channel nobody had */
        GdkPixbuf *o = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
        gdk_pixbuf_composite_color(out, o, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_NEAREST, 255, 0, 0,
                                   1, 0xffffff, 0xffffff);
        g_object_unref(out);
        out = o;
    }
    return out;
}
