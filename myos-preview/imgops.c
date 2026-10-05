/*
 * imgops.c — what Preview's Tools menu does to a picture's pixels. Every
 * function returns a new pixbuf (the old one stays as it was, for Undo).
 */
#include <math.h>
#include <string.h>

#include "preview.h"

GdkPixbuf *img_rotate(GdkPixbuf *p, int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    /* GTK counts its turns anticlockwise. */
    GdkPixbufRotation r = degrees == 90 ? GDK_PIXBUF_ROTATE_CLOCKWISE
                          : degrees == 180 ? GDK_PIXBUF_ROTATE_UPSIDEDOWN
                          : degrees == 270 ? GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE
                                           : GDK_PIXBUF_ROTATE_NONE;
    return gdk_pixbuf_rotate_simple(p, r);
}

GdkPixbuf *img_flip(GdkPixbuf *p, gboolean horizontal)
{
    return gdk_pixbuf_flip(p, horizontal);
}

/* Is (x, y) inside the polygon? (Even-odd: count the edges a ray crosses.) */
static gboolean in_polygon(GArray *pts, double x, double y)
{
    gboolean in = FALSE;
    guint n = pts->len / 2;
    double *v = (double *)pts->data;
    for (guint i = 0, j = n - 1; i < n; j = i++) {
        double xi = v[2 * i], yi = v[2 * i + 1], xj = v[2 * j], yj = v[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

/* Crop to the box (x, y, w, h). shape 1: an ellipse in it, 2: the lasso's
   polygon (image coordinates); outside the shape becomes transparent, as
   Preview does. */
GdkPixbuf *img_crop(GdkPixbuf *p, int x, int y, int w, int h, int shape, GArray *lasso)
{
    int pw = gdk_pixbuf_get_width(p), ph = gdk_pixbuf_get_height(p);
    x = CLAMP(x, 0, pw - 1);
    y = CLAMP(y, 0, ph - 1);
    w = CLAMP(w, 1, pw - x);
    h = CLAMP(h, 1, ph - y);
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(p, x, y, w, h);
    GdkPixbuf *out = shape ? gdk_pixbuf_add_alpha(sub, FALSE, 0, 0, 0) : gdk_pixbuf_copy(sub);
    g_object_unref(sub);
    if (!shape)
        return out;
    guchar *d = gdk_pixbuf_get_pixels(out);
    int rs = gdk_pixbuf_get_rowstride(out);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            gboolean in;
            if (shape == 1) {
                double dx = (i + .5) / w * 2 - 1, dy = (j + .5) / h * 2 - 1;
                in = dx * dx + dy * dy <= 1;
            } else {
                in = lasso && lasso->len >= 6 && in_polygon(lasso, x + i + .5, y + j + .5);
            }
            if (!in)
                d[j * rs + i * 4 + 3] = 0;
        }
    return out;
}

GdkPixbuf *img_resize(GdkPixbuf *p, int w, int h)
{
    return gdk_pixbuf_scale_simple(p, MAX(w, 1), MAX(h, 1), GDK_INTERP_HYPER);
}

/* --- colour ------------------------------------------------------------------------- */

static double smooth(double e0, double e1, double v)
{
    double t = CLAMP((v - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

/* The tone curve, as a table: levels, exposure, contrast, highlights,
   shadows. Applied to each channel. */
static void tone_table(const struct color_adjust *a, guchar lut[256])
{
    double lo = CLAMP(a->black, 0, .99), hi = CLAMP(a->white, lo + .01, 1);
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        v = (v - lo) / (hi - lo);
        v *= pow(2, a->exposure * 2);                           /* up to two stops */
        v = (v - .5) * (1 + a->contrast) + .5;
        v += a->highlights * .35 * smooth(.45, 1, v) * (1.2 - v);
        v += a->shadows * .35 * (1 - smooth(0, .55, v)) * (v + .2);
        lut[i] = (guchar)(CLAMP(v, 0, 1) * 255 + .5);
    }
}

GdkPixbuf *img_adjust(GdkPixbuf *src, const struct color_adjust *a)
{
    GdkPixbuf *p = gdk_pixbuf_copy(src);
    int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    int n = gdk_pixbuf_get_n_channels(p), rs = gdk_pixbuf_get_rowstride(p);
    guchar *d = gdk_pixbuf_get_pixels(p);
    guchar lut[256];
    tone_table(a, lut);
    double sat = a->saturation, tr = a->temperature * 30, tg = -a->tint * 30, sep = a->sepia;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            guchar *o = d + y * rs + x * n;
            double r = lut[o[0]], g = lut[o[1]], b = lut[o[2]];
            double l = .299 * r + .587 * g + .114 * b;
            r = l + (r - l) * sat;
            g = l + (g - l) * sat;
            b = l + (b - l) * sat;
            r += tr;
            b -= tr;
            g += tg;
            if (sep > 0) {                      /* the classic sepia matrix */
                double sr = .393 * r + .769 * g + .189 * b, sg = .349 * r + .686 * g + .168 * b,
                       sb = .272 * r + .534 * g + .131 * b;
                r += (sr - r) * sep;
                g += (sg - g) * sep;
                b += (sb - b) * sep;
            }
            o[0] = CLAMP(r, 0, 255);
            o[1] = CLAMP(g, 0, 255);
            o[2] = CLAMP(b, 0, 255);
        }
    if (fabs(a->sharpness) > .01) {
        /* Unsharp mask: each pixel moves away from (or, below 0, towards)
           the average of its 3x3 neighbourhood. */
        GdkPixbuf *orig = gdk_pixbuf_copy(p);
        const guchar *s = gdk_pixbuf_read_pixels(orig);
        double amt = a->sharpness * 1.5;
        for (int y = 1; y < h - 1; y++)
            for (int x = 1; x < w - 1; x++)
                for (int c = 0; c < 3; c++) {
                    int sum = 0;
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++)
                            sum += s[(y + j) * rs + (x + i) * n + c];
                    double v = s[y * rs + x * n + c], avg = sum / 9.0;
                    d[y * rs + x * n + c] = CLAMP(v + (v - avg) * amt, 0, 255);
                }
        g_object_unref(orig);
    }
    return p;
}

/* Auto Levels: black and white points at the darkest and brightest 0.5%
   of the picture. */
void img_auto_levels(GdkPixbuf *p, struct color_adjust *a)
{
    int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    int n = gdk_pixbuf_get_n_channels(p), rs = gdk_pixbuf_get_rowstride(p);
    const guchar *d = gdk_pixbuf_read_pixels(p);
    long hist[256] = {0}, total = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            const guchar *o = d + y * rs + x * n;
            hist[(o[0] * 77 + o[1] * 150 + o[2] * 29) >> 8]++;
            total++;
        }
    long cut = total / 200, acc = 0;
    int lo = 0, hi = 255;
    while (lo < 254 && (acc += hist[lo]) < cut)
        lo++;
    acc = 0;
    while (hi > lo + 1 && (acc += hist[hi]) < cut)
        hi--;
    a->black = lo / 255.0;
    a->white = hi / 255.0;
}

/* --- Instant Alpha --------------------------------------------------------------------- */

/* Everything joined to (x, y) whose colour is within `tolerance` (0..1) of
   it becomes transparent: a flood fill, one row span at a time. */
GdkPixbuf *img_instant_alpha(GdkPixbuf *src, int x, int y, double tolerance)
{
    GdkPixbuf *p = gdk_pixbuf_get_has_alpha(src) ? gdk_pixbuf_copy(src)
                                                 : gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0);
    int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    if (x < 0 || y < 0 || x >= w || y >= h)
        return p;
    int rs = gdk_pixbuf_get_rowstride(p);
    guchar *d = gdk_pixbuf_get_pixels(p);
    guchar ref[3];
    memcpy(ref, d + y * rs + x * 4, 3);
    int lim = (int)(tolerance * tolerance * 3 * 255 * 255);
    guint8 *seen = g_malloc0((size_t)w * h);
#define CLOSE(px, py)                                                                     \
    ({                                                                                    \
        const guchar *c_ = d + (py) * rs + (px) * 4;                                      \
        int dr = c_[0] - ref[0], dg = c_[1] - ref[1], db = c_[2] - ref[2];                \
        !seen[(py) * w + (px)] && c_[3] && dr * dr + dg * dg + db * db <= lim;            \
    })
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(int) * 2);
    int start[2] = {x, y};
    g_array_append_val(stack, start);
    while (stack->len) {
        int *s = &g_array_index(stack, int, (stack->len - 1) * 2);
        int sx = s[0], sy = s[1];
        g_array_set_size(stack, stack->len - 1);
        if (!CLOSE(sx, sy))
            continue;
        int l = sx, r = sx;
        while (l > 0 && CLOSE(l - 1, sy))
            l--;
        while (r < w - 1 && CLOSE(r + 1, sy))
            r++;
        for (int i = l; i <= r; i++) {
            seen[sy * w + i] = 1;
            d[sy * rs + i * 4 + 3] = 0;
        }
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = sy + dy;
            if (ny < 0 || ny >= h)
                continue;
            for (int i = l; i <= r; i++)
                if (CLOSE(i, ny) && (i == l || !CLOSE(i - 1, ny))) {
                    int q[2] = {i, ny};
                    g_array_append_val(stack, q);
                }
        }
    }
#undef CLOSE
    g_array_free(stack, TRUE);
    g_free(seen);
    return p;
}
