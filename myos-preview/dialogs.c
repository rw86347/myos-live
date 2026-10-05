/*
 * dialogs.c — Preview's dialogs, each laid out like the Mac's:
 * Export, Print, Adjust Size, Adjust Color, the Inspector, Settings, Go to
 * Page, the password prompt, Rename, Move To, Signatures, About, Help,
 * Customize Toolbar, Take Screenshot, the save-on-close question, and the
 * colour popover of the markup bar. Also where signatures and bookmarks
 * are kept.
 */
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <X11/Xlib.h>

#include "preview.h"

static GtkWidget *grid_new(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), S(8));
    gtk_grid_set_column_spacing(GTK_GRID(g), S(10));
    gtk_container_set_border_width(GTK_CONTAINER(g), S(14));
    return g;
}

static GtkWidget *label_right(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    return l;
}

static GtkWidget *label_left(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *dialog(GtkWindow *parent, const char *title, const char *ok)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "Cancel", GTK_RESPONSE_CANCEL, ok, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    return d;
}

static GtkWindow *parent_of(struct win *w)
{
    return GTK_WINDOW(w->window);
}

static char *stem_of(const char *name)
{
    char *s = g_strdup(name ? name : "Untitled");
    char *dot = strrchr(s, '.');
    if (dot && dot != s)
        *dot = 0;
    return s;
}

static char *human_size(goffset n)
{
    return g_format_size(n);
}

/* --- Export ---------------------------------------------------------------------- */

struct export_ui {
    struct win *w;
    GtkWidget *chooser, *format, *quality, *quality_row, *lossless, *alpha, *dpi, *dpi_row;
    GtkWidget *tiff, *tiff_row, *encrypt, *pw1, *pw2, *pw_row, *pw2_row, *size;
    int fmts[EX_COUNT], nfmts;
};

static enum export_format fmt_of(struct export_ui *u)
{
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(u->format));
    return u->fmts[CLAMP(i, 0, u->nfmts - 1)];
}

static void export_update(struct export_ui *u)
{
    enum export_format f = fmt_of(u);
    gboolean q = f == EX_JPEG || f == EX_HEIC || f == EX_JP2 || f == EX_WEBP;
    gboolean a = f == EX_PNG || f == EX_TIFF || f == EX_HEIC || f == EX_JP2 || f == EX_EXR || f == EX_TGA ||
                 f == EX_GIF || f == EX_ICO || f == EX_WEBP;
    gtk_widget_set_visible(u->quality_row, q);
    gtk_widget_set_visible(u->lossless, f == EX_JP2);
    gtk_widget_set_sensitive(u->quality, !(f == EX_JP2 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->lossless))));
    gtk_widget_set_visible(u->alpha, a);
    gtk_widget_set_visible(u->dpi_row, f != EX_PDF && u->w->doc->kind == DOC_PDF);
    gtk_widget_set_visible(u->tiff_row, f == EX_TIFF);
    gtk_widget_set_visible(u->encrypt, f == EX_PDF);
    gboolean enc = f == EX_PDF && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->encrypt));
    gtk_widget_set_visible(u->pw_row, enc);
    gtk_widget_set_visible(u->pw2_row, enc);
    /* Swap the name's extension for the format's. */
    char *name = gtk_file_chooser_get_current_name(GTK_FILE_CHOOSER(u->chooser));
    if (name) {
        char *stem = stem_of(name);
        char *n = g_strdup_printf("%s.%s", stem, export_exts[f]);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(u->chooser), n);
        g_free(n), g_free(stem), g_free(name);
    }
    /* What the file will roughly weigh: the pixels, then what the format
       usually makes of them. */
    if (f != EX_PDF) {
        struct page *p = PAGE(u->w->doc, u->w->page);
        double pw, ph;
        page_size(p, &pw, &ph);
        double s = p->pdf ? gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->dpi)) / 72 : 1;
        double px = pw * s * ph * s, qq = gtk_range_get_value(GTK_RANGE(u->quality));
        double bpp = f == EX_JPEG ? .3 + 3 * qq * qq : f == EX_HEIC ? .15 + 1.5 * qq * qq
                   : f == EX_WEBP ? .25 + 2 * qq * qq : f == EX_JP2 ? .3 + 4 * qq : f == EX_EXR ? 6
                   : f == EX_GIF ? 1 : f == EX_PNG ? 2.5 : 3 + (a ? 1 : 0);
        char *sz = human_size((goffset)(px * bpp));
        char *t = g_strdup_printf("about %s", sz);
        gtk_label_set_text(GTK_LABEL(u->size), t);
        g_free(t), g_free(sz);
    } else {
        gtk_label_set_text(GTK_LABEL(u->size), "—");
    }
}

static void export_changed(GtkWidget *wd, gpointer data)
{
    (void)wd;
    export_update(data);
}

gboolean dlg_export(struct win *w, gboolean pdf_only)
{
    struct doc *d = w->doc;
    struct export_ui u = {.w = w};
    GtkWidget *c = gtk_file_chooser_dialog_new(pdf_only ? "Export As PDF" : "Export", parent_of(w),
                                               GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel", GTK_RESPONSE_CANCEL,
                                               "Save", GTK_RESPONSE_OK, NULL);
    u.chooser = c;
    gtk_dialog_set_default_response(GTK_DIALOG(c), GTK_RESPONSE_OK);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(c), TRUE);
    if (d->path) {
        char *dir = g_path_get_dirname(d->path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(c), dir);
        g_free(dir);
    }
    char *stem = stem_of(d->name);
    char *name = g_strdup_printf("%s.pdf", stem);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(c), name);
    g_free(name), g_free(stem);

    GtkWidget *g = grid_new();
    int row = 0;
    u.format = gtk_combo_box_text_new();
    /* The Mac lists the common formats; holding Option (Alt here) while
       choosing Export shows every one. */
    GdkModifierType mods = 0;
    gtk_get_current_event_state(&mods);
    gboolean all = (mods & GDK_MOD1_MASK) != 0;
    static const int common[] = {EX_HEIC, EX_JPEG, EX_JP2, EX_EXR, EX_PDF, EX_PNG, EX_TIFF};
    static const int every[] = {EX_GIF, EX_HEIC, EX_JPEG, EX_JP2, EX_BMP, EX_ICO, EX_EXR,
                                EX_PDF, EX_PNG, EX_TGA, EX_TIFF, EX_WEBP};
    const int *list = all ? every : common;
    int n = all ? G_N_ELEMENTS(every) : G_N_ELEMENTS(common);
    int pick = 0;
    for (int i = 0; i < n; i++) {
        if (pdf_only && list[i] != EX_PDF)
            continue;
        u.fmts[u.nfmts] = list[i];
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u.format), export_names[list[i]]);
        if (list[i] == EX_PDF || (!pdf_only && d->kind == DOC_IMAGE && list[i] == EX_PNG))
            pick = u.nfmts;
        u.nfmts++;
    }
    if (!pdf_only && d->kind == DOC_IMAGE)
        for (int i = 0; i < u.nfmts; i++)
            if (u.fmts[i] == EX_PNG)
                pick = i;
    gtk_combo_box_set_active(GTK_COMBO_BOX(u.format), pick);
    gtk_grid_attach(GTK_GRID(g), label_right("Format:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.format, 1, row++, 2, 1);
    gtk_widget_set_sensitive(u.format, !pdf_only);

    u.quality_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
    gtk_box_pack_start(GTK_BOX(u.quality_row), gtk_label_new("Least"), FALSE, FALSE, 0);
    u.quality = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, .01);
    gtk_scale_set_draw_value(GTK_SCALE(u.quality), FALSE);
    gtk_range_set_value(GTK_RANGE(u.quality), .85);
    gtk_widget_set_size_request(u.quality, S(200), -1);
    gtk_box_pack_start(GTK_BOX(u.quality_row), u.quality, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(u.quality_row), gtk_label_new("Best"), FALSE, FALSE, 0);
    u.lossless = gtk_check_button_new_with_label("Lossless");
    gtk_box_pack_start(GTK_BOX(u.quality_row), u.lossless, FALSE, FALSE, 0);
    GtkWidget *ql = label_right("Quality:");
    gtk_grid_attach(GTK_GRID(g), ql, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.quality_row, 1, row++, 2, 1);
    g_object_bind_property(u.quality_row, "visible", ql, "visible", G_BINDING_SYNC_CREATE);

    u.alpha = gtk_check_button_new_with_label("Alpha");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.alpha), TRUE);
    gtk_grid_attach(GTK_GRID(g), u.alpha, 1, row++, 2, 1);

    u.dpi_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
    u.dpi = gtk_spin_button_new_with_range(18, 1200, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(u.dpi), 144);
    gtk_box_pack_start(GTK_BOX(u.dpi_row), u.dpi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(u.dpi_row), gtk_label_new("pixels/inch"), FALSE, FALSE, 0);
    GtkWidget *rl = label_right("Resolution:");
    gtk_grid_attach(GTK_GRID(g), rl, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.dpi_row, 1, row++, 2, 1);
    g_object_bind_property(u.dpi_row, "visible", rl, "visible", G_BINDING_SYNC_CREATE);

    u.tiff_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    u.tiff = gtk_combo_box_text_new();
    for (const char *const *t = (const char *const[]){"None", "LZW", "Packbits", "ZIP", NULL}; *t; t++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u.tiff), *t);
    gtk_combo_box_set_active(GTK_COMBO_BOX(u.tiff), 1);
    gtk_box_pack_start(GTK_BOX(u.tiff_row), u.tiff, FALSE, FALSE, 0);
    GtkWidget *tl = label_right("Compression:");
    gtk_grid_attach(GTK_GRID(g), tl, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.tiff_row, 1, row++, 2, 1);
    g_object_bind_property(u.tiff_row, "visible", tl, "visible", G_BINDING_SYNC_CREATE);

    u.encrypt = gtk_check_button_new_with_label("Encrypt");
    gtk_grid_attach(GTK_GRID(g), u.encrypt, 1, row++, 2, 1);
    u.pw1 = gtk_entry_new();
    u.pw2 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(u.pw1), FALSE);
    gtk_entry_set_visibility(GTK_ENTRY(u.pw2), FALSE);
    u.pw_row = u.pw1, u.pw2_row = u.pw2;
    GtkWidget *l1 = label_right("Password:"), *l2 = label_right("Verify:");
    gtk_grid_attach(GTK_GRID(g), l1, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.pw1, 1, row++, 2, 1);
    gtk_grid_attach(GTK_GRID(g), l2, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.pw2, 1, row++, 2, 1);
    g_object_bind_property(u.pw1, "visible", l1, "visible", G_BINDING_SYNC_CREATE);
    g_object_bind_property(u.pw2, "visible", l2, "visible", G_BINDING_SYNC_CREATE);

    u.size = label_left("");
    gtk_grid_attach(GTK_GRID(g), label_right("File Size:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.size, 1, row++, 2, 1);

    gtk_widget_show_all(g);
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(c), g);
    for (GtkWidget **wd = (GtkWidget *[]){u.format, u.lossless, u.encrypt, u.tiff, NULL}; *wd; wd++)
        g_signal_connect(*wd, GTK_IS_COMBO_BOX(*wd) ? "changed" : "toggled", G_CALLBACK(export_changed), &u);
    g_signal_connect(u.quality, "value-changed", G_CALLBACK(export_changed), &u);
    g_signal_connect(u.dpi, "value-changed", G_CALLBACK(export_changed), &u);
    export_update(&u);

    gboolean done = FALSE;
    while (!done && gtk_dialog_run(GTK_DIALOG(c)) == GTK_RESPONSE_OK) {
        enum export_format f = fmt_of(&u);
        struct export_opts o = {
            .fmt = f,
            .quality = f == EX_JP2 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u.lossless))
                           ? 1 : gtk_range_get_value(GTK_RANGE(u.quality)),
            .alpha = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u.alpha)),
            .dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u.dpi)),
            .tiff_compression = gtk_combo_box_get_active(GTK_COMBO_BOX(u.tiff)),
        };
        if (f == EX_PDF && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u.encrypt))) {
            const char *a = gtk_entry_get_text(GTK_ENTRY(u.pw1)), *b = gtk_entry_get_text(GTK_ENTRY(u.pw2));
            if (!*a || strcmp(a, b)) {
                GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(c), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                                      GTK_BUTTONS_OK, "The passwords don't match.");
                gtk_dialog_run(GTK_DIALOG(m));
                gtk_widget_destroy(m);
                continue;
            }
            o.password = (char *)a;
        }
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(c));
        GError *err = NULL;
        if (doc_export(d, w->page, path, &o, &err)) {
            done = TRUE;
            gtk_recent_manager_add_item(gtk_recent_manager_get_default(),
                                        g_filename_to_uri(path, NULL, NULL));
            /* An untitled document (a screenshot, the clipboard) is that
               file from now on. */
            if (!d->path) {
                struct doc *nd = doc_open(path, o.password, NULL);
                if (nd)
                    win_set_doc(w, nd);
            }
        } else {
            win_error(w, "The document could not be exported.", err);
            g_clear_error(&err);
        }
        g_free(path);
    }
    gtk_widget_destroy(c);
    return done;
}

/* --- Print ------------------------------------------------------------------------ */

static void print_page(GtkPrintOperation *op, GtkPrintContext *pc, int n, gpointer data)
{
    (void)op;
    struct win *w = data;
    struct page *p = PAGE(w->doc, n);
    double pw, ph, cw = gtk_print_context_get_width(pc), ch = gtk_print_context_get_height(pc);
    page_size(p, &pw, &ph);
    /* Scale to fit, centred, as Preview's "Scale to Fit" does. */
    double s = MIN(cw / pw, ch / ph);
    if (p->pdf)
        s = MIN(s, 72 * gtk_print_context_get_dpi_x(pc) / 72 / 72);
    cairo_t *cr = gtk_print_context_get_cairo_context(pc);
    cairo_translate(cr, (cw - pw * s) / 2, (ch - ph * s) / 2);
    cairo_scale(cr, s, s);
    page_render(cr, p, w->doc->st->marks);
}

void dlg_print(struct win *w)
{
    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_n_pages(op, NPAGES(w->doc));
    gtk_print_operation_set_current_page(op, w->page);
    gtk_print_operation_set_job_name(op, w->doc->name);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    g_signal_connect(op, "draw-page", G_CALLBACK(print_page), w);
    GError *err = NULL;
    if (gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent_of(w), &err) ==
        GTK_PRINT_OPERATION_RESULT_ERROR) {
        win_error(w, "The document could not be printed.", err);
        g_clear_error(&err);
    }
    g_object_unref(op);
}

/* --- Adjust Size ------------------------------------------------------------------- */

struct size_ui {
    struct win *w;
    GtkWidget *fit, *width, *height, *units, *dpi, *keep, *resample, *now, *after;
    double pw, ph, dpi0;        /* the picture as it is */
    gboolean busy;
};

static const char *const unit_names[] = {"pixels", "percent", "inches", "cm", "mm", "points"};

/* Pixels per unit. */
static double unit_px(struct size_ui *u, int unit, double base, double dpi)
{
    switch (unit) {
    case 1: return base / 100;
    case 2: return dpi;
    case 3: return dpi / 2.54;
    case 4: return dpi / 25.4;
    case 5: return dpi / 72;
    default: (void)u; return 1;
    }
}

static void size_result(struct size_ui *u)
{
    int unit = gtk_combo_box_get_active(GTK_COMBO_BOX(u->units));
    double dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->dpi));
    double w = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->width)) * unit_px(u, unit, u->pw, dpi);
    double h = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->height)) * unit_px(u, unit, u->ph, dpi);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->resample)))
        w = u->pw, h = u->ph;
    char *a = human_size((goffset)(u->pw * u->ph * 4)), *b = human_size((goffset)(w * h * 4));
    char *t1 = g_strdup_printf("Current Size: %s", a), *t2 = g_strdup_printf("Resulting Size: %s", b);
    gtk_label_set_text(GTK_LABEL(u->now), t1);
    gtk_label_set_text(GTK_LABEL(u->after), t2);
    g_free(a), g_free(b), g_free(t1), g_free(t2);
}

static void size_show(struct size_ui *u, double wpx, double hpx, double dpi)
{
    int unit = gtk_combo_box_get_active(GTK_COMBO_BOX(u->units));
    u->busy = TRUE;
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(u->width), unit == 0 ? 0 : 2);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(u->height), unit == 0 ? 0 : 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(u->width), wpx / unit_px(u, unit, u->pw, dpi));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(u->height), hpx / unit_px(u, unit, u->ph, dpi));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(u->dpi), dpi);
    u->busy = FALSE;
    size_result(u);
}

static void size_edited(GtkWidget *wd, gpointer data)
{
    struct size_ui *u = data;
    if (u->busy)
        return;
    int unit = gtk_combo_box_get_active(GTK_COMBO_BOX(u->units));
    double dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->dpi));
    double w = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->width)) * unit_px(u, unit, u->pw, dpi);
    double h = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->height)) * unit_px(u, unit, u->ph, dpi);
    gboolean resample = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->resample));
    if (!resample && (wd == u->width || wd == u->height) && unit >= 2) {
        /* Not resampling: the pixels stay, so a new print size is a new
           resolution. */
        double inches = gtk_spin_button_get_value(GTK_SPIN_BUTTON(wd)) * unit_px(u, unit, 0, 1);
        dpi = (wd == u->width ? u->pw : u->ph) / MAX(inches, 1e-3);
        size_show(u, u->pw, u->ph, dpi);
        return;
    }
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->keep))) {
        if (wd == u->width)
            h = w * u->ph / u->pw;
        else if (wd == u->height)
            w = h * u->pw / u->ph;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->fit), 0);
    size_show(u, w, h, dpi);
}

static void size_units(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct size_ui *u = data;
    /* Show the same size in the new unit: remember it in pixels first. */
    static int last;
    double dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->dpi));
    double w = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->width)) * unit_px(u, last, u->pw, dpi);
    double h = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->height)) * unit_px(u, last, u->ph, dpi);
    last = gtk_combo_box_get_active(GTK_COMBO_BOX(u->units));
    size_show(u, w, h, dpi);
}

static void size_fit(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct size_ui *u = data;
    static const int fits[][2] = {{0, 0}, {320, 240}, {640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1920, 1080}};
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(u->fit));
    if (i <= 0 || u->busy)
        return;
    double s = MIN(fits[i][0] / u->pw, fits[i][1] / u->ph);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u->resample), TRUE);
    size_show(u, round(u->pw * s), round(u->ph * s), gtk_spin_button_get_value(GTK_SPIN_BUTTON(u->dpi)));
}

void dlg_adjust_size(struct win *w)
{
    struct doc *d = w->doc;
    if (d->kind != DOC_IMAGE)
        return;
    struct page *p = PAGE(d, w->page);
    struct size_ui u = {.w = w, .pw = p->w, .ph = p->h, .dpi0 = d->dpi};
    GtkWidget *dl = dialog(parent_of(w), "Image Dimensions", "OK");
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    int row = 0;
    u.fit = gtk_combo_box_text_new();
    for (const char *const *t = (const char *const[]){"Custom", "320 × 240", "640 × 480", "800 × 600", "1024 × 768",
                                                     "1280 × 1024", "1920 × 1080", NULL}; *t; t++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u.fit), *t);
    gtk_combo_box_set_active(GTK_COMBO_BOX(u.fit), 0);
    gtk_grid_attach(GTK_GRID(g), label_right("Fit into:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.fit, 1, row++, 2, 1);
    u.width = gtk_spin_button_new_with_range(.01, 100000, 1);
    u.height = gtk_spin_button_new_with_range(.01, 100000, 1);
    u.units = gtk_combo_box_text_new();
    for (int i = 0; i < 6; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u.units), unit_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(u.units), 0);
    gtk_grid_attach(GTK_GRID(g), label_right("Width:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.width, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.units, 2, row++, 1, 2);
    gtk_grid_attach(GTK_GRID(g), label_right("Height:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.height, 1, row++, 1, 1);
    u.dpi = gtk_spin_button_new_with_range(1, 10000, 1);
    gtk_grid_attach(GTK_GRID(g), label_right("Resolution:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), u.dpi, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label_left("pixels/inch"), 2, row++, 1, 1);
    u.keep = gtk_check_button_new_with_label("Scale proportionally");
    u.resample = gtk_check_button_new_with_label("Resample image");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.keep), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.resample), TRUE);
    gtk_grid_attach(GTK_GRID(g), u.keep, 1, row++, 2, 1);
    gtk_grid_attach(GTK_GRID(g), u.resample, 1, row++, 2, 1);
    u.now = label_left(""), u.after = label_left("");
    gtk_grid_attach(GTK_GRID(g), u.now, 0, row++, 3, 1);
    gtk_grid_attach(GTK_GRID(g), u.after, 0, row++, 3, 1);
    size_show(&u, u.pw, u.ph, u.dpi0);
    g_signal_connect(u.width, "value-changed", G_CALLBACK(size_edited), &u);
    g_signal_connect(u.height, "value-changed", G_CALLBACK(size_edited), &u);
    g_signal_connect(u.dpi, "value-changed", G_CALLBACK(size_edited), &u);
    g_signal_connect(u.resample, "toggled", G_CALLBACK(size_edited), &u);
    g_signal_connect(u.units, "changed", G_CALLBACK(size_units), &u);
    g_signal_connect(u.fit, "changed", G_CALLBACK(size_fit), &u);
    gtk_widget_show_all(dl);
    if (gtk_dialog_run(GTK_DIALOG(dl)) == GTK_RESPONSE_OK) {
        int unit = gtk_combo_box_get_active(GTK_COMBO_BOX(u.units));
        double dpi = gtk_spin_button_get_value(GTK_SPIN_BUTTON(u.dpi));
        int nw = (int)round(gtk_spin_button_get_value(GTK_SPIN_BUTTON(u.width)) * unit_px(&u, unit, u.pw, dpi));
        int nh = (int)round(gtk_spin_button_get_value(GTK_SPIN_BUTTON(u.height)) * unit_px(&u, unit, u.ph, dpi));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u.resample)) && (nw != (int)u.pw || nh != (int)u.ph) &&
            nw > 0 && nh > 0) {
            GdkPixbuf *src = doc_page_pixels(d, w->page);
            GdkPixbuf *out = img_resize(src, nw, nh);
            doc_set_pixels(d, w->page, out);
            g_object_unref(src), g_object_unref(out);
        } else if (fabs(dpi - d->dpi) > .01) {
            d->edits++;         /* the resolution is the document's, not a page's: no undo */
        }
        d->dpi = dpi;
        win_doc_changed(w);
    }
    gtk_widget_destroy(dl);
}

/* --- Adjust Color ----------------------------------------------------------------- */

struct color_ui {
    struct win *w;
    int page;
    unsigned page_id;
    GdkPixbuf *base;            /* the picture before the dialog */
    guint hist[3][256];
    struct color_adjust a;
    GtkWidget *win, *sliders[11];
    gboolean committed;
    guint timer;
};

static const struct { const char *name; double lo, hi, def; } color_rows[11] = {
    {"Exposure:", -1, 1, 0},   {"Contrast:", -1, 1, 0},  {"Highlights:", -1, 1, 0}, {"Shadows:", -1, 1, 0},
    {"Saturation:", 0, 2, 1},  {"Temperature:", -1, 1, 0}, {"Tint:", -1, 1, 0},    {"Sepia:", 0, 1, 0},
    {"Sharpness:", -1, 1, 0},  {"Levels black:", 0, 1, 0}, {"Levels white:", 0, 1, 1},
};

static double *field(struct color_adjust *a, int i)
{
    double *f[11] = {&a->exposure, &a->contrast, &a->highlights, &a->shadows, &a->saturation, &a->temperature,
                     &a->tint, &a->sepia, &a->sharpness, &a->black, &a->white};
    return f[i];
}

static gboolean color_apply(gpointer data)
{
    struct color_ui *u = data;
    u->timer = 0;
    struct doc *d = u->w->doc;
    if (u->page >= NPAGES(d) || PAGE(d, u->page)->id != u->page_id)
        return G_SOURCE_REMOVE;         /* the page went (undo, delete) */
    GdkPixbuf *out = img_adjust(u->base, &u->a);
    if (!u->committed) {
        doc_set_pixels(d, u->page, out);    /* the first change: one undo step */
        u->committed = TRUE;
    } else {
        /* Later changes refine that step: the current state is ours alone
           (doc_set_pixels made it), so the page can be swapped in place. */
        struct page *p = page_copy(PAGE(d, u->page));
        g_object_unref(p->pix);
        p->pix = g_object_ref(out);
        g_ptr_array_remove_index(d->st->pages, u->page);
        g_ptr_array_insert(d->st->pages, u->page, p);
    }
    u->page_id = PAGE(d, u->page)->id;
    g_object_unref(out);
    win_doc_changed(u->w);
    return G_SOURCE_REMOVE;
}

static void color_changed(GtkRange *r, gpointer data)
{
    struct color_ui *u = data;
    for (int i = 0; i < 11; i++)
        if (GTK_WIDGET(r) == u->sliders[i])
            *field(&u->a, i) = gtk_range_get_value(r);
    /* Wait for the slider to rest: a big photo takes a while to redo. */
    if (u->timer)
        g_source_remove(u->timer);
    u->timer = g_timeout_add(120, color_apply, u);
}

static void color_set_all(struct color_ui *u)
{
    for (int i = 0; i < 11; i++)
        gtk_range_set_value(GTK_RANGE(u->sliders[i]), *field(&u->a, i));
}

static void color_auto(GtkButton *b, gpointer data)
{
    (void)b;
    struct color_ui *u = data;
    img_auto_levels(u->base, &u->a);
    color_set_all(u);
}

static void color_reset(GtkButton *b, gpointer data)
{
    (void)b;
    struct color_ui *u = data;
    for (int i = 0; i < 11; i++)
        *field(&u->a, i) = color_rows[i].def;
    color_set_all(u);
}

static gboolean hist_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    struct color_ui *u = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    cairo_set_source_rgb(cr, .12, .12, .12);
    cairo_paint(cr);
    guint max = 1;
    for (int c = 0; c < 3; c++)
        for (int i = 1; i < 255; i++)
            max = MAX(max, u->hist[c][i]);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    for (int c = 0; c < 3; c++) {
        cairo_move_to(cr, 0, h);
        for (int i = 0; i < 256; i++)
            cairo_line_to(cr, i * (w - 1) / 255.0, h - MIN(1.0, u->hist[c][i] / (double)max) * (h - 2));
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, c == 0, c == 1, c == 2, .7);
        cairo_fill(cr);
    }
    return TRUE;
}

static void color_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct color_ui *u = data;
    if (u->timer) {
        g_source_remove(u->timer);
        color_apply(u);
    }
    u->w->adjust_color = NULL;
    g_object_unref(u->base);
    g_free(u);
}

void dlg_adjust_color(struct win *w)
{
    if (w->adjust_color) {
        gtk_window_present(GTK_WINDOW(w->adjust_color));
        return;
    }
    struct doc *d = w->doc;
    if (d->kind != DOC_IMAGE)
        return;
    struct color_ui *u = g_new0(struct color_ui, 1);
    u->w = w;
    u->page = w->page;
    u->base = doc_page_pixels(d, w->page);
    u->page_id = PAGE(d, w->page)->id;
    for (int i = 0; i < 11; i++)
        *field(&u->a, i) = color_rows[i].def;
    /* The histogram, sampled: every 4th pixel is plenty. */
    int pw = gdk_pixbuf_get_width(u->base), ph = gdk_pixbuf_get_height(u->base);
    int n = gdk_pixbuf_get_n_channels(u->base), rs = gdk_pixbuf_get_rowstride(u->base);
    const guchar *px = gdk_pixbuf_read_pixels(u->base);
    for (int y = 0; y < ph; y += 2)
        for (int x = 0; x < pw; x += 2)
            for (int c = 0; c < 3; c++)
                u->hist[c][px[y * rs + x * n + c]]++;

    u->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    w->adjust_color = u->win;
    gtk_window_set_title(GTK_WINDOW(u->win), "Adjust Color");
    gtk_window_set_transient_for(GTK_WINDOW(u->win), parent_of(w));
    gtk_window_set_type_hint(GTK_WINDOW(u->win), GDK_WINDOW_TYPE_HINT_UTILITY);
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(u->win), g);
    GtkWidget *hist = gtk_drawing_area_new();
    gtk_widget_set_size_request(hist, S(280), S(90));
    g_signal_connect(hist, "draw", G_CALLBACK(hist_draw), u);
    gtk_grid_attach(GTK_GRID(g), hist, 0, 0, 2, 1);
    for (int i = 0; i < 11; i++) {
        u->sliders[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, color_rows[i].lo, color_rows[i].hi, .01);
        gtk_scale_set_draw_value(GTK_SCALE(u->sliders[i]), FALSE);
        gtk_range_set_value(GTK_RANGE(u->sliders[i]), color_rows[i].def);
        gtk_scale_add_mark(GTK_SCALE(u->sliders[i]), color_rows[i].def, GTK_POS_BOTTOM, NULL);
        gtk_widget_set_hexpand(u->sliders[i], TRUE);
        g_signal_connect(u->sliders[i], "value-changed", G_CALLBACK(color_changed), u);
        gtk_grid_attach(GTK_GRID(g), label_right(color_rows[i].name), 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(g), u->sliders[i], 1, i + 1, 1, 1);
    }
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    GtkWidget *autob = gtk_button_new_with_label("Auto Levels"), *reset = gtk_button_new_with_label("Reset All");
    gtk_box_pack_start(GTK_BOX(bb), autob, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb), reset, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g), bb, 0, 12, 2, 1);
    g_signal_connect(autob, "clicked", G_CALLBACK(color_auto), u);
    g_signal_connect(reset, "clicked", G_CALLBACK(color_reset), u);
    g_signal_connect(u->win, "destroy", G_CALLBACK(color_destroy), u);
    gtk_widget_show_all(u->win);
}

/* --- the Inspector ---------------------------------------------------------------- */

static void info_row(GtkWidget *g, int *row, const char *k, const char *v)
{
    if (!v || !*v)
        return;
    gtk_grid_attach(GTK_GRID(g), label_right(k), 0, *row, 1, 1);
    GtkWidget *l = label_left(v);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
    gtk_grid_attach(GTK_GRID(g), l, 1, (*row)++, 1, 1);
}

static char *date_str(GDateTime *t)
{
    return t ? g_date_time_format(t, "%e %B %Y at %H:%M") : NULL;
}

static char *time_str(time_t t)
{
    if (t <= 0)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    char *s = date_str(dt);
    g_date_time_unref(dt);
    return s;
}

static void inspector_note_clicked(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    struct win *w = data;
    struct mark *m = g_object_get_data(G_OBJECT(row), "mark");
    if (m && g_ptr_array_find(w->doc->st->marks, m, NULL))
        view_select_mark(w->view, m);
}

static void inspector_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    ((struct win *)data)->inspector = NULL;
}

void dlg_inspector(struct win *w)
{
    if (w->inspector) {             /* Show Inspector toggles it */
        gtk_widget_destroy(w->inspector);
        return;
    }
    struct doc *d = w->doc;
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    w->inspector = win;
    char *title = g_strdup_printf("%s Info", d->name);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent_of(w));
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_default_size(GTK_WINDOW(win), S(380), S(420));
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(win), nb);

    /* General */
    GtkWidget *g = grid_new();
    int row = 0;
    struct stat st;
    gboolean have = d->path && g_stat(d->path, &st) == 0;
    info_row(g, &row, "File:", d->name);
    info_row(g, &row, "Kind:", d->kind == DOC_PDF ? "PDF document" : d->format);
    char *s = have ? human_size(st.st_size) : NULL;
    info_row(g, &row, "File Size:", s);
    g_free(s);
    s = have ? time_str(st.st_mtime) : NULL;
    info_row(g, &row, "Modified:", s);
    g_free(s);
    info_row(g, &row, "Where:", d->path);
    if (d->kind == DOC_PDF) {
        double pw, ph;
        page_size(PAGE(d, w->page), &pw, &ph);
        s = g_strdup_printf("%d", NPAGES(d));
        info_row(g, &row, "Pages:", s);
        g_free(s);
        s = g_strdup_printf("%.0f × %.0f points (%.1f × %.1f cm)", pw, ph, pw / 72 * 2.54, ph / 72 * 2.54);
        info_row(g, &row, "Page Size:", s);
        g_free(s);
        char *v;
        static const char *const keys[][2] = {{"title", "Title:"},       {"author", "Author:"},
                                              {"subject", "Subject:"},   {"keywords", "Keywords:"},
                                              {"creator", "Content Creator:"}, {"producer", "PDF Producer:"}};
        for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
            g_object_get(d->pdf, keys[k][0], &v, NULL);
            info_row(g, &row, keys[k][1], v);
            g_free(v);
        }
        s = date_str(poppler_document_get_creation_date_time(d->pdf));
        info_row(g, &row, "Created:", s);
        g_free(s);
        v = poppler_document_get_pdf_version_string(d->pdf);
        info_row(g, &row, "Version:", v);
        g_free(v);
    } else {
        struct page *p = PAGE(d, w->page);
        s = g_strdup_printf("%.0f × %.0f pixels", p->w, p->h);
        info_row(g, &row, "Image Size:", s);
        g_free(s);
        s = g_strdup_printf("%.0f pixels/inch", d->dpi);
        info_row(g, &row, "Resolution:", s);
        g_free(s);
        info_row(g, &row, "Color Model:", gdk_pixbuf_get_has_alpha(p->pix) ? "RGB with alpha" : "RGB");
        if (NPAGES(d) > 1) {
            s = g_strdup_printf("%d", NPAGES(d));
            info_row(g, &row, d->frames ? "Frames:" : "Images:", s);
            g_free(s);
        }
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("General"));

    /* Annotations: every mark, click to go to it */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *lb = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(sw), lb);
    for (guint k = 0; k < d->st->marks->len; k++) {
        struct mark *m = MARK(d, k);
        int pg = 0;
        for (int i = 0; i < NPAGES(d); i++)
            if (PAGE(d, i)->id == m->page)
                pg = i + 1;
        char *t = g_strdup_printf("Page %d — %s%s%s", pg, mark_name(m->kind), m->text ? ": " : "",
                                  m->text ? m->text : "");
        GtkWidget *l = label_left(t);
        gtk_label_set_selectable(GTK_LABEL(l), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        g_free(t);
        GtkWidget *r = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(r), l);
        g_object_set_data(G_OBJECT(r), "mark", m);
        gtk_list_box_insert(GTK_LIST_BOX(lb), r, -1);
    }
    if (!d->st->marks->len)
        gtk_list_box_insert(GTK_LIST_BOX(lb), label_left("No annotations"), -1);
    g_signal_connect(lb, "row-activated", G_CALLBACK(inspector_note_clicked), w);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Annotations"));

    /* Encryption (PDF) or Crop (pictures) */
    g = grid_new();
    row = 0;
    if (d->kind == DOC_PDF) {
        PopplerPermissions pm = poppler_document_get_permissions(d->pdf);
        info_row(g, &row, "Encrypted:", d->password || pm != POPPLER_PERMISSIONS_FULL ? "Yes" : "No");
        info_row(g, &row, "Printing:", pm & POPPLER_PERMISSIONS_OK_TO_PRINT ? "Allowed" : "Not Allowed");
        info_row(g, &row, "Copying:", pm & POPPLER_PERMISSIONS_OK_TO_COPY ? "Allowed" : "Not Allowed");
        info_row(g, &row, "Changing:", pm & POPPLER_PERMISSIONS_OK_TO_MODIFY ? "Allowed" : "Not Allowed");
        info_row(g, &row, "Annotating:", pm & POPPLER_PERMISSIONS_OK_TO_ADD_NOTES ? "Allowed" : "Not Allowed");
        info_row(g, &row, "Form Filling:", pm & POPPLER_PERMISSIONS_OK_TO_FILL_FORM ? "Allowed" : "Not Allowed");
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Encryption"));
    } else {
        int pg, shape;
        double r[4];
        if (view_area(w->view, &pg, r, &shape, NULL)) {
            s = g_strdup_printf("%.0f × %.0f pixels at %.0f, %.0f", r[2], r[3], r[0], r[1]);
            info_row(g, &row, "Selection:", s);
            g_free(s);
        } else {
            info_row(g, &row, "Selection:", "None");
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Crop"));
    }
    g_signal_connect(win, "destroy", G_CALLBACK(inspector_destroy), w);
    gtk_widget_show_all(win);
}

/* --- Settings ------------------------------------------------------------------------ */

/* The radio groups pick a number; these prefs are yes/no, first choice yes. */
static int st_px100, st_fit;

static void settings_apply(void)
{
    prefs.image_pixels_100 = st_px100 == 0;
    prefs.pdf_fit = st_fit == 0;
    prefs_save();
    for (GList *l = win_list(); l; l = l->next)
        view_redraw(((struct win *)l->data)->view);
}

static void set_bg(GtkColorButton *b, gpointer data)
{
    (void)data;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &prefs.background);
    settings_apply();
}

static void set_radio(GtkToggleButton *b, gpointer data)
{
    if (!gtk_toggle_button_get_active(b))
        return;
    int *target = data;
    *target = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "value"));
    settings_apply();
}

static void set_check(GtkToggleButton *b, gpointer data)
{
    *(gboolean *)data = gtk_toggle_button_get_active(b);
    settings_apply();
}

static GtkWidget *radios(GtkWidget *g, int *row, const char *title, const char *const *labels, int *target)
{
    GtkWidget *first = NULL;
    for (int i = 0; labels[i]; i++) {
        GtkWidget *r = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first), labels[i]);
        first = first ? first : r;
        g_object_set_data(G_OBJECT(r), "value", GINT_TO_POINTER(i));
        if (*target == i)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r), TRUE);
        g_signal_connect(r, "toggled", G_CALLBACK(set_radio), target);
        if (i == 0)
            gtk_grid_attach(GTK_GRID(g), label_right(title), 0, *row, 1, 1);
        gtk_grid_attach(GTK_GRID(g), r, 1, (*row)++, 1, 1);
    }
    return first;
}

static void open_signatures(GtkButton *b, gpointer data)
{
    (void)b, (void)data;
    GList *l = win_list();
    if (l)
        dlg_signatures(l->data);
}

void dlg_settings(GtkWindow *parent)
{
    static GtkWidget *win;
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&win);
    gtk_window_set_title(GTK_WINDOW(win), "Settings");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(win), nb);

    GtkWidget *g = grid_new();
    int row = 0;
    GtkWidget *bg = gtk_color_button_new_with_rgba(&prefs.background);
    gtk_grid_attach(GTK_GRID(g), label_right("Window background:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), bg, 1, row++, 1, 1);
    g_signal_connect(bg, "color-set", G_CALLBACK(set_bg), NULL);
    st_px100 = prefs.image_pixels_100 ? 0 : 1;
    st_fit = prefs.pdf_fit ? 0 : 1;
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("General"));

    g = grid_new();
    row = 0;
    radios(g, &row, "Define 100% scale as:",
           (const char *const[]){"1 image pixel equals 1 screen pixel", "Size on screen equals size on printout", NULL},
           &st_px100);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Images"));

    g = grid_new();
    row = 0;
    radios(g, &row, "Opening for the first time:", (const char *const[]){"Zoom to fit", "Scale to 100%", NULL}, &st_fit);
    radios(g, &row, "Pages to show:",
           (const char *const[]){"Continuous Scroll", "Single Page", "Two Pages", NULL}, (int *)&prefs.pdf_mode);
    GtkWidget *smooth = gtk_check_button_new_with_label("Smooth text and line art");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(smooth), prefs.smooth);
    g_signal_connect(smooth, "toggled", G_CALLBACK(set_check), &prefs.smooth);
    gtk_grid_attach(GTK_GRID(g), smooth, 1, row++, 1, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("PDF"));

    g = grid_new();
    GtkWidget *sb = gtk_button_new_with_label("Manage Signatures…");
    g_signal_connect(sb, "clicked", G_CALLBACK(open_signatures), NULL);
    gtk_grid_attach(GTK_GRID(g), sb, 0, 0, 1, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), g, gtk_label_new("Signatures"));

    gtk_widget_show_all(win);
}

/* --- small questions ------------------------------------------------------------------- */

void dlg_go_to_page(struct win *w)
{
    int n = NPAGES(w->doc);
    if (n < 1)
        return;
    GtkWidget *dl = dialog(parent_of(w), "Go to Page", "OK");
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, n, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), w->page + 1);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    char *of = g_strdup_printf("of %d", n);
    gtk_grid_attach(GTK_GRID(g), label_right("Go to page:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), spin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), label_left(of), 2, 0, 1, 1);
    g_free(of);
    gtk_widget_show_all(dl);
    if (gtk_dialog_run(GTK_DIALOG(dl)) == GTK_RESPONSE_OK)
        win_set_page(w, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) - 1);
    gtk_widget_destroy(dl);
}

char *dlg_password(GtkWindow *parent, const char *file, gboolean again)
{
    GtkWidget *dl = dialog(parent, "Password", "Open");
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    GtkWidget *lock = symbol_new(SY_LOCK, 40);
    gtk_grid_attach(GTK_GRID(g), lock, 0, 0, 1, 2);
    char *name = g_filename_display_basename(file);
    char *t = g_strdup_printf("<b>“%s” is locked.</b>", name);
    GtkWidget *l = label_left("");
    gtk_label_set_markup(GTK_LABEL(l), t);
    g_free(t), g_free(name);
    gtk_grid_attach(GTK_GRID(g), l, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(g), label_left(again ? "That password is not right. Try again."
                                                  : "Enter the password to open it."), 1, 1, 2, 1);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Password");
    gtk_grid_attach(GTK_GRID(g), e, 1, 2, 2, 1);
    gtk_widget_show_all(dl);
    char *pw = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dl)) == GTK_RESPONSE_OK)
        pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    gtk_widget_destroy(dl);
    return pw;
}

static void moved_to(struct win *w, const char *path)
{
    struct doc *d = w->doc;
    g_free(d->path);
    d->path = g_strdup(path);
    g_free(d->name);
    d->name = g_filename_display_basename(path);
    /* Pages of the file point at it by name, for qpdf. */
    for (int i = 0; i < NPAGES(d); i++)
        if (PAGE(d, i)->pdf == d->pdf) {
            g_free(PAGE(d, i)->src);
            PAGE(d, i)->src = g_strdup(path);
        }
    win_update(w);
}

/* Rename across folders and file systems. */
static gboolean move_file(const char *from, const char *to, GError **err)
{
    GFile *a = g_file_new_for_path(from), *b = g_file_new_for_path(to);
    gboolean ok = g_file_move(a, b, G_FILE_COPY_NONE, NULL, NULL, NULL, err);
    g_object_unref(a), g_object_unref(b);
    return ok;
}

void dlg_rename(struct win *w)
{
    struct doc *d = w->doc;
    if (!d->path)
        return;
    GtkWidget *dl = dialog(parent_of(w), "Rename", "Rename");
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), d->name);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 36);
    gtk_grid_attach(GTK_GRID(g), label_right("Name:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), e, 1, 0, 1, 1);
    gtk_widget_show_all(dl);
    /* Select the name without its extension, as Finder does. */
    const char *dot = strrchr(d->name, '.');
    gtk_editable_select_region(GTK_EDITABLE(e), 0, dot ? (int)g_utf8_pointer_to_offset(d->name, dot) : -1);
    if (gtk_dialog_run(GTK_DIALOG(dl)) == GTK_RESPONSE_OK) {
        const char *n = gtk_entry_get_text(GTK_ENTRY(e));
        if (*n && !strchr(n, '/') && strcmp(n, d->name)) {
            char *dir = g_path_get_dirname(d->path);
            char *to = g_build_filename(dir, n, NULL);
            GError *err = NULL;
            if (g_file_test(to, G_FILE_TEST_EXISTS))
                win_error(w, "That name is taken. Please choose a different name.", NULL);
            else if (move_file(d->path, to, &err))
                moved_to(w, to);
            else
                win_error(w, "The document could not be renamed.", err);
            g_clear_error(&err);
            g_free(to), g_free(dir);
        }
    }
    gtk_widget_destroy(dl);
}

void dlg_move_to(struct win *w)
{
    struct doc *d = w->doc;
    if (!d->path)
        return;
    GtkWidget *c = gtk_file_chooser_dialog_new("Move To", parent_of(w), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                               "Cancel", GTK_RESPONSE_CANCEL, "Move", GTK_RESPONSE_OK, NULL);
    if (gtk_dialog_run(GTK_DIALOG(c)) == GTK_RESPONSE_OK) {
        char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(c));
        char *base = g_path_get_basename(d->path);
        char *to = g_build_filename(dir, base, NULL);
        GError *err = NULL;
        if (strcmp(to, d->path)) {
            if (g_file_test(to, G_FILE_TEST_EXISTS))
                win_error(w, "A file with that name is already in that folder.", NULL);
            else if (move_file(d->path, to, &err))
                moved_to(w, to);
            else
                win_error(w, "The document could not be moved.", err);
        }
        g_clear_error(&err);
        g_free(to), g_free(base), g_free(dir);
    }
    gtk_widget_destroy(c);
}

/* --- signatures ------------------------------------------------------------------------ */

/* One signature per line: x y pairs (0..1), "nan nan" between strokes. */
GPtrArray *signatures_load(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)g_array_unref);
    char *path = config_path("preview-signatures"), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (!**l)
                continue;
            GArray *pts = g_array_new(FALSE, FALSE, sizeof(double));
            char *p = *l, *end;
            for (;;) {
                double v = g_ascii_strtod(p, &end);
                if (end == p)
                    break;
                g_array_append_val(pts, v);
                p = end;
            }
            if (pts->len >= 4 && pts->len % 2 == 0)
                g_ptr_array_add(a, pts);
            else
                g_array_unref(pts);
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
    return a;
}

void signatures_save(GPtrArray *sigs)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < sigs->len; i++) {
        GArray *pts = g_ptr_array_index(sigs, i);
        for (guint j = 0; j < pts->len; j++) {
            double v = g_array_index(pts, double, j);
            if (isnan(v))
                g_string_append(s, "nan ");
            else
                g_string_append_printf(s, "%.4f ", v);
        }
        g_string_append_c(s, '\n');
    }
    char *path = config_path("preview-signatures");
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(path);
    g_string_free(s, TRUE);
}

static void draw_sig(cairo_t *cr, GArray *pts, double w, double h, double lw)
{
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    gboolean open = FALSE;
    for (guint i = 0; i + 1 < pts->len; i += 2) {
        double x = g_array_index(pts, double, i), y = g_array_index(pts, double, i + 1);
        if (isnan(x)) {
            open = FALSE;
            continue;
        }
        if (!open)
            cairo_move_to(cr, x * w, y * h), cairo_line_to(cr, x * w + .1, y * h);
        else
            cairo_line_to(cr, x * w, y * h);
        open = TRUE;
    }
    cairo_stroke(cr);
}

struct sig_ui {
    struct win *w;
    GtkWidget *win, *list, *pad;
    GPtrArray *sigs;
    GArray *drawing;            /* canvas pixels while drawing */
    gboolean down;
};

static gboolean sig_preview_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    GArray *pts = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    double s = MIN(w, h * 3);
    cairo_translate(cr, (w - s) / 2, (h - s / 3) / 2);
    draw_sig(cr, pts, s, s / 3, S(1.5));
    return TRUE;
}

static void sig_rebuild(struct sig_ui *u);

static void sig_pick(GtkButton *b, gpointer data)
{
    struct sig_ui *u = data;
    u->w->sign_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));
    win_set_tool(u->w, T_SIGN);
    gtk_widget_destroy(u->win);
}

static void sig_delete(GtkButton *b, gpointer data)
{
    struct sig_ui *u = data;
    g_ptr_array_remove_index(u->sigs, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index")));
    signatures_save(u->sigs);
    sig_rebuild(u);
}

static void sig_rebuild(struct sig_ui *u)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(u->list));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    for (guint i = 0; i < u->sigs->len; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
        GtkWidget *b = gtk_button_new();
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, S(210), S(70));
        g_signal_connect(da, "draw", G_CALLBACK(sig_preview_draw), g_ptr_array_index(u->sigs, i));
        gtk_container_add(GTK_CONTAINER(b), da);
        g_object_set_data(G_OBJECT(b), "index", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(sig_pick), u);
        GtkWidget *del = gtk_button_new_with_label("Delete");
        g_object_set_data(G_OBJECT(del), "index", GINT_TO_POINTER(i));
        g_signal_connect(del, "clicked", G_CALLBACK(sig_delete), u);
        gtk_box_pack_start(GTK_BOX(row), b, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), del, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(u->list), row, FALSE, FALSE, 0);
    }
    if (!u->sigs->len)
        gtk_box_pack_start(GTK_BOX(u->list), label_left("No signatures yet. Sign on the pad below."), FALSE, FALSE, 0);
    gtk_widget_show_all(u->list);
}

static gboolean pad_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    struct sig_ui *u = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, .6, .6, .6);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, S(20), h * .72 + .5);
    cairo_line_to(cr, w - S(20), h * .72 + .5);
    cairo_stroke(cr);
    if (!u->drawing->len) {
        PangoLayout *l = gtk_widget_create_pango_layout(wd, "Sign your name here with the mouse.");
        int lw, lh;
        pango_layout_get_pixel_size(l, &lw, &lh);
        cairo_move_to(cr, (w - lw) / 2, h * .72 + S(6));
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
    }
    cairo_set_source_rgb(cr, 0, 0, 0);
    draw_sig(cr, u->drawing, 1, 1, S(2));
    return TRUE;
}

static gboolean pad_press(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    struct sig_ui *u = data;
    u->down = TRUE;
    if (u->drawing->len) {
        double n = NAN;
        g_array_append_val(u->drawing, n);
        g_array_append_val(u->drawing, n);
    }
    g_array_append_val(u->drawing, e->x);
    g_array_append_val(u->drawing, e->y);
    gtk_widget_queue_draw(wd);
    return TRUE;
}

static gboolean pad_motion(GtkWidget *wd, GdkEventMotion *e, gpointer data)
{
    struct sig_ui *u = data;
    if (!u->down)
        return FALSE;
    g_array_append_val(u->drawing, e->x);
    g_array_append_val(u->drawing, e->y);
    gtk_widget_queue_draw(wd);
    return TRUE;
}

static gboolean pad_release(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    (void)wd, (void)e;
    ((struct sig_ui *)data)->down = FALSE;
    return TRUE;
}

static void pad_clear(GtkButton *b, gpointer data)
{
    (void)b;
    struct sig_ui *u = data;
    g_array_set_size(u->drawing, 0);
    gtk_widget_queue_draw(u->pad);
}

/* Keep the signature: fit its strokes into a 3:1 box, centred. */
static void pad_done(GtkButton *b, gpointer data)
{
    (void)b;
    struct sig_ui *u = data;
    GArray *d = u->drawing;
    if (d->len < 4)
        return;
    double x1 = G_MAXDOUBLE, y1 = G_MAXDOUBLE, x2 = -G_MAXDOUBLE, y2 = -G_MAXDOUBLE;
    for (guint i = 0; i < d->len; i += 2) {
        double x = g_array_index(d, double, i), y = g_array_index(d, double, i + 1);
        if (isnan(x))
            continue;
        x1 = MIN(x1, x), y1 = MIN(y1, y), x2 = MAX(x2, x), y2 = MAX(y2, y);
    }
    double w = MAX(x2 - x1, 1), h = MAX(y2 - y1, 1);
    double bw = MAX(w, h * 3), bh = bw / 3;
    double ox = x1 - (bw - w) / 2, oy = y1 - (bh - h) / 2;
    GArray *n = g_array_sized_new(FALSE, FALSE, sizeof(double), d->len);
    for (guint i = 0; i < d->len; i += 2) {
        double x = g_array_index(d, double, i), y = g_array_index(d, double, i + 1);
        if (!isnan(x))
            x = (x - ox) / bw, y = (y - oy) / bh;
        g_array_append_val(n, x);
        g_array_append_val(n, y);
    }
    g_ptr_array_add(u->sigs, n);
    signatures_save(u->sigs);
    pad_clear(NULL, u);
    sig_rebuild(u);
}

static void sig_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct sig_ui *u = data;
    g_ptr_array_unref(u->sigs);
    g_array_unref(u->drawing);
    g_free(u);
}

void dlg_signatures(struct win *w)
{
    struct sig_ui *u = g_new0(struct sig_ui, 1);
    u->w = w;
    u->sigs = signatures_load();
    u->drawing = g_array_new(FALSE, FALSE, sizeof(double));
    u->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(u->win), "Signatures");
    gtk_window_set_transient_for(GTK_WINDOW(u->win), parent_of(w));
    gtk_window_set_modal(GTK_WINDOW(u->win), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
    gtk_container_set_border_width(GTK_CONTAINER(box), S(14));
    gtk_container_add(GTK_CONTAINER(u->win), box);
    gtk_box_pack_start(GTK_BOX(box), label_left("Click a signature to place it on the page."), FALSE, FALSE, 0);
    u->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
    gtk_box_pack_start(GTK_BOX(box), u->list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, S(4));
    gtk_box_pack_start(GTK_BOX(box), label_left("Create Signature"), FALSE, FALSE, 0);
    u->pad = gtk_drawing_area_new();
    gtk_widget_set_size_request(u->pad, S(420), S(140));
    gtk_widget_add_events(u->pad, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
    g_signal_connect(u->pad, "draw", G_CALLBACK(pad_draw), u);
    g_signal_connect(u->pad, "button-press-event", G_CALLBACK(pad_press), u);
    g_signal_connect(u->pad, "motion-notify-event", G_CALLBACK(pad_motion), u);
    g_signal_connect(u->pad, "button-release-event", G_CALLBACK(pad_release), u);
    gtk_box_pack_start(GTK_BOX(box), u->pad, TRUE, TRUE, 0);
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    GtkWidget *clear = gtk_button_new_with_label("Clear"), *done = gtk_button_new_with_label("Done");
    gtk_box_pack_start(GTK_BOX(bb), clear, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bb), done, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 0);
    g_signal_connect(clear, "clicked", G_CALLBACK(pad_clear), u);
    g_signal_connect(done, "clicked", G_CALLBACK(pad_done), u);
    g_signal_connect(u->win, "destroy", G_CALLBACK(sig_destroy), u);
    sig_rebuild(u);
    gtk_widget_show_all(u->win);
}

/* --- bookmarks ------------------------------------------------------------------------- */

static GKeyFile *bookmarks_file(char **path)
{
    *path = config_path("preview-bookmarks");
    GKeyFile *k = g_key_file_new();
    g_key_file_load_from_file(k, *path, G_KEY_FILE_NONE, NULL);
    return k;
}

static void bm_free(gpointer p)
{
    struct bookmark *b = p;
    g_free(b->name);
    g_free(b);
}

static int bm_cmp(gconstpointer a, gconstpointer b)
{
    return (*(struct bookmark *const *)a)->page - (*(struct bookmark *const *)b)->page;
}

GPtrArray *bookmarks_get(const char *path)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(bm_free);
    if (!path)
        return a;
    char *f;
    GKeyFile *k = bookmarks_file(&f);
    char **keys = g_key_file_get_keys(k, path, NULL, NULL);
    for (char **p = keys; p && *p; p++) {
        struct bookmark *b = g_new(struct bookmark, 1);
        b->page = atoi(*p) - 1;
        b->name = g_key_file_get_string(k, path, *p, NULL);
        g_ptr_array_add(a, b);
    }
    g_ptr_array_sort(a, bm_cmp);
    g_strfreev(keys);
    g_key_file_free(k);
    g_free(f);
    return a;
}

void bookmark_add(const char *path, int page, const char *name)
{
    char *f, key[16];
    GKeyFile *k = bookmarks_file(&f);
    g_snprintf(key, sizeof key, "%d", page + 1);
    g_key_file_set_string(k, path, key, name);
    g_key_file_save_to_file(k, f, NULL);
    g_key_file_free(k);
    g_free(f);
}

void bookmark_remove(const char *path, int page)
{
    char *f, key[16];
    GKeyFile *k = bookmarks_file(&f);
    g_snprintf(key, sizeof key, "%d", page + 1);
    g_key_file_remove_key(k, path, key, NULL);
    g_key_file_save_to_file(k, f, NULL);
    g_key_file_free(k);
    g_free(f);
}

void dlg_bookmark_name(struct win *w)
{
    struct doc *d = w->doc;
    if (!d->path)
        return;
    GtkWidget *dl = dialog(parent_of(w), "Add Bookmark", "Add");
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    GtkWidget *e = gtk_entry_new();
    char *def = g_strdup_printf("Page %d", w->page + 1);
    gtk_entry_set_text(GTK_ENTRY(e), def);
    g_free(def);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_grid_attach(GTK_GRID(g), label_right("Bookmark name:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), e, 1, 0, 1, 1);
    gtk_widget_show_all(dl);
    if (gtk_dialog_run(GTK_DIALOG(dl)) == GTK_RESPONSE_OK) {
        bookmark_add(d->path, w->page, gtk_entry_get_text(GTK_ENTRY(e)));
        win_update(w);
    }
    gtk_widget_destroy(dl);
}

/* --- About, Help ---------------------------------------------------------------------- */

void dlg_about(GtkWindow *parent)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S(128), S(128));
    cairo_t *cr = cairo_create(s);
    app_icon_draw(cr, S(128));
    cairo_destroy(cr);
    GdkPixbuf *icon = gdk_pixbuf_get_from_surface(s, 0, 0, S(128), S(128));
    cairo_surface_destroy(s);
    gtk_show_about_dialog(parent, "program-name", "Preview", "version", "11.0 (MyOS)", "logo", icon,
                          "comments", "View, mark up and convert PDFs and pictures.",
                          "copyright", "MyOS", NULL);
    g_object_unref(icon);
}

static const char help_text[] =
    "<big><b>Preview Help</b></big>\n\n"
    "<b>Open</b> PDFs and pictures (HEIC, JPEG, JPEG-2000, OpenEXR, PNG, TIFF, GIF, BMP, ICO, ICNS, TGA, "
    "WebP, AVIF, JPEG XL, SVG, Photoshop, camera RAW) with File ▸ Open or by double-clicking them.\n\n"
    "<b>Move around</b>: scroll, Page Up / Page Down, Home / End, or click a thumbnail in the sidebar "
    "(View ▸ Thumbnails, Alt+Ctrl+2). Go ▸ Go to Page (Alt+Ctrl+G) jumps to a page.\n\n"
    "<b>Zoom</b>: Ctrl+plus, Ctrl+minus, Ctrl+0 for actual size, Ctrl+9 to fit; or Ctrl + mouse wheel.\n\n"
    "<b>Find</b> text with the search field (Ctrl+F); Ctrl+G and Shift+Ctrl+G step through the matches.\n\n"
    "<b>Mark up</b>: show the Markup toolbar (Shift+Ctrl+A). Draw shapes, add text boxes, notes and your "
    "signature; highlight text with the highlight button. Click a mark to select it, drag it to move it, "
    "drag a handle to resize it, Delete removes it.\n\n"
    "<b>Pictures</b>: select an area and choose Tools ▸ Crop (Ctrl+K). Tools ▸ Adjust Color and Adjust "
    "Size change the picture; Instant Alpha (in the Markup toolbar) removes a background: click it and "
    "drag further for more.\n\n"
    "<b>Pages</b>: drag thumbnails to reorder them, select one and press Delete to remove it, or use "
    "Edit ▸ Insert. Changes are saved when you save (Ctrl+S) or close the window.\n\n"
    "<b>Convert</b>: File ▸ Export saves in another format; hold Alt when you choose it to see every "
    "format.\n\n"
    "<b>Shortcuts</b>: the Mac's ⌘ is Ctrl here, ⌥ is Alt and ⌃ is the Windows key.";

void dlg_help(GtkWindow *parent)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Preview Help");
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_default_size(GTK_WINDOW(win), S(520), S(560));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), help_text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    g_object_set(l, "margin", S(18), NULL);
    gtk_container_add(GTK_CONTAINER(sw), l);
    gtk_container_add(GTK_CONTAINER(win), sw);
    gtk_widget_show_all(win);
}

/* --- Customize Toolbar ----------------------------------------------------------------- */

static void tb_toggled(GtkToggleButton *b, gpointer data)
{
    (void)data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "item"));
    if (gtk_toggle_button_get_active(b))
        prefs.toolbar_hidden &= ~(1u << i);
    else
        prefs.toolbar_hidden |= 1u << i;
    prefs_save();
    for (GList *l = win_list(); l; l = l->next)
        win_apply_toolbar(l->data);
}

static void tb_text(GtkComboBox *c, gpointer data)
{
    (void)data;
    prefs.toolbar_text = gtk_combo_box_get_active(c) == 0;
    prefs_save();
    for (GList *l = win_list(); l; l = l->next)
        win_apply_toolbar(l->data);
}

void dlg_customize_toolbar(struct win *w)
{
    GtkWidget *dl = gtk_dialog_new_with_buttons("Customize Toolbar", parent_of(w), GTK_DIALOG_MODAL, "Done",
                                                GTK_RESPONSE_OK, NULL);
    GtkWidget *g = grid_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dl))), g);
    gtk_grid_attach(GTK_GRID(g), label_left("Choose the items in the toolbar:"), 0, 0, 2, 1);
    for (int i = 0; i < n_toolbar_items; i++) {
        GtkWidget *c = gtk_check_button_new_with_label(toolbar_items[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), !(prefs.toolbar_hidden & (1u << i)));
        g_object_set_data(G_OBJECT(c), "item", GINT_TO_POINTER(i));
        g_signal_connect(c, "toggled", G_CALLBACK(tb_toggled), NULL);
        gtk_grid_attach(GTK_GRID(g), c, i % 2, 1 + i / 2, 1, 1);
    }
    GtkWidget *show = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(show), "Icon and Text");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(show), "Icon Only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(show), prefs.toolbar_text ? 0 : 1);
    g_signal_connect(show, "changed", G_CALLBACK(tb_text), NULL);
    int row = 2 + n_toolbar_items / 2;
    gtk_grid_attach(GTK_GRID(g), label_right("Show:"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g), show, 1, row, 1, 1);
    gtk_widget_show_all(dl);
    gtk_dialog_run(GTK_DIALOG(dl));
    gtk_widget_destroy(dl);
}

/* --- Take Screenshot -------------------------------------------------------------------- */

struct shot {
    int kind;
    GdkPixbuf *screen;
    GtkWidget *win;
    double x0, y0, x1, y1;
    gboolean down;
};

static void shot_done(struct shot *s, GdkPixbuf *pix)
{
    for (GList *l = win_list(); l; l = l->next)
        gtk_widget_show(GTK_WIDGET(((struct win *)l->data)->window));
    if (pix) {
        GDateTime *now = g_date_time_new_now_local();
        char *name = g_date_time_format(now, "Screenshot %Y-%m-%d at %H.%M.%S");
        g_date_time_unref(now);
        win_new(doc_new_image(pix, name));
        g_free(name);
    }
    if (s->win)
        gtk_widget_destroy(s->win);
    g_object_unref(s->screen);
    g_free(s);
}

static gboolean shot_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    (void)wd;
    struct shot *s = data;
    gdk_cairo_set_source_pixbuf(cr, s->screen, 0, 0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, .25);
    cairo_paint(cr);
    if (s->down) {
        double x = MIN(s->x0, s->x1), y = MIN(s->y0, s->y1), w = fabs(s->x1 - s->x0), h = fabs(s->y1 - s->y0);
        cairo_save(cr);
        cairo_rectangle(cr, x, y, w, h);
        cairo_clip(cr);
        gdk_cairo_set_source_pixbuf(cr, s->screen, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_rectangle(cr, x + .5, y + .5, w, h);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    return TRUE;
}

/* The top-level window under (x, y): the root's children are in stacking
   order, bottom first, so the last that's mapped and contains it wins. */
static gboolean window_at(int x, int y, GdkRectangle *r)
{
    Display *dp = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window root = DefaultRootWindow(dp), rr, parent, *kids = NULL;
    unsigned n = 0;
    gboolean found = FALSE;
    if (!XQueryTree(dp, root, &rr, &parent, &kids, &n))
        return FALSE;
    for (unsigned i = n; i-- > 0;) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(dp, kids[i], &a) || a.map_state != IsViewable || a.class != InputOutput)
            continue;
        if (x >= a.x && y >= a.y && x < a.x + a.width && y < a.y + a.height) {
            /* Skip our own full-screen overlay. */
            if (a.width >= DisplayWidth(dp, DefaultScreen(dp)) && a.height >= DisplayHeight(dp, DefaultScreen(dp)))
                continue;
            *r = (GdkRectangle){a.x, a.y, a.width, a.height};
            found = TRUE;
            break;
        }
    }
    if (kids)
        XFree(kids);
    return found;
}

static gboolean shot_press(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    (void)wd;
    struct shot *s = data;
    if (e->button != 1) {
        shot_done(s, NULL);
        return TRUE;
    }
    if (s->kind == 1) {
        GdkRectangle r;
        if (window_at((int)e->x_root, (int)e->y_root, &r)) {
            int sw = gdk_pixbuf_get_width(s->screen), sh = gdk_pixbuf_get_height(s->screen);
            int x = CLAMP(r.x, 0, sw - 1), y = CLAMP(r.y, 0, sh - 1);
            int w = MIN(r.width, sw - x), h = MIN(r.height, sh - y);
            shot_done(s, gdk_pixbuf_new_subpixbuf(s->screen, x, y, w, h));
        }
        return TRUE;
    }
    s->down = TRUE;
    s->x0 = s->x1 = e->x, s->y0 = s->y1 = e->y;
    return TRUE;
}

static gboolean shot_motion(GtkWidget *wd, GdkEventMotion *e, gpointer data)
{
    struct shot *s = data;
    if (s->down) {
        s->x1 = e->x, s->y1 = e->y;
        gtk_widget_queue_draw(wd);
    }
    return TRUE;
}

static gboolean shot_release(GtkWidget *wd, GdkEventButton *e, gpointer data)
{
    (void)wd, (void)e;
    struct shot *s = data;
    if (!s->down)
        return TRUE;
    int x = (int)MIN(s->x0, s->x1), y = (int)MIN(s->y0, s->y1);
    int w = (int)fabs(s->x1 - s->x0), h = (int)fabs(s->y1 - s->y0);
    if (w < 2 || h < 2) {
        s->down = FALSE;
        return TRUE;
    }
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(s->screen, x, y, w, h);
    GdkPixbuf *copy = gdk_pixbuf_copy(sub);
    g_object_unref(sub);
    shot_done(s, copy);
    return TRUE;
}

static gboolean shot_key(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd;
    if (e->keyval == GDK_KEY_Escape)
        shot_done(data, NULL);
    return TRUE;
}

static gboolean shot_take(gpointer data)
{
    struct shot *s = data;
    GdkWindow *root = gdk_get_default_root_window();
    int w = gdk_window_get_width(root), h = gdk_window_get_height(root);
    s->screen = gdk_pixbuf_get_from_window(root, 0, 0, w, h);
    if (!s->screen) {
        g_free(s);
        return G_SOURCE_REMOVE;
    }
    if (s->kind == 2) {                 /* the entire screen: done */
        shot_done(s, g_object_ref(s->screen));
        return G_SOURCE_REMOVE;
    }
    /* Selection or window: the frozen screen, full-screen, to pick on. */
    s->win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_move(GTK_WINDOW(s->win), 0, 0);
    gtk_window_set_default_size(GTK_WINDOW(s->win), w, h);
    GtkWidget *da = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(s->win), da);
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da, "draw", G_CALLBACK(shot_draw), s);
    g_signal_connect(da, "button-press-event", G_CALLBACK(shot_press), s);
    g_signal_connect(da, "motion-notify-event", G_CALLBACK(shot_motion), s);
    g_signal_connect(da, "button-release-event", G_CALLBACK(shot_release), s);
    g_signal_connect(s->win, "key-press-event", G_CALLBACK(shot_key), s);
    gtk_widget_show_all(s->win);
    GdkWindow *gw = gtk_widget_get_window(s->win);
    GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(gw), s->kind == 1 ? "pointer" : "crosshair");
    GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(gw));
    gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, c, NULL, NULL, NULL);
    if (c)
        g_object_unref(c);
    return G_SOURCE_REMOVE;
}

void dlg_screenshot(int kind)
{
    struct shot *s = g_new0(struct shot, 1);
    s->kind = kind;
    /* Our windows step aside first, as on the Mac. */
    for (GList *l = win_list(); l; l = l->next)
        gtk_widget_hide(GTK_WIDGET(((struct win *)l->data)->window));
    g_timeout_add(kind == 2 ? 600 : 350, shot_take, s);
}

/* --- closing with changes ------------------------------------------------------------------ */

gboolean dlg_close_changed(struct win *w)
{
    struct doc *d = w->doc;
    if (!d->edits)
        return TRUE;
    /* A file Preview can write is saved as it closes, as on the Mac. */
    if (d->path && d->writable) {
        GError *err = NULL;
        if (doc_save(d, &err))
            return TRUE;
        win_error(w, "The document could not be saved.", err);
        g_clear_error(&err);
        return FALSE;
    }
    GtkWidget *m = gtk_message_dialog_new(parent_of(w), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Do you want to keep this new document “%s”?", d->name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), d->path
        ? "Preview can't save in this file's format. You can export it in another format."
        : "You can choose to save your changes, or delete this document immediately. You can't undo this action.");
    gtk_dialog_add_buttons(GTK_DIALOG(m), "Delete", GTK_RESPONSE_REJECT, "Cancel", GTK_RESPONSE_CANCEL,
                           "Save…", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(m), GTK_RESPONSE_ACCEPT);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    if (r == GTK_RESPONSE_REJECT)
        return TRUE;
    if (r == GTK_RESPONSE_ACCEPT)
        return dlg_export(w, FALSE);
    return FALSE;
}

/* --- the colour popover ----------------------------------------------------------------- */

struct color_pick {
    GdkRGBA *target;
    void (*done)(struct win *);
    struct win *w;
    GtkWidget *pop;
};

static void swatch_clicked(GtkButton *b, gpointer data)
{
    struct color_pick *c = data;
    GdkRGBA *rgba = g_object_get_data(G_OBJECT(b), "rgba");
    *c->target = *rgba;
    gtk_popover_popdown(GTK_POPOVER(c->pop));
    c->done(c->w);
}

static gboolean swatch_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    GdkRGBA *c = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    if (c->alpha == 0) {                /* "none": a red slash */
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_move_to(cr, 2, h - 2);
        cairo_line_to(cr, w - 2, 2);
        cairo_set_source_rgb(cr, .9, .1, .1);
        cairo_set_line_width(cr, S(2));
        cairo_stroke(cr);
    } else {
        gdk_cairo_set_source_rgba(cr, c);
        cairo_paint(cr);
    }
    cairo_rectangle(cr, .5, .5, w - 1, h - 1);
    cairo_set_source_rgba(cr, 0, 0, 0, .3);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return TRUE;
}

static void show_colors(GtkButton *b, gpointer data)
{
    (void)b;
    struct color_pick *c = data;
    gtk_popover_popdown(GTK_POPOVER(c->pop));
    GtkWidget *d = gtk_color_chooser_dialog_new("Colors", parent_of(c->w));
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(d), TRUE);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(d), c->target);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(d), c->target);
        c->done(c->w);
    }
    gtk_widget_destroy(d);
}

GtkWidget *dlg_color_popover(GtkWidget *relative, GdkRGBA *target, void (*done)(struct win *), struct win *w)
{
    /* Preview's palette: a row of greys, then the colours in light,
       full and dark, plus "none". */
    static GdkRGBA pal[] = {
        {0, 0, 0, 0},       {1, 1, 1, 1},        {.8, .8, .8, 1},     {.6, .6, .6, 1},     {.4, .4, .4, 1},
        {.2, .2, .2, 1},    {0, 0, 0, 1},        {1, .23, .19, 1},    {1, .58, 0, 1},      {1, .8, 0, 1},
        {.2, .78, .35, 1},  {0, .48, 1, 1},      {.69, .32, .87, 1},  {1, .18, .33, 1},    {.64, .52, .37, 1},
        {1, .6, .6, 1},     {1, .8, .6, 1},      {1, 1, .6, 1},       {.6, 1, .7, 1},      {.6, .8, 1, 1},
        {.85, .7, 1, 1},    {.6, 0, 0, 1},       {.6, .3, 0, 1},      {.55, .5, 0, 1},     {0, .4, .15, 1},
        {0, .2, .6, 1},     {.35, 0, .5, 1},     {.4, .25, .1, 1},
    };
    struct color_pick *c = g_new0(struct color_pick, 1);
    c->target = target, c->done = done, c->w = w;
    GtkWidget *pop = gtk_popover_new(relative);
    c->pop = pop;
    g_object_set_data_full(G_OBJECT(pop), "pick", c, g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(6));
    gtk_container_set_border_width(GTK_CONTAINER(box), S(8));
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), S(4));
    gtk_grid_set_column_spacing(GTK_GRID(g), S(4));
    for (guint i = 0; i < G_N_ELEMENTS(pal); i++) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, S(20), S(20));
        g_signal_connect(da, "draw", G_CALLBACK(swatch_draw), &pal[i]);
        gtk_container_add(GTK_CONTAINER(b), da);
        g_object_set_data(G_OBJECT(b), "rgba", &pal[i]);
        g_signal_connect(b, "clicked", G_CALLBACK(swatch_clicked), c);
        gtk_grid_attach(GTK_GRID(g), b, i % 7, i / 7, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    GtkWidget *more = gtk_button_new_with_label("Show Colors…");
    g_signal_connect(more, "clicked", G_CALLBACK(show_colors), c);
    gtk_box_pack_start(GTK_BOX(box), more, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_widget_show_all(box);
    return pop;
}
