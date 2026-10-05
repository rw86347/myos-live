/*
 * icons.c — the toolbar's symbols, drawn the way Apple's SF Symbols look:
 * thin, round-ended strokes on a square of `size` pixels. Drawn rather than
 * loaded, so they're sharp at every desktop scale.
 */
#include <math.h>

#include "preview.h"

static void rr(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* A magnifying glass whose lens is centred at (cx, cy). */
static void glass(cairo_t *cr, double s, double cx, double cy, double r)
{
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + r * .72, cy + r * .72);
    cairo_line_to(cr, cx + r * 1.65, cy + r * 1.65);
    cairo_set_line_width(cr, s * .095);
    cairo_stroke(cr);
    cairo_set_line_width(cr, s * .07);
}

/* A pen nib pointing down-left, its tip at (x, y). */
static void pen(cairo_t *cr, double s, double x, double y)
{
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, -M_PI / 4);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, s * .10, -s * .16);
    cairo_line_to(cr, s * .10, -s * .62);
    cairo_line_to(cr, -s * .10, -s * .62);
    cairo_line_to(cr, -s * .10, -s * .16);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, -s * .10, -s * .48);
    cairo_line_to(cr, s * .10, -s * .48);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void symbol_draw(cairo_t *cr, enum symbol sy, double s, const GdkRGBA *c)
{
    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, c);
    cairo_set_line_width(cr, s * .07);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (sy) {
    case SY_SIDEBAR:                    /* sidebar.left */
        rr(cr, s * .10, s * .20, s * .80, s * .60, s * .10);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .38, s * .20);
        cairo_line_to(cr, s * .38, s * .80);
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, s * .18, s * (.34 + i * .11));
            cairo_line_to(cr, s * .29, s * (.34 + i * .11));
        }
        cairo_set_line_width(cr, s * .05);
        cairo_stroke(cr);
        break;
    case SY_INFO:                       /* info.circle */
        cairo_arc(cr, s * .5, s * .5, s * .38, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_arc(cr, s * .5, s * .32, s * .05, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_move_to(cr, s * .5, s * .45);
        cairo_line_to(cr, s * .5, s * .70);
        cairo_set_line_width(cr, s * .09);
        cairo_stroke(cr);
        break;
    case SY_ZOOM_OUT:
    case SY_ZOOM_IN:
        glass(cr, s, s * .42, s * .42, s * .27);
        cairo_move_to(cr, s * .30, s * .42);
        cairo_line_to(cr, s * .54, s * .42);
        if (sy == SY_ZOOM_IN) {
            cairo_move_to(cr, s * .42, s * .30);
            cairo_line_to(cr, s * .42, s * .54);
        }
        cairo_stroke(cr);
        break;
    case SY_SEARCH:
        glass(cr, s, s * .42, s * .42, s * .25);
        break;
    case SY_SHARE:                      /* square.and.arrow.up */
        cairo_move_to(cr, s * .34, s * .40);
        cairo_line_to(cr, s * .22, s * .40);
        cairo_line_to(cr, s * .22, s * .88);
        cairo_line_to(cr, s * .78, s * .88);
        cairo_line_to(cr, s * .78, s * .40);
        cairo_line_to(cr, s * .66, s * .40);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .5, s * .62);
        cairo_line_to(cr, s * .5, s * .10);
        cairo_move_to(cr, s * .36, s * .23);
        cairo_line_to(cr, s * .5, s * .09);
        cairo_line_to(cr, s * .64, s * .23);
        cairo_stroke(cr);
        break;
    case SY_HIGHLIGHT:                  /* highlighter */
        pen(cr, s, s * .24, s * .70);
        cairo_move_to(cr, s * .14, s * .86);
        cairo_line_to(cr, s * .86, s * .86);
        cairo_set_line_width(cr, s * .09);
        cairo_stroke(cr);
        break;
    case SY_ROTATE:                     /* rotate.left */
    case SY_ROTATE_RIGHT:
        if (sy == SY_ROTATE_RIGHT) {
            cairo_translate(cr, s, 0);
            cairo_scale(cr, -1, 1);
        }
        rr(cr, s * .38, s * .40, s * .44, s * .46, s * .06);
        cairo_stroke(cr);
        cairo_arc_negative(cr, s * .50, s * .38, s * .26, 0, -M_PI * .95);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .12, s * .26);
        cairo_line_to(cr, s * .24, s * .42);
        cairo_line_to(cr, s * .38, s * .30);
        cairo_stroke(cr);
        break;
    case SY_MARKUP:                     /* pencil.tip.crop.circle */
        cairo_arc(cr, s * .5, s * .5, s * .40, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_set_line_width(cr, s * .06);
        pen(cr, s * .78, s * .34, s * .67);
        break;
    case SY_TEXT_SEL:                   /* a text cursor beside "A" */
        cairo_move_to(cr, s * .14, s * .78);
        cairo_line_to(cr, s * .34, s * .22);
        cairo_line_to(cr, s * .54, s * .78);
        cairo_move_to(cr, s * .22, s * .58);
        cairo_line_to(cr, s * .46, s * .58);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .76, s * .16);
        cairo_line_to(cr, s * .76, s * .84);
        cairo_move_to(cr, s * .68, s * .16);
        cairo_line_to(cr, s * .84, s * .16);
        cairo_move_to(cr, s * .68, s * .84);
        cairo_line_to(cr, s * .84, s * .84);
        cairo_stroke(cr);
        break;
    case SY_RECT_SEL: {                 /* a dashed square */
        double d[] = {s * .09, s * .07};
        cairo_set_dash(cr, d, 2, 0);
        cairo_rectangle(cr, s * .16, s * .20, s * .68, s * .60);
        cairo_stroke(cr);
        break;
    }
    case SY_ALPHA:                      /* a wand with a star */
        cairo_move_to(cr, s * .16, s * .84);
        cairo_line_to(cr, s * .58, s * .42);
        cairo_set_line_width(cr, s * .09);
        cairo_stroke(cr);
        cairo_set_line_width(cr, s * .06);
        for (int i = 0; i < 4; i++) {
            double a = i * M_PI / 4;
            cairo_move_to(cr, s * .72 + cos(a) * s * .16, s * .28 + sin(a) * s * .16);
            cairo_line_to(cr, s * .72 - cos(a) * s * .16, s * .28 - sin(a) * s * .16);
        }
        cairo_stroke(cr);
        break;
    case SY_SKETCH:                     /* a squiggle */
        cairo_move_to(cr, s * .12, s * .66);
        cairo_curve_to(cr, s * .25, s * .20, s * .40, s * .20, s * .45, s * .52);
        cairo_curve_to(cr, s * .50, s * .82, s * .70, s * .82, s * .88, s * .34);
        cairo_stroke(cr);
        break;
    case SY_DRAW:                       /* a pen and its line */
        pen(cr, s, s * .22, s * .74);
        cairo_move_to(cr, s * .46, s * .88);
        cairo_curve_to(cr, s * .60, s * .80, s * .72, s * .92, s * .88, s * .84);
        cairo_stroke(cr);
        break;
    case SY_SHAPES:                     /* square.on.circle */
        cairo_arc(cr, s * .38, s * .38, s * .24, 0, 2 * M_PI);
        cairo_stroke(cr);
        rr(cr, s * .42, s * .42, s * .42, s * .42, s * .06);
        cairo_stroke(cr);
        break;
    case SY_TEXTBOX:                    /* "A" in a box */
        rr(cr, s * .12, s * .16, s * .76, s * .68, s * .08);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .32, s * .70);
        cairo_line_to(cr, s * .50, s * .28);
        cairo_line_to(cr, s * .68, s * .70);
        cairo_move_to(cr, s * .38, s * .56);
        cairo_line_to(cr, s * .62, s * .56);
        cairo_stroke(cr);
        break;
    case SY_SIGN:                       /* signature */
        cairo_move_to(cr, s * .10, s * .62);
        cairo_curve_to(cr, s * .22, s * .14, s * .34, s * .14, s * .30, s * .48);
        cairo_curve_to(cr, s * .28, s * .70, s * .48, s * .30, s * .52, s * .52);
        cairo_curve_to(cr, s * .56, s * .66, s * .70, s * .40, s * .90, s * .50);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .10, s * .84);
        cairo_line_to(cr, s * .90, s * .84);
        cairo_set_line_width(cr, s * .05);
        cairo_stroke(cr);
        break;
    case SY_NOTE:                       /* note.text */
        cairo_move_to(cr, s * .16, s * .16);
        cairo_line_to(cr, s * .84, s * .16);
        cairo_line_to(cr, s * .84, s * .62);
        cairo_line_to(cr, s * .62, s * .84);
        cairo_line_to(cr, s * .16, s * .84);
        cairo_close_path(cr);
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, s * .28, s * (.34 + i * .14));
            cairo_line_to(cr, s * (i == 2 ? .52 : .72), s * (.34 + i * .14));
        }
        cairo_set_line_width(cr, s * .05);
        cairo_stroke(cr);
        break;
    case SY_SHAPE_STYLE:                /* three lines of growing weight */
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, s * .16, s * (.26 + i * .24));
            cairo_line_to(cr, s * .84, s * (.26 + i * .24));
            cairo_set_line_width(cr, s * (.04 + i * .05));
            cairo_stroke(cr);
        }
        break;
    case SY_BORDER:                     /* square, outlined */
        rr(cr, s * .18, s * .18, s * .64, s * .64, s * .08);
        cairo_set_line_width(cr, s * .10);
        cairo_stroke(cr);
        break;
    case SY_FILL:                       /* square, filled */
        rr(cr, s * .18, s * .18, s * .64, s * .64, s * .08);
        cairo_fill(cr);
        break;
    case SY_FONT:                       /* "Aa" */
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, s * .62);
        cairo_move_to(cr, s * .06, s * .76);
        cairo_show_text(cr, "Aa");
        break;
    case SY_ADJUST_COLOR:               /* a prism: three overlapping drops */
        cairo_set_line_width(cr, s * .06);
        cairo_arc(cr, s * .38, s * .40, s * .22, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_arc(cr, s * .62, s * .40, s * .22, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_arc(cr, s * .50, s * .62, s * .22, 0, 2 * M_PI);
        cairo_stroke(cr);
        break;
    case SY_ADJUST_SIZE:                /* arrows out of a corner */
        rr(cr, s * .14, s * .14, s * .72, s * .72, s * .08);
        cairo_stroke(cr);
        cairo_move_to(cr, s * .34, s * .66);
        cairo_line_to(cr, s * .66, s * .34);
        cairo_move_to(cr, s * .46, s * .34);
        cairo_line_to(cr, s * .66, s * .34);
        cairo_line_to(cr, s * .66, s * .54);
        cairo_stroke(cr);
        break;
    case SY_CROP:
        cairo_move_to(cr, s * .28, s * .08);
        cairo_line_to(cr, s * .28, s * .72);
        cairo_line_to(cr, s * .92, s * .72);
        cairo_move_to(cr, s * .08, s * .28);
        cairo_line_to(cr, s * .72, s * .28);
        cairo_line_to(cr, s * .72, s * .92);
        cairo_stroke(cr);
        break;
    case SY_CHEVRON:                    /* chevron.down, small */
        cairo_move_to(cr, s * .30, s * .40);
        cairo_line_to(cr, s * .50, s * .60);
        cairo_line_to(cr, s * .70, s * .40);
        cairo_stroke(cr);
        break;
    case SY_LOCK:
        rr(cr, s * .22, s * .44, s * .56, s * .44, s * .08);
        cairo_fill(cr);
        cairo_arc(cr, s * .5, s * .40, s * .18, M_PI, 2 * M_PI);
        cairo_move_to(cr, s * .32, s * .40);
        cairo_line_to(cr, s * .32, s * .46);
        cairo_move_to(cr, s * .68, s * .40);
        cairo_line_to(cr, s * .68, s * .46);
        cairo_set_line_width(cr, s * .08);
        cairo_stroke(cr);
        break;
    case SY_FORM:                       /* rectangle.and.pencil */
        rr(cr, s * .10, s * .30, s * .62, s * .44, s * .06);
        cairo_stroke(cr);
        pen(cr, s * .7, s * .58, s * .64);
        break;
    case SY_DOC:                        /* doc */
        cairo_move_to(cr, s * .22, s * .10);
        cairo_line_to(cr, s * .60, s * .10);
        cairo_line_to(cr, s * .80, s * .30);
        cairo_line_to(cr, s * .80, s * .90);
        cairo_line_to(cr, s * .22, s * .90);
        cairo_close_path(cr);
        cairo_stroke(cr);
        break;
    case SY_BOOKMARK:
        cairo_move_to(cr, s * .28, s * .12);
        cairo_line_to(cr, s * .72, s * .12);
        cairo_line_to(cr, s * .72, s * .88);
        cairo_line_to(cr, s * .50, s * .70);
        cairo_line_to(cr, s * .28, s * .88);
        cairo_close_path(cr);
        cairo_stroke(cr);
        break;
    }
    cairo_restore(cr);
}

/* The symbol in the widget's own text colour (so :disabled greys it, and a
   selected toggle can go white). */
static gboolean draw_symbol(GtkWidget *w, cairo_t *cr, gpointer data)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(gtk_widget_get_parent(w));
    GdkRGBA c;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
    double h = gtk_widget_get_allocated_height(w), wd = gtk_widget_get_allocated_width(w);
    double s = MIN(h, wd);
    cairo_translate(cr, (wd - s) / 2, (h - s) / 2);
    symbol_draw(cr, GPOINTER_TO_INT(data), s, &c);
    return TRUE;
}

GtkWidget *symbol_new(enum symbol s, int pts)
{
    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_size_request(w, S(pts), S(pts));
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
    g_signal_connect(w, "draw", G_CALLBACK(draw_symbol), GINT_TO_POINTER(s));
    return w;
}

/* Preview's icon: a photo on a contact sheet, under a loupe. */
void app_icon_draw(cairo_t *cr, double s)
{
    cairo_save(cr);
    /* the sheet */
    cairo_save(cr);
    cairo_translate(cr, s * .5, s * .5);
    cairo_rotate(cr, -0.08);
    rr(cr, -s * .36, -s * .42, s * .72, s * .84, s * .03);
    cairo_set_source_rgb(cr, .97, .97, .96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, .25);
    cairo_set_line_width(cr, s * .01);
    cairo_stroke(cr);
    /* the photo: sky, sun, hills */
    cairo_rectangle(cr, -s * .30, -s * .36, s * .60, s * .46);
    cairo_pattern_t *sky = cairo_pattern_create_linear(0, -s * .36, 0, s * .10);
    cairo_pattern_add_color_stop_rgb(sky, 0, .38, .64, .93);
    cairo_pattern_add_color_stop_rgb(sky, 1, .78, .89, .98);
    cairo_set_source(cr, sky);
    cairo_fill(cr);
    cairo_pattern_destroy(sky);
    cairo_arc(cr, s * .14, -s * .20, s * .07, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, .82, .30);
    cairo_fill(cr);
    cairo_move_to(cr, -s * .30, s * .10);
    cairo_curve_to(cr, -s * .16, -s * .12, -s * .04, -s * .10, s * .06, s * .02);
    cairo_curve_to(cr, s * .14, -s * .06, s * .24, -s * .06, s * .30, s * .00);
    cairo_line_to(cr, s * .30, s * .10);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, .30, .62, .32);
    cairo_fill(cr);
    for (int i = 0; i < 3; i++) {       /* text lines under it */
        cairo_rectangle(cr, -s * .30, s * (.17 + i * .07), s * (i == 2 ? .36 : .60), s * .025);
        cairo_set_source_rgb(cr, .75, .75, .75);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    /* the loupe */
    cairo_arc(cr, s * .66, s * .64, s * .17, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, .85, .93, 1, .55);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, .25, .25, .28);
    cairo_set_line_width(cr, s * .045);
    cairo_stroke(cr);
    cairo_move_to(cr, s * .78, s * .76);
    cairo_line_to(cr, s * .92, s * .90);
    cairo_set_line_width(cr, s * .08);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    cairo_restore(cr);
}
