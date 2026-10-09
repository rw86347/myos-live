/*
 * main.c — Preview: the application, its windows and its menus.
 *
 * The menu bar is Preview's, menu for menu and item for item: Preview,
 * File, Edit, View, Go, Bookmarks, Tools, Window, Help. Shortcuts map the
 * Mac's keys onto a PC keyboard: Command is Ctrl, Option is Alt, Control
 * is the Windows (Super) key. Items for services MyOS doesn't have
 * (Services, Share, importing from an iPhone or a scanner, colour profiles)
 * are there and greyed out, as on a Mac with none set up.
 *
 * Every menu item is a GAction ("win.save", "app.open"...), so a menu item,
 * its shortcut and a toolbar button are the same thing, and win_update()
 * turns them on and off together.
 *
 * A window: the toolbar (sidebar button, title, Info, Zoom, Share,
 * Highlight, Rotate, Markup, Search), the markup toolbar under it (shown by
 * the Markup button), and below them the sidebar and the pages.
 */
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <X11/Xatom.h>

#include "../myos-settings/scale.h"
#include "preview.h"

GtkApplication *app;
double px_per_pt = 1;
double k = 1;
struct prefs prefs;
static GList *windows;

const char *const toolbar_items[] = {"Sidebar", "Info", "Zoom", "Share", "Highlight", "Rotate", "Markup", "Search"};
const int n_toolbar_items = G_N_ELEMENTS(toolbar_items);
enum { TB_SIDEBAR, TB_INFO, TB_ZOOM, TB_SHARE, TB_HIGHLIGHT, TB_ROTATE, TB_MARKUP, TB_SEARCH, TB_N };

struct win_ui {
    GtkWidget *tb[TB_N], *tb_label[TB_N], *markup_toggle;
    GtkWidget *results_label, *results_list, *notes_list, *bookmarks_list, *toc_view;
    GtkWidget *tool_btn[T_STRIKE + 1];
    GtkWidget *bar_alpha, *bar_note, *bar_color, *bar_size, *bar_crop, *bar_form;
    GtkWidget *stroke_btn, *fill_btn;
    GPtrArray *built;           /* the pages the thumbnail list shows */
    gboolean syncing, toolbar_shown, searching;
    guint search_timer;
    GArray *back, *fwd;         /* Go > Back, Forward: pages */
};

/* --- settings and files ------------------------------------------------------------ */

char *config_path(const char *name)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "myos", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *p = g_build_filename(dir, name, NULL);
    g_free(dir);
    return p;
}

void prefs_load(void)
{
    prefs.background = (GdkRGBA){.78, .78, .80, 1};   /* Preview's page canvas */
    prefs.image_pixels_100 = TRUE;
    prefs.pdf_fit = TRUE;
    prefs.pdf_mode = DM_CONTINUOUS;
    prefs.smooth = TRUE;
    char *path = config_path("preview.conf");
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *bg = g_key_file_get_string(kf, "Preview", "background", NULL);
        if (bg)
            gdk_rgba_parse(&prefs.background, bg);
        g_free(bg);
        GError *e = NULL;
#define BOOL(key, field)                                                                           \
    do {                                                                                           \
        gboolean v = g_key_file_get_boolean(kf, "Preview", key, &e);                               \
        if (!e)                                                                                    \
            prefs.field = v;                                                                       \
        g_clear_error(&e);                                                                         \
    } while (0)
        BOOL("image-pixels-100", image_pixels_100);
        BOOL("pdf-fit", pdf_fit);
        BOOL("smooth", smooth);
        BOOL("toolbar-text", toolbar_text);
#undef BOOL
        int m = g_key_file_get_integer(kf, "Preview", "pdf-mode", NULL);
        prefs.pdf_mode = CLAMP(m, DM_CONTINUOUS, DM_TWO);
        prefs.toolbar_hidden = g_key_file_get_integer(kf, "Preview", "toolbar-hidden", NULL);
    }
    g_key_file_free(kf);
    g_free(path);
}

void prefs_save(void)
{
    GKeyFile *kf = g_key_file_new();
    char *bg = gdk_rgba_to_string(&prefs.background);
    g_key_file_set_string(kf, "Preview", "background", bg);
    g_free(bg);
    g_key_file_set_boolean(kf, "Preview", "image-pixels-100", prefs.image_pixels_100);
    g_key_file_set_boolean(kf, "Preview", "pdf-fit", prefs.pdf_fit);
    g_key_file_set_boolean(kf, "Preview", "smooth", prefs.smooth);
    g_key_file_set_boolean(kf, "Preview", "toolbar-text", prefs.toolbar_text);
    g_key_file_set_integer(kf, "Preview", "pdf-mode", prefs.pdf_mode);
    g_key_file_set_integer(kf, "Preview", "toolbar-hidden", (int)prefs.toolbar_hidden);
    char *path = config_path("preview.conf");
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

/* The desktop's scale, as myos-settings sets it (see scale.h): GTK doubles
   everything itself at 200%; below that we scale our own sizes by k. */
static void read_scale(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd))
        return;
    Display *d = GDK_DISPLAY_XDISPLAY(gd);
    Atom a_scale = XInternAtom(d, "_MYOS_SCALE", False), a_auto = XInternAtom(d, "_MYOS_SCALE_AUTO", False);
    long vals[2] = {-1, auto_scale(DisplayWidth(d, DefaultScreen(d)))};
    Atom atoms[2] = {a_scale, a_auto};
    for (int i = 0; i < 2; i++) {
        Atom type;
        int fmt;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d, DefaultRootWindow(d), atoms[i], 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after,
                               &data) == Success && data && n == 1 && fmt == 32)
            vals[i] = *(long *)data;
        if (data)
            XFree(data);
    }
    int pct = vals[0] < 0 ? scale_read_conf() : vals[0] ? clamp_scale(vals[0]) : 0;
    if (!pct)
        pct = vals[1];
    k = pct >= 200 ? pct / 200.0 : pct / 100.0;
    px_per_pt = k;          /* 100%: a point is a point on screen, as on the Mac */
}

/* --- small helpers ---------------------------------------------------------------- */

GList *win_list(void)
{
    return windows;
}

struct win *win_of(GtkWidget *widget)
{
    GtkWidget *top = gtk_widget_get_toplevel(widget);
    for (GList *l = windows; l; l = l->next)
        if (GTK_WIDGET(((struct win *)l->data)->window) == top)
            return l->data;
    return NULL;
}

static struct win *active_win(void)
{
    GtkWindow *a = gtk_application_get_active_window(app);
    for (GList *l = windows; l; l = l->next)
        if (GTK_WINDOW(((struct win *)l->data)->window) == a)
            return l->data;
    return windows ? windows->data : NULL;
}

void win_error(struct win *w, const char *what, GError *err)
{
    GtkWidget *m = gtk_message_dialog_new(w ? GTK_WINDOW(w->window) : NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_OK, "%s", what);
    if (err)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", err->message);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

static void enable(struct win *w, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w->window), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void set_state(struct win *w, const char *name, GVariant *v)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w->window), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
    else
        g_variant_unref(g_variant_ref_sink(v));
}

/* The text field being typed in, if any: Copy, Paste, Select All and so on
   go to it then, not to the document. */
static GtkWidget *focus_text(struct win *w)
{
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(w->window));
    return f && (GTK_IS_EDITABLE(f) || GTK_IS_TEXT_VIEW(f)) ? f : NULL;
}

static gboolean is_pdf(struct win *w)
{
    return w->doc->kind == DOC_PDF;
}

/* Pages the action is for: those selected in the sidebar, else the current one. */
static GArray *target_pages(struct win *w)
{
    GArray *a = g_array_new(FALSE, FALSE, sizeof(int));
    if (w->sidebar == SB_THUMBS && w->thumbs) {
        GList *rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(w->thumbs));
        for (GList *l = rows; l; l = l->next) {
            int i = gtk_list_box_row_get_index(l->data);
            g_array_append_val(a, i);
        }
        g_list_free(rows);
    }
    if (a->len <= 1) {
        g_array_set_size(a, 0);
        g_array_append_val(a, w->page);
    }
    return a;
}

/* --- page edits ---------------------------------------------------------------------- */

static void remove_marks_of(struct state *st, unsigned id)
{
    for (guint j = st->marks->len; j-- > 0;)
        if (((struct mark *)g_ptr_array_index(st->marks, j))->page == id)
            g_ptr_array_remove_index(st->marks, j);
}

/* Rotate the pages: a PDF page just records the turn (its marks turn with
   it); a picture's pixels turn, with its marks painted in first. */
static void rotate_pages(struct win *w, int deg)
{
    struct doc *d = w->doc;
    GArray *pages = target_pages(w);
    struct state *st = state_copy(d->st);
    for (guint n = 0; n < pages->len; n++) {
        int i = g_array_index(pages, int, n);
        struct page *p = page_copy(PAGE(d, i));
        if (p->pdf) {
            p->rotation = (p->rotation + deg + 360) % 360;
        } else {
            GdkPixbuf *flat = marks_flatten(d->st->marks, PAGE(d, i));
            g_object_unref(p->pix);
            p->pix = img_rotate(flat, deg);
            g_object_unref(flat);
            p->w = gdk_pixbuf_get_width(p->pix), p->h = gdk_pixbuf_get_height(p->pix);
            remove_marks_of(st, p->id);
        }
        g_ptr_array_remove_index(st->pages, i);
        g_ptr_array_insert(st->pages, i, p);
    }
    g_array_unref(pages);
    doc_commit(d, st);
    view_deselect(w->view);
    win_doc_changed(w);
}

static void delete_pages(struct win *w, GArray *pages)
{
    struct doc *d = w->doc;
    if ((int)pages->len >= NPAGES(d))
        return;                 /* a document keeps at least one page */
    struct state *st = state_copy(d->st);
    for (guint n = pages->len; n-- > 0;) {
        int i = g_array_index(pages, int, n);
        remove_marks_of(st, PAGE(d, i)->id);
    }
    /* Remove from the end so the indices stay right. */
    gboolean *gone = g_new0(gboolean, NPAGES(d));
    for (guint n = 0; n < pages->len; n++)
        gone[g_array_index(pages, int, n)] = TRUE;
    for (int i = NPAGES(d); i-- > 0;)
        if (gone[i])
            g_ptr_array_remove_index(st->pages, i);
    g_free(gone);
    doc_commit(d, st);
    w->page = MIN(w->page, (int)st->pages->len - 1);
    win_doc_changed(w);
}

/* Move the pages `pages` to before index `to` (in the old numbering). */
static void move_pages(struct win *w, GArray *pages, int to)
{
    struct doc *d = w->doc;
    int n = NPAGES(d);
    gboolean *moving = g_new0(gboolean, n);
    for (guint i = 0; i < pages->len; i++)
        moving[g_array_index(pages, int, i)] = TRUE;
    struct state *st = state_copy(d->st);
    g_ptr_array_set_size(st->pages, 0);
    int first = -1;
    for (int i = 0; i <= n; i++) {
        if (i == to)
            for (int j = 0; j < n; j++)
                if (moving[j]) {
                    if (first < 0)
                        first = st->pages->len;
                    g_ptr_array_add(st->pages, page_ref(PAGE(d, j)));
                }
        if (i < n && !moving[i])
            g_ptr_array_add(st->pages, page_ref(PAGE(d, i)));
    }
    g_free(moving);
    doc_commit(d, st);
    w->page = MAX(first, 0);
    win_doc_changed(w);
}

/* Insert the pages of `path` after page `after`. */
static void insert_file(struct win *w, const char *path, int after)
{
    struct doc *d = w->doc;
    GError *err = NULL;
    struct doc *src = doc_open(path, NULL, &err);
    if (!src) {
        win_error(w, "The file couldn't be inserted.", err);
        g_clear_error(&err);
        return;
    }
    struct state *st = state_copy(d->st);
    int at = after + 1;
    for (int i = 0; i < NPAGES(src); i++) {
        struct page *p = PAGE(src, i);
        struct page *n;
        if (d->kind == DOC_PDF && !p->pdf)
            n = page_pdf_from_pixbuf(p->pix, src->dpi);    /* a picture becomes a PDF page */
        else if (d->kind == DOC_IMAGE && p->pdf)
            n = NULL;                                       /* (pictures take pictures only) */
        else
            n = page_ref(p);
        if (n) {
            n->id = page_new_id();
            g_ptr_array_insert(st->pages, at++, n);
        }
    }
    doc_free(src);
    doc_commit(d, st);
    w->page = after + 1;
    win_doc_changed(w);
}

/* Clear the selected area of a picture (or everything but it). */
static void clear_area(struct win *w)
{
    int pg, shape;
    double r[4];
    GArray *lasso;
    if (!view_area(w->view, &pg, r, &shape, &lasso) || is_pdf(w))
        return;
    GdkPixbuf *src = doc_page_pixels(w->doc, pg);
    int pw = gdk_pixbuf_get_width(src), ph = gdk_pixbuf_get_height(src);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t *cr = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    if (shape & 4)
        cairo_rectangle(cr, 0, 0, pw, ph);
    if ((shape & 3) == 2 && lasso && lasso->len >= 6) {
        double *p = (double *)lasso->data;
        cairo_move_to(cr, p[0], p[1]);
        for (guint i = 2; i < lasso->len; i += 2)
            cairo_line_to(cr, p[i], p[i + 1]);
        cairo_close_path(cr);
    } else if ((shape & 3) == 1) {
        cairo_save(cr);
        cairo_translate(cr, r[0] + r[2] / 2, r[1] + r[3] / 2);
        cairo_scale(cr, MAX(r[2], 1e-3) / 2, MAX(r[3], 1e-3) / 2);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
    } else {
        cairo_rectangle(cr, r[0], r[1], r[2], r[3]);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_fill(cr);
    cairo_destroy(cr);
    GdkPixbuf *out = gdk_pixbuf_get_from_surface(s, 0, 0, pw, ph);
    cairo_surface_destroy(s);
    doc_set_pixels(w->doc, pg, out);
    g_object_unref(out), g_object_unref(src);
    view_deselect(w->view);
    win_doc_changed(w);
}

/* --- the sidebar ------------------------------------------------------------------- */

static gboolean thumb_draw(GtkWidget *da, cairo_t *cr, gpointer data)
{
    struct win *w = data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da), "index"));
    if (i >= NPAGES(w->doc))
        return TRUE;
    int aw = gtk_widget_get_allocated_width(da), ah = gtk_widget_get_allocated_height(da);
    gboolean pending;
    struct page *p = PAGE(w->doc, i);
    cairo_surface_t *s = thumb_get(w, p, MIN(aw, 96), &pending);
    double pw, ph;
    page_size(p, &pw, &ph);
    double sc = MIN((aw - 8) / pw, (ah - 8) / ph);
    double tw = pw * sc, th = ph * sc, x = (aw - tw) / 2, y = (ah - th) / 2;
    /* Selected page: the blue rounded plate Preview uses, not GTK's row wash. */
    if (i == w->page) {
        double rx = x - 4, ry = y - 4, rw = tw + 8, rh = th + 8;
        cairo_new_sub_path(cr);
        cairo_arc(cr, rx + rw - 6, ry + 6, 6, -G_PI_2, 0);
        cairo_arc(cr, rx + rw - 6, ry + rh - 6, 6, 0, G_PI_2);
        cairo_arc(cr, rx + 6, ry + rh - 6, 6, G_PI_2, G_PI);
        cairo_arc(cr, rx + 6, ry + 6, 6, G_PI, 3 * G_PI_2);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.0, 0.48, 1.0);
        cairo_fill(cr);
    }
    cairo_rectangle(cr, x + 1, y + 2, tw, th);
    cairo_set_source_rgba(cr, 0, 0, 0, .18);
    cairo_fill(cr);
    cairo_rectangle(cr, x, y, tw, th);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    if (s) {
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, tw / cairo_image_surface_get_width(s), th / cairo_image_surface_get_height(s));
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    return TRUE;
}

static GtkWidget *thumb_widget(struct win *w, int i, int size)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(3));
    g_object_set(box, "margin", S(6), NULL);
    GtkWidget *da = gtk_drawing_area_new();
    double pw, ph;
    page_size(PAGE(w->doc, i), &pw, &ph);
    double s = size / MAX(pw, ph);
    gtk_widget_set_size_request(da, MAX(8, (int)(pw * s)), MAX(8, (int)(ph * s)));
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(da), "index", GINT_TO_POINTER(i));
    g_signal_connect(da, "draw", G_CALLBACK(thumb_draw), w);
    char num[16];
    g_snprintf(num, sizeof num, "%d", i + 1);
    gtk_box_pack_start(GTK_BOX(box), da, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(num), FALSE, FALSE, 0);
    return box;
}

static const GtkTargetEntry page_target = {"application/x-preview-page", GTK_TARGET_SAME_APP, 1};
static const GtkTargetEntry uri_target = {"text/uri-list", 0, 2};

static void thumbs_drag_get(GtkWidget *row, GdkDragContext *c, GtkSelectionData *sd, guint info, guint t, gpointer data)
{
    (void)row, (void)c, (void)info, (void)t, (void)data;
    gtk_selection_data_set(sd, gdk_atom_intern_static_string(page_target.target), 8, (const guchar *)"p", 1);
}

static void thumbs_drag_begin(GtkWidget *row, GdkDragContext *c, gpointer data)
{
    (void)data;
    /* Dragging a row that isn't selected drags just it. */
    GtkListBox *lb = GTK_LIST_BOX(gtk_widget_get_parent(row));
    if (!gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(row))) {
        gtk_list_box_unselect_all(lb);
        gtk_list_box_select_row(lb, GTK_LIST_BOX_ROW(row));
    }
    gtk_drag_set_icon_default(c);
}

static void thumbs_drop(GtkWidget *lb, GdkDragContext *c, int x, int y, GtkSelectionData *sd, guint info, guint t,
                        gpointer data)
{
    (void)x;
    struct win *w = data;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(lb), y);
    int to = NPAGES(w->doc);
    if (row) {
        GtkAllocation a;
        gtk_widget_get_allocation(GTK_WIDGET(row), &a);
        to = gtk_list_box_row_get_index(row) + (y > a.y + a.height / 2 ? 1 : 0);
    }
    if (info == 1) {
        GArray *pages = target_pages(w);
        move_pages(w, pages, to);
        g_array_unref(pages);
    } else {
        char **uris = gtk_selection_data_get_uris(sd);
        for (int i = uris ? g_strv_length(uris) : 0; i-- > 0;) {
            char *f = g_filename_from_uri(uris[i], NULL, NULL);
            if (f)
                insert_file(w, f, to - 1);
            g_free(f);
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(c, TRUE, FALSE, t);
}

static void thumbs_rebuild(struct win *w)
{
    struct doc *d = w->doc;
    struct win_ui *u = w->ui;
    /* Only when the pages changed: a mark doesn't change the list. */
    gboolean same = u->built && (int)u->built->len == NPAGES(d);
    for (int i = 0; same && i < NPAGES(d); i++)
        same = g_ptr_array_index(u->built, i) == PAGE(d, i);
    if (same)
        return;
    if (u->built)
        g_ptr_array_unref(u->built);
    u->built = g_ptr_array_new();
    for (int i = 0; i < NPAGES(d); i++)
        g_ptr_array_add(u->built, PAGE(d, i));
    u->syncing = TRUE;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w->thumbs));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    for (int i = 0; i < NPAGES(d); i++) {
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), thumb_widget(w, i, S(110)));
        gtk_drag_source_set(row, GDK_BUTTON1_MASK, &page_target, 1, GDK_ACTION_MOVE);
        g_signal_connect(row, "drag-data-get", G_CALLBACK(thumbs_drag_get), w);
        g_signal_connect(row, "drag-begin", G_CALLBACK(thumbs_drag_begin), w);
        gtk_list_box_insert(GTK_LIST_BOX(w->thumbs), row, -1);
    }
    gtk_widget_show_all(w->thumbs);
    thumbs_fill_now(w, MAX(0, w->page - 1), 5);
    /* The contact sheet: the same pages, bigger, in a grid. */
    kids = gtk_container_get_children(GTK_CONTAINER(w->contact));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    for (int i = 0; i < NPAGES(d); i++)
        gtk_flow_box_insert(GTK_FLOW_BOX(w->contact), thumb_widget(w, i, S(170)), -1);
    gtk_widget_show_all(w->contact);
    u->syncing = FALSE;
}

static void thumbs_selected(GtkListBox *lb, gpointer data)
{
    struct win *w = data;
    if (w->ui->syncing)
        return;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(lb);
    if (row)
        win_set_page(w, gtk_list_box_row_get_index(row));
}

static gboolean thumbs_key(GtkWidget *wd, GdkEventKey *e, gpointer data)
{
    (void)wd;
    struct win *w = data;
    if (e->keyval == GDK_KEY_Delete || e->keyval == GDK_KEY_BackSpace) {
        GArray *pages = target_pages(w);
        delete_pages(w, pages);
        g_array_unref(pages);
        return TRUE;
    }
    return FALSE;
}

static void contact_activated(GtkFlowBox *fb, GtkFlowBoxChild *c, gpointer data)
{
    (void)fb;
    struct win *w = data;
    win_set_page(w, gtk_flow_box_child_get_index(c));
    win_set_sidebar(w, w->last_sidebar == SB_CONTACT ? SB_THUMBS : w->last_sidebar);
}

/* Table of contents: the PDF's outline. */
enum { TOC_TITLE, TOC_ACTION, TOC_N };

static void toc_fill(GtkTreeStore *ts, GtkTreeIter *parent, PopplerIndexIter *it)
{
    do {
        PopplerAction *a = poppler_index_iter_get_action(it);
        GtkTreeIter row;
        gtk_tree_store_append(ts, &row, parent);
        gtk_tree_store_set(ts, &row, TOC_TITLE, a && a->any.title ? a->any.title : "", TOC_ACTION, a, -1);
        PopplerIndexIter *child = poppler_index_iter_get_child(it);
        if (child) {
            toc_fill(ts, &row, child);
            poppler_index_iter_free(child);
        }
        if (a)
            poppler_action_free(a);
    } while (poppler_index_iter_next(it));
}

static void toc_rebuild(struct win *w)
{
    GtkTreeStore *ts = gtk_tree_store_new(TOC_N, G_TYPE_STRING, POPPLER_TYPE_ACTION);
    PopplerIndexIter *it = w->doc->pdf ? poppler_index_iter_new(w->doc->pdf) : NULL;
    if (it) {
        toc_fill(ts, NULL, it);
        poppler_index_iter_free(it);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(w->ui->toc_view), GTK_TREE_MODEL(ts));
    g_object_unref(ts);
}

static void toc_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    struct win *w = data;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it;
    PopplerAction *a = NULL;
    if (gtk_tree_model_get_iter(m, &it, path))
        gtk_tree_model_get(m, &it, TOC_ACTION, &a, -1);
    if (a && a->type == POPPLER_ACTION_GOTO_DEST)
        view_go_dest(w->view, a->goto_dest.dest);
    if (a)
        poppler_action_free(a);
}

static void toc_selected(GtkTreeSelection *sel, gpointer data)
{
    struct win *w = data;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it))
        return;
    GtkTreePath *p = gtk_tree_model_get_path(m, &it);
    toc_activated(gtk_tree_selection_get_tree_view(sel), p, NULL, w);
    gtk_tree_path_free(p);
}

static void list_clear(GtkWidget *lb)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(lb));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
}

static GtkWidget *two_line_row(const char *top, const char *bottom)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(2));
    g_object_set(box, "margin", S(6), NULL);
    GtkWidget *a = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", top);
    gtk_label_set_markup(GTK_LABEL(a), mk);
    g_free(mk);
    gtk_widget_set_halign(a, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), a, FALSE, FALSE, 0);
    if (bottom && *bottom) {
        GtkWidget *b = gtk_label_new(bottom);
        gtk_label_set_line_wrap(GTK_LABEL(b), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(b), 28);
        gtk_label_set_lines(GTK_LABEL(b), 3);
        gtk_label_set_ellipsize(GTK_LABEL(b), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(b), 0);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

static int page_of_id(struct doc *d, unsigned id)
{
    for (int i = 0; i < NPAGES(d); i++)
        if (PAGE(d, i)->id == id)
            return i;
    return -1;
}

/* Highlights and Notes: the marks that say something. */
static void notes_rebuild(struct win *w)
{
    struct doc *d = w->doc;
    list_clear(w->ui->notes_list);
    for (guint k = 0; k < d->st->marks->len; k++) {
        struct mark *m = MARK(d, k);
        if (m->kind != MK_HIGHLIGHT && m->kind != MK_UNDERLINE && m->kind != MK_STRIKE && m->kind != MK_NOTE &&
            m->kind != MK_TEXT && m->kind != MK_BUBBLE)
            continue;
        char *top = g_strdup_printf("Page %d · %s", page_of_id(d, m->page) + 1, mark_name(m->kind));
        GtkWidget *row = two_line_row(top, m->text);
        g_free(top);
        g_object_set_data(G_OBJECT(row), "mark", m);
        gtk_list_box_insert(GTK_LIST_BOX(w->ui->notes_list), row, -1);
    }
    gtk_widget_show_all(w->ui->notes_list);
}

static void notes_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    struct win *w = data;
    struct mark *m = g_object_get_data(G_OBJECT(row), "mark");
    if (m && g_ptr_array_find(w->doc->st->marks, m, NULL))
        view_select_mark(w->view, m);
}

static void bookmarks_rebuild(struct win *w)
{
    list_clear(w->ui->bookmarks_list);
    GPtrArray *b = bookmarks_get(w->doc->path);
    for (guint i = 0; i < b->len; i++) {
        struct bookmark *bm = g_ptr_array_index(b, i);
        char *pg = g_strdup_printf("Page %d", bm->page + 1);
        GtkWidget *row = two_line_row(bm->name, pg);
        g_free(pg);
        g_object_set_data(G_OBJECT(row), "page", GINT_TO_POINTER(bm->page));
        gtk_list_box_insert(GTK_LIST_BOX(w->ui->bookmarks_list), row, -1);
    }
    if (!b->len) {
        GtkWidget *l = gtk_label_new("No Bookmarks\n\nChoose Bookmarks ▸ Add Bookmark.");
        gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
        g_object_set(l, "margin", S(16), NULL);
        gtk_list_box_insert(GTK_LIST_BOX(w->ui->bookmarks_list), l, -1);
    }
    g_ptr_array_unref(b);
    gtk_widget_show_all(w->ui->bookmarks_list);
}

static void bookmark_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    struct win *w = data;
    if (GTK_IS_LIST_BOX_ROW(row) && gtk_bin_get_child(GTK_BIN(row)) && !GTK_IS_LABEL(gtk_bin_get_child(GTK_BIN(row))))
        win_set_page(w, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "page")));
}

static gboolean bookmarks_key(GtkWidget *lb, GdkEventKey *e, gpointer data)
{
    struct win *w = data;
    if (e->keyval != GDK_KEY_Delete && e->keyval != GDK_KEY_BackSpace)
        return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(lb));
    if (row && w->doc->path) {
        bookmark_remove(w->doc->path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "page")));
        bookmarks_rebuild(w);
        win_update(w);
    }
    return TRUE;
}

static void results_rebuild(struct win *w)
{
    struct win_ui *u = w->ui;
    list_clear(u->results_list);
    GList *hits = view_search_results(w->view);
    int i = 0;
    for (GList *l = hits; l; l = l->next, i++) {
        struct hit *h = l->data;
        char *pg = g_strdup_printf("Page %d", h->page + 1);
        GtkWidget *row = two_line_row(pg, h->context);
        g_free(pg);
        g_object_set_data(G_OBJECT(row), "hit", GINT_TO_POINTER(i));
        gtk_list_box_insert(GTK_LIST_BOX(u->results_list), row, -1);
        if (i >= 1000)
            break;              /* a list longer than that helps nobody */
    }
    g_list_free(hits);
    int n = view_search_count(w->view);
    char *t = n ? g_strdup_printf(n == 1 ? "1 match" : "%d matches", n) : g_strdup("Not found");
    gtk_label_set_text(GTK_LABEL(u->results_label), t);
    g_free(t);
    gtk_widget_show_all(u->results_list);
}

static void result_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data)
{
    (void)lb;
    struct win *w = data;
    view_search_select(w->view, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hit")));
}

void win_set_sidebar(struct win *w, enum sidebar_mode m)
{
    if (m != SB_CONTACT && m != SB_NONE)
        w->last_sidebar = m;
    else if (w->sidebar != SB_CONTACT && w->sidebar != SB_NONE)
        w->last_sidebar = w->sidebar;
    w->sidebar = m;
    gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), m == SB_CONTACT ? "contact" : "pages");
    gboolean show = m != SB_NONE && m != SB_CONTACT;
    gtk_widget_set_visible(w->sidebar_box, show || w->ui->searching);
    static const char *const names[] = {"", "thumbs", "toc", "notes", "bookmarks", ""};
    if (w->ui->searching)
        gtk_stack_set_visible_child_name(GTK_STACK(w->sidebar_stack), "results");
    else if (show)
        gtk_stack_set_visible_child_name(GTK_STACK(w->sidebar_stack), names[m]);
    if (m == SB_TOC)
        toc_rebuild(w);
    if (m == SB_NOTES)
        notes_rebuild(w);
    if (m == SB_BOOKMARKS)
        bookmarks_rebuild(w);
    win_update(w);
}

/* --- search ------------------------------------------------------------------------ */

static gboolean search_now(gpointer data)
{
    struct win *w = data;
    w->ui->search_timer = 0;
    const char *t = gtk_entry_get_text(GTK_ENTRY(w->search));
    w->ui->searching = *t != 0;
    if (*t)
        view_search(w->view, t, FALSE, FALSE);
    else
        view_search_clear(w->view);
    results_rebuild(w);
    win_set_sidebar(w, w->sidebar);
    return G_SOURCE_REMOVE;
}

static void search_changed(GtkSearchEntry *e, gpointer data)
{
    (void)e;
    struct win *w = data;
    if (w->ui->search_timer)
        g_source_remove(w->ui->search_timer);
    /* Wait for a pause in the typing: every search reads every page. */
    w->ui->search_timer = g_timeout_add(300, search_now, w);
}

static void search_activate(GtkEntry *e, gpointer data)
{
    (void)e;
    struct win *w = data;
    if (w->ui->search_timer) {
        g_source_remove(w->ui->search_timer);
        search_now(w);
        return;
    }
    GdkModifierType mods = 0;
    gtk_get_current_event_state(&mods);
    view_search_next(w->view, mods & GDK_SHIFT_MASK ? -1 : 1);
}

static void search_stop(GtkSearchEntry *e, gpointer data)
{
    struct win *w = data;
    gtk_entry_set_text(GTK_ENTRY(e), "");
    search_now(w);
    gtk_widget_grab_focus(view_widget(w->view));
}

/* --- keeping the window up to date --------------------------------------------------- */

static GMenu *m_window_list, *m_bookmarks_list, *m_recent_list;
static GMenu *m_toolbar_sec, *m_markup_sec, *m_tools_sec, *m_fullscreen_sec;

/* A menu item whose words change ("Show Toolbar" / "Hide Toolbar"), as the
   Mac's do: put a new item in its place. */
static void relabel(GMenu *sec, int index, const char *label, const char *action, const char *accel)
{
    GMenuItem *it = g_menu_item_new(label, action);
    if (accel)
        g_menu_item_set_attribute(it, "accel", "s", accel);
    g_menu_remove(sec, index);
    g_menu_insert_item(sec, index, it);
    g_object_unref(it);
}

static void menus_for(struct win *w)
{
    static int last[5] = {-1, -1, -1, -1, -1};
    int now[5] = {w->ui->toolbar_shown, gtk_revealer_get_reveal_child(GTK_REVEALER(w->markup_revealer)),
                  view_magnifier(w->view), w->inspector != NULL, w->fullscreen};
    if (now[0] != last[0])
        relabel(m_toolbar_sec, 0, now[0] ? "Hide Toolbar" : "Show Toolbar", "win.toolbar", "<Alt><Primary>t");
    if (now[1] != last[1])
        relabel(m_markup_sec, 0, now[1] ? "Hide Markup Toolbar" : "Show Markup Toolbar", "win.markup-bar",
                "<Shift><Primary>a");
    if (now[3] != last[3])
        relabel(m_tools_sec, 0, now[3] ? "Hide Inspector" : "Show Inspector", "win.inspector", "<Primary>i");
    if (now[2] != last[2])
        relabel(m_tools_sec, 1, now[2] ? "Hide Magnifier" : "Show Magnifier", "win.magnifier", "grave");
    if (now[4] != last[4])
        relabel(m_fullscreen_sec, 1, now[4] ? "Exit Full Screen" : "Enter Full Screen", "win.fullscreen", "F11");
    memcpy(last, now, sizeof last);
}

static void bookmarks_menu(struct win *w)
{
    g_menu_remove_all(m_bookmarks_list);
    GPtrArray *b = bookmarks_get(w ? w->doc->path : NULL);
    for (guint i = 0; i < b->len; i++) {
        struct bookmark *bm = g_ptr_array_index(b, i);
        GMenuItem *it = g_menu_item_new(bm->name, NULL);
        g_menu_item_set_action_and_target_value(it, "win.go-bookmark", g_variant_new_int32(bm->page));
        g_menu_append_item(m_bookmarks_list, it);
        g_object_unref(it);
    }
    g_ptr_array_unref(b);
}

static void window_menu(void)
{
    g_menu_remove_all(m_window_list);
    int i = 0;
    for (GList *l = windows; l; l = l->next, i++) {
        struct win *w = l->data;
        GMenuItem *it = g_menu_item_new(gtk_window_get_title(GTK_WINDOW(w->window)), NULL);
        g_menu_item_set_action_and_target_value(it, "app.window", g_variant_new_int32(i));
        g_menu_append_item(m_window_list, it);
        g_object_unref(it);
    }
}

static const char *const readable_exts[] = {
    "pdf", "png", "jpg", "jpeg", "jpe", "heic", "heif", "avif", "jxl", "webp", "tif", "tiff", "gif", "bmp",
    "ico", "icns", "tga", "jp2", "j2k", "jpf", "jpx", "exr", "psd", "svg", "svgz", "pnm", "ppm", "pgm", "pbm",
    "xbm", "xpm", "cr2", "cr3", "nef", "arw", "dng", "orf", "rw2", "raf", "pef", "srw", NULL,
};

static gboolean readable(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return FALSE;
    char *e = g_ascii_strdown(dot + 1, -1);
    gboolean ok = g_strv_contains(readable_exts, e);
    g_free(e);
    return ok;
}

static void recent_menu(void)
{
    g_menu_remove_all(m_recent_list);
    GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
    int n = 0;
    for (GList *l = items; l && n < 10; l = l->next) {
        GtkRecentInfo *ri = l->data;
        char *path = g_filename_from_uri(gtk_recent_info_get_uri(ri), NULL, NULL);
        if (path && readable(path) && g_file_test(path, G_FILE_TEST_EXISTS)) {
            char *name = g_filename_display_basename(path);
            GMenuItem *it = g_menu_item_new(name, NULL);
            g_menu_item_set_action_and_target_value(it, "app.open-recent", g_variant_new_string(path));
            g_menu_append_item(m_recent_list, it);
            g_object_unref(it);
            g_free(name);
            n++;
        }
        g_free(path);
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
}

void win_update(struct win *w)
{
    struct doc *d = w->doc;
    struct win_ui *u = w->ui;
    if (!u)
        return;
    int n = NPAGES(d);
    gboolean pdf = is_pdf(w), img = !pdf;
    gboolean area = view_has_area(w->view), sel = view_has_selection(w->view), mark = view_has_mark(w->view);

    char *title = g_strdup_printf("%s%s", d->name, d->edits ? " — Edited" : "");
    gtk_window_set_title(GTK_WINDOW(w->window), title);
    char *mk = g_markup_printf_escaped("<b>%s</b>", d->name);
    gtk_label_set_markup(GTK_LABEL(w->title), mk);
    g_free(mk);
    g_free(title);
    char *sub = n > 1 ? g_strdup_printf("Page %d of %d", w->page + 1, n)
              : d->edits ? g_strdup("Edited")
              : img ? g_strdup_printf("%.0f × %.0f", PAGE(d, 0)->w, PAGE(d, 0)->h) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(w->subtitle), sub);
    g_free(sub);

    /* File */
    enable(w, "save", d->edits || !d->path);
    enable(w, "rename", d->path != NULL);
    enable(w, "move-to", d->path != NULL);
    enable(w, "revert", d->path && d->edits);
    enable(w, "enter-password", pdf && poppler_document_get_permissions(d->pdf) != POPPLER_PERMISSIONS_FULL);
    /* Edit */
    enable(w, "undo", d->undo->len > 0);
    enable(w, "redo", d->redo->len > 0);
    enable(w, "copy", sel || mark || focus_text(w));
    enable(w, "cut", (area && img) || mark || focus_text(w));
    enable(w, "delete", (area && img) || mark || (w->sidebar == SB_THUMBS && n > 1));
    enable(w, "deselect", sel || mark);
    enable(w, "invert-selection", area && img);
    enable(w, "insert-file", pdf);
    enable(w, "insert-blank", pdf);
    enable(w, "find", pdf);
    enable(w, "find-next", view_search_count(w->view) > 0);
    enable(w, "find-prev", view_search_count(w->view) > 0);
    enable(w, "use-selection", sel && pdf);
    enable(w, "jump-selection", sel);
    /* View */
    enable(w, "zoom-selection", area);
    set_state(w, "sidebar", g_variant_new_string((const char *[]){"none", "thumbs", "toc", "notes", "bookmarks",
                                                                  "contact"}[w->sidebar]));
    set_state(w, "display", g_variant_new_string((const char *[]){"continuous", "single", "two"}[view_mode(w->view)]));
    enable(w, "sidebar", TRUE);
    /* Go */
    enable(w, "prev-page", w->page > 0);
    enable(w, "next-page", w->page + 1 < n);
    enable(w, "first-page", w->page > 0);
    enable(w, "last-page", w->page + 1 < n);
    enable(w, "back", u->back->len > 0);
    enable(w, "forward", u->fwd->len > 0);
    enable(w, "go-to-page", n > 1);
    enable(w, "add-bookmark", d->path != NULL);
    /* Tools */
    enable(w, "adjust-color", img);
    enable(w, "adjust-size", img);
    enable(w, "crop", area && img);
    enable(w, "flip-h", img);
    enable(w, "flip-v", img);
    enable(w, "highlight", pdf);
    enable(w, "underline", pdf);
    enable(w, "strike", pdf);
    enable(w, "note", pdf);
    enable(w, "tool-text", pdf);

    gtk_widget_set_visible(u->bar_alpha, img);
    gtk_widget_set_visible(u->bar_color, img);
    gtk_widget_set_visible(u->bar_size, img);
    gtk_widget_set_visible(u->bar_crop, img && area);
    gtk_widget_set_visible(u->bar_note, pdf);
    gtk_widget_set_sensitive(u->tb[TB_HIGHLIGHT], pdf);
    gtk_widget_set_sensitive(u->tb[TB_SEARCH], pdf);

    /* The sidebar follows the current page: one selected row, always. */
    if (w->sidebar == SB_THUMBS && !u->syncing) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(w->thumbs), w->page);
        if (row && !gtk_list_box_row_is_selected(row)) {
            u->syncing = TRUE;
            gtk_list_box_unselect_all(GTK_LIST_BOX(w->thumbs));
            gtk_list_box_select_row(GTK_LIST_BOX(w->thumbs), row);
            gtk_widget_queue_draw(w->thumbs);
            u->syncing = FALSE;
        }
    }
    if (GTK_WINDOW(w->window) == gtk_application_get_active_window(app))
        menus_for(w);
}

void win_doc_changed(struct win *w)
{
    view_reload(w->view);
    thumbs_rebuild(w);
    if (w->sidebar == SB_NOTES)
        notes_rebuild(w);
    if (w->ui->searching)
        search_now(w);
    win_update(w);
}

void win_set_page(struct win *w, int page)
{
    page = CLAMP(page, 0, NPAGES(w->doc) - 1);
    if (page != w->page) {
        g_array_append_val(w->ui->back, w->page);
        g_array_set_size(w->ui->fwd, 0);
        w->page = page;
        if (w->thumbs)
            gtk_widget_queue_draw(w->thumbs);
    }
    view_go_page(w->view, page);
    win_update(w);
}

void win_set_tool(struct win *w, enum tool t)
{
    view_set_tool(w->view, t);
    struct win_ui *u = w->ui;
    u->syncing = TRUE;
    for (int i = 0; i <= T_STRIKE; i++)
        if (u->tool_btn[i])
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u->tool_btn[i]), i == (int)t);
    u->syncing = FALSE;
}

void win_apply_toolbar(struct win *w)
{
    struct win_ui *u = w->ui;
    for (int i = 0; i < TB_N; i++) {
        gtk_widget_set_visible(u->tb[i], !(prefs.toolbar_hidden & (1u << i)));
        if (u->tb_label[i])
            gtk_widget_set_visible(u->tb_label[i], prefs.toolbar_text);
    }
}

/* --- actions: Preview and File -------------------------------------------------------- */

#define WIN(name) static void name(GSimpleAction *a, GVariant *p, gpointer data)
#define UNUSED (void)a, (void)p
#define W struct win *w = data

static void a_about(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    struct win *w = active_win();
    dlg_about(w ? GTK_WINDOW(w->window) : NULL);
}

static void a_settings(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    struct win *w = active_win();
    dlg_settings(w ? GTK_WINDOW(w->window) : NULL);
}

static void a_hide(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    for (GList *l = windows; l; l = l->next)
        gtk_window_iconify(GTK_WINDOW(((struct win *)l->data)->window));
}

static void a_show_all(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    for (GList *l = windows; l; l = l->next)
        gtk_window_present(GTK_WINDOW(((struct win *)l->data)->window));
}

static void win_close(struct win *w);

static void a_quit(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    GList *copy = g_list_copy(windows);
    for (GList *l = copy; l; l = l->next) {
        struct win *w = l->data;
        if (!dlg_close_changed(w))
            break;              /* Cancel stops the quitting */
        w->doc->edits = 0;
        win_close(w);
    }
    g_list_free(copy);
}

static GtkFileFilter *filter(const char *name, const char *const *exts)
{
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, name);
    for (; *exts; exts++) {
        char *pat = g_strdup_printf("*.%s", *exts), *up = g_ascii_strup(pat, -1);
        gtk_file_filter_add_pattern(f, pat);
        gtk_file_filter_add_pattern(f, up);
        g_free(pat), g_free(up);
    }
    return f;
}

static void a_open(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    struct win *w = active_win();
    GtkWidget *c = gtk_file_chooser_dialog_new("Open", w ? GTK_WINDOW(w->window) : NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                               "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_OK, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(c), TRUE);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(c), filter("All Readable Documents", readable_exts));
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(c), filter("PDF", (const char *const[]){"pdf", NULL}));
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(c), filter("Images", readable_exts + 1));
    if (w && w->doc->path) {
        char *dir = g_path_get_dirname(w->doc->path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(c), dir);
        g_free(dir);
    }
    if (gtk_dialog_run(GTK_DIALOG(c)) == GTK_RESPONSE_OK) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(c));
        gtk_widget_destroy(c);
        for (GSList *l = files; l; l = l->next)
            win_open_file(l->data);
        g_slist_free_full(files, g_free);
        return;
    }
    gtk_widget_destroy(c);
}

static void a_open_recent(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a, (void)data;
    win_open_file(g_variant_get_string(p, NULL));
}

static void a_clear_recent(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    GtkRecentManager *rm = gtk_recent_manager_get_default();
    GList *items = gtk_recent_manager_get_items(rm);
    for (GList *l = items; l; l = l->next) {
        char *path = g_filename_from_uri(gtk_recent_info_get_uri(l->data), NULL, NULL);
        if (path && readable(path))
            gtk_recent_manager_remove_item(rm, gtk_recent_info_get_uri(l->data), NULL);
        g_free(path);
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    recent_menu();
}

static void a_new_clipboard(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GdkPixbuf *pix = gtk_clipboard_wait_for_image(cb);
    if (!pix) {
        gdk_display_beep(gdk_display_get_default());
        return;
    }
    win_new(doc_new_image(pix, "Untitled"));
    g_object_unref(pix);
}

static void a_screenshot(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a, (void)data;
    dlg_screenshot(g_variant_get_int32(p));
}

static void a_window(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a, (void)data;
    struct win *w = g_list_nth_data(windows, g_variant_get_int32(p));
    if (w)
        gtk_window_present(GTK_WINDOW(w->window));
}

static void a_bring_all(GSimpleAction *a, GVariant *p, gpointer data)
{
    a_show_all(a, p, data);
}

static void a_help(GSimpleAction *a, GVariant *p, gpointer data)
{
    UNUSED, (void)data;
    struct win *w = active_win();
    dlg_help(w ? GTK_WINDOW(w->window) : NULL);
}

WIN(a_close) { UNUSED; W; if (dlg_close_changed(w)) { w->doc->edits = 0; win_close(w); } }

WIN(a_save)
{
    UNUSED;
    W;
    struct doc *d = w->doc;
    if (!d->path || !d->writable) {
        dlg_export(w, FALSE);
        return;
    }
    GError *err = NULL;
    if (!doc_save(d, &err))
        win_error(w, "The document could not be saved.", err);
    g_clear_error(&err);
    win_doc_changed(w);
}

WIN(a_duplicate) { UNUSED; W; win_new(doc_duplicate(w->doc)); }
WIN(a_rename) { UNUSED; W; dlg_rename(w); }
WIN(a_move_to) { UNUSED; W; dlg_move_to(w); }

WIN(a_revert)
{
    UNUSED;
    W;
    GError *err = NULL;
    if (!doc_revert(w->doc, &err))
        win_error(w, "The document could not be reverted.", err);
    g_clear_error(&err);
    view_deselect(w->view);
    win_doc_changed(w);
}

WIN(a_export) { UNUSED; W; dlg_export(w, FALSE); }
WIN(a_export_pdf) { UNUSED; W; dlg_export(w, TRUE); }
WIN(a_print) { UNUSED; W; dlg_print(w); }

/* Enter Password: unlock a PDF's owner rights (printing, copying...). */
WIN(a_enter_password)
{
    UNUSED;
    W;
    struct doc *d = w->doc;
    for (gboolean again = FALSE;; again = TRUE) {
        char *pw = dlg_password(GTK_WINDOW(w->window), d->path, again);
        if (!pw)
            return;
        struct doc *n = doc_open(d->path, pw, NULL);
        g_free(pw);
        if (n && poppler_document_get_permissions(n->pdf) == POPPLER_PERMISSIONS_FULL) {
            win_set_doc(w, n);
            return;
        }
        doc_free(n);
    }
}

/* --- Edit ------------------------------------------------------------------------------ */

static struct mark *mark_clip;      /* Copy of a mark, for Paste */

WIN(a_undo) { UNUSED; W; if (doc_undo(w->doc)) { view_deselect(w->view); win_doc_changed(w); } }
WIN(a_redo) { UNUSED; W; if (doc_redo(w->doc)) { view_deselect(w->view); win_doc_changed(w); } }

static gboolean copy_selection(struct win *w)
{
    GtkWidget *f = focus_text(w);
    if (f) {
        g_signal_emit_by_name(f, "copy-clipboard");
        return TRUE;
    }
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    char *t = view_selected_text(w->view);
    if (t) {
        gtk_clipboard_set_text(cb, t, -1);
        g_free(t);
        return TRUE;
    }
    GdkPixbuf *pix = view_selected_image(w->view);
    if (pix) {
        gtk_clipboard_set_image(cb, pix);
        g_object_unref(pix);
        return TRUE;
    }
    return FALSE;
}

/* A selected mark is copied inside Preview (Paste puts a twin of it on the
   page); text and pictures go to the system clipboard. */
static gboolean copy_mark(struct win *w)
{
    struct mark *m = view_selected_mark(w->view);
    if (!m)
        return FALSE;
    if (mark_clip)
        mark_unref(mark_clip);
    mark_clip = mark_copy(m);
    return TRUE;
}

WIN(a_copy)
{
    UNUSED;
    W;
    if (!focus_text(w) && copy_mark(w))
        return;
    copy_selection(w);
}

WIN(a_cut)
{
    UNUSED;
    W;
    GtkWidget *f = focus_text(w);
    if (f) {
        g_signal_emit_by_name(f, "cut-clipboard");
        return;
    }
    if (copy_mark(w)) {
        view_delete_selected_mark(w->view);
        return;
    }
    if (copy_selection(w))
        clear_area(w);
}

WIN(a_paste)
{
    UNUSED;
    W;
    GtkWidget *f = focus_text(w);
    if (f) {
        g_signal_emit_by_name(f, "paste-clipboard");
        return;
    }
    if (mark_clip)
        view_add_mark(w->view, mark_copy(mark_clip));
}

WIN(a_delete)
{
    UNUSED;
    W;
    if (view_has_mark(w->view)) {
        view_delete_selected_mark(w->view);
    } else if (view_has_area(w->view) && !is_pdf(w)) {
        clear_area(w);
    } else if (w->sidebar == SB_THUMBS) {
        GArray *pages = target_pages(w);
        delete_pages(w, pages);
        g_array_unref(pages);
    }
}

WIN(a_select_all)
{
    UNUSED;
    W;
    GtkWidget *f = focus_text(w);
    if (f && GTK_IS_EDITABLE(f))
        gtk_editable_select_region(GTK_EDITABLE(f), 0, -1);
    else if (f)
        g_signal_emit_by_name(f, "select-all", TRUE);
    else
        view_select_all(w->view);
}

WIN(a_deselect) { UNUSED; W; view_deselect(w->view); }
WIN(a_invert) { UNUSED; W; view_invert_selection(w->view); }

WIN(a_insert_file)
{
    UNUSED;
    W;
    GtkWidget *c = gtk_file_chooser_dialog_new("Insert Page from File", GTK_WINDOW(w->window),
                                               GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Open",
                                               GTK_RESPONSE_OK, NULL);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(c), filter("All Readable Documents", readable_exts));
    if (gtk_dialog_run(GTK_DIALOG(c)) == GTK_RESPONSE_OK) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(c));
        gtk_widget_destroy(c);
        insert_file(w, f, w->page);
        g_free(f);
        return;
    }
    gtk_widget_destroy(c);
}

WIN(a_insert_blank)
{
    UNUSED;
    W;
    struct doc *d = w->doc;
    double pw, ph;
    page_size(PAGE(d, w->page), &pw, &ph);
    struct page *b = page_blank_pdf(pw, ph);
    if (!b)
        return;
    struct state *st = state_copy(d->st);
    g_ptr_array_insert(st->pages, w->page + 1, b);
    doc_commit(d, st);
    w->page++;
    win_doc_changed(w);
}

WIN(a_find)
{
    UNUSED;
    W;
    if (!w->ui->toolbar_shown) {
        gtk_widget_show(w->toolbar);
        w->ui->toolbar_shown = TRUE;
    }
    gtk_widget_grab_focus(w->search);
}

WIN(a_find_next) { UNUSED; W; view_search_next(w->view, 1); }
WIN(a_find_prev) { UNUSED; W; view_search_next(w->view, -1); }

WIN(a_use_selection)
{
    UNUSED;
    W;
    char *t = view_selected_text(w->view);
    if (t) {
        gtk_entry_set_text(GTK_ENTRY(w->search), g_strstrip(t));
        g_free(t);
    }
}

WIN(a_jump_selection) { UNUSED; W; view_zoom_selection(w->view); }

/* --- View ------------------------------------------------------------------------------ */

WIN(a_toolbar)
{
    UNUSED;
    W;
    w->ui->toolbar_shown = !w->ui->toolbar_shown;
    gtk_widget_set_visible(w->toolbar, w->ui->toolbar_shown);
    win_update(w);
}

WIN(a_customize) { UNUSED; W; dlg_customize_toolbar(w); }

static void a_sidebar(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    const char *s = g_variant_get_string(p, NULL);
    static const char *const names[] = {"none", "thumbs", "toc", "notes", "bookmarks", "contact"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(s, names[i]))
            win_set_sidebar(w, i);
}

static void a_display(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    const char *s = g_variant_get_string(p, NULL);
    view_set_mode(w->view, !strcmp(s, "single") ? DM_SINGLE : !strcmp(s, "two") ? DM_TWO : DM_CONTINUOUS);
}

WIN(a_actual) { UNUSED; W; view_zoom_actual(w->view); }
WIN(a_fit) { UNUSED; W; view_zoom_fit(w->view); }
WIN(a_zoom_in) { UNUSED; W; view_zoom(w->view, 1.25); }
WIN(a_zoom_out) { UNUSED; W; view_zoom(w->view, 0.8); }
WIN(a_zoom_selection) { UNUSED; W; view_zoom_selection(w->view); }

WIN(a_markup_bar)
{
    UNUSED;
    W;
    gboolean on = !gtk_revealer_get_reveal_child(GTK_REVEALER(w->markup_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->markup_revealer), on);
    g_signal_handlers_block_matched(w->ui->markup_toggle, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, w);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->ui->markup_toggle), on);
    g_signal_handlers_unblock_matched(w->ui->markup_toggle, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, w);
    win_update(w);
}

WIN(a_slideshow) { UNUSED; W; view_slideshow(w); }

WIN(a_fullscreen)
{
    UNUSED;
    W;
    w->fullscreen = !w->fullscreen;
    if (w->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(w->window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(w->window));
    win_update(w);
}

/* --- Go, Bookmarks ----------------------------------------------------------------------- */

WIN(a_prev_page) { UNUSED; W; win_set_page(w, w->page - (view_mode(w->view) == DM_TWO ? 2 : 1)); }
WIN(a_next_page) { UNUSED; W; win_set_page(w, w->page + (view_mode(w->view) == DM_TWO ? 2 : 1)); }
WIN(a_first_page) { UNUSED; W; win_set_page(w, 0); }
WIN(a_last_page) { UNUSED; W; win_set_page(w, NPAGES(w->doc) - 1); }
WIN(a_go_to_page) { UNUSED; W; dlg_go_to_page(w); }

WIN(a_back)
{
    UNUSED;
    W;
    struct win_ui *u = w->ui;
    if (!u->back->len)
        return;
    int pg = g_array_index(u->back, int, u->back->len - 1);
    g_array_set_size(u->back, u->back->len - 1);
    g_array_append_val(u->fwd, w->page);
    view_go_page(w->view, pg);
    win_update(w);
}

WIN(a_forward)
{
    UNUSED;
    W;
    struct win_ui *u = w->ui;
    if (!u->fwd->len)
        return;
    int pg = g_array_index(u->fwd, int, u->fwd->len - 1);
    g_array_set_size(u->fwd, u->fwd->len - 1);
    g_array_append_val(u->back, w->page);
    view_go_page(w->view, pg);
    win_update(w);
}

WIN(a_add_bookmark)
{
    UNUSED;
    W;
    dlg_bookmark_name(w);
    bookmarks_menu(w);
    if (w->sidebar == SB_BOOKMARKS)
        bookmarks_rebuild(w);
}

static void a_go_bookmark(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    win_set_page(w, g_variant_get_int32(p));
}

/* --- Tools ------------------------------------------------------------------------------ */

WIN(a_adjust_color) { UNUSED; W; dlg_adjust_color(w); }
WIN(a_adjust_size) { UNUSED; W; dlg_adjust_size(w); }

WIN(a_crop)
{
    UNUSED;
    W;
    int pg, shape;
    double r[4];
    GArray *lasso;
    if (!view_area(w->view, &pg, r, &shape, &lasso) || is_pdf(w))
        return;
    GdkPixbuf *src = doc_page_pixels(w->doc, pg);
    GdkPixbuf *out = img_crop(src, (int)r[0], (int)r[1], (int)ceil(r[2]), (int)ceil(r[3]), shape & 3, lasso);
    g_object_unref(src);
    if (out) {
        doc_set_pixels(w->doc, pg, out);
        g_object_unref(out);
    }
    view_deselect(w->view);
    win_doc_changed(w);
}

WIN(a_rotate_left) { UNUSED; W; rotate_pages(w, 270); }
WIN(a_rotate_right) { UNUSED; W; rotate_pages(w, 90); }

static void flip(struct win *w, gboolean horizontal)
{
    if (is_pdf(w))
        return;
    GdkPixbuf *src = doc_page_pixels(w->doc, w->page);
    GdkPixbuf *out = img_flip(src, horizontal);
    doc_set_pixels(w->doc, w->page, out);
    g_object_unref(src), g_object_unref(out);
    win_doc_changed(w);
}

WIN(a_flip_h) { UNUSED; W; flip(w, TRUE); }
WIN(a_flip_v) { UNUSED; W; flip(w, FALSE); }
WIN(a_inspector) { UNUSED; W; dlg_inspector(w); win_update(w); }
WIN(a_magnifier) { UNUSED; W; view_set_magnifier(w->view, !view_magnifier(w->view)); win_update(w); }
WIN(a_tool_text) { UNUSED; W; win_set_tool(w, T_TEXT); }
WIN(a_tool_rect) { UNUSED; W; win_set_tool(w, T_RECT_SEL); }

/* A new shape, text box or note in the middle of the page, styled as the
   markup bar says. */
static void add_shape(struct win *w, enum mark_kind kind)
{
    struct mark *m = mark_new(kind, 0);
    /* Sized for the screen, whatever the zoom: 120 pixels is a shape you
       can see and grab, on a postcard or on a 6000-pixel photo. */
    double unit = 1 / view_screen_scale(w->view);
    m->stroke = w->stroke;
    m->width = w->line_width;
    m->text_color = w->text_color;
    g_free(m->font);
    m->font = g_strdup(w->font);
    if (kind != MK_LINE && kind != MK_ARROW)
        m->fill = w->fill;
    m->w = 120 * unit, m->h = (kind == MK_LINE || kind == MK_ARROW ? 1 : 90) * unit;
    if (kind == MK_LINE || kind == MK_ARROW)
        m->ax = 0, m->ay = .5, m->bx = 1, m->by = .5;
    if (kind == MK_TEXT) {
        m->fill.alpha = 0, m->stroke.alpha = 0;
        m->text = g_strdup("Text");
        m->h = 28 * unit;
    }
    if (kind == MK_BUBBLE)
        m->text = g_strdup("");
    if (kind == MK_NOTE)
        m->w = m->h = S(22) * unit;
    if (kind == MK_LOUPE)
        m->w = m->h = 110 * unit, m->stroke = (GdkRGBA){.3, .3, .3, 1}, m->fill.alpha = 0, m->width = 3;
    view_add_mark(w->view, m);
}

static void a_annotate(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    const char *s = g_variant_get_string(p, NULL);
    static const struct { const char *n; enum mark_kind k; } shapes[] = {
        {"rect", MK_RECT}, {"oval", MK_OVAL}, {"line", MK_LINE}, {"arrow", MK_ARROW}, {"polygon", MK_POLYGON},
        {"star", MK_STAR}, {"bubble", MK_BUBBLE}, {"text", MK_TEXT}, {"note", MK_NOTE}, {"loupe", MK_LOUPE},
        {"rrect", MK_RRECT},
    };
    for (guint i = 0; i < G_N_ELEMENTS(shapes); i++)
        if (!strcmp(s, shapes[i].n)) {
            add_shape(w, shapes[i].k);
            return;
        }
}

/* Highlight (and Underline, Strikethrough): the selected text if there is
   some, else the tool, for marking text by dragging over it. */
static void markup_or_tool(struct win *w, enum mark_kind k, enum tool t)
{
    char *text = view_selected_text(w->view);
    if (text)
        view_markup_text(w->view, k);
    else
        win_set_tool(w, t);
    g_free(text);
}

WIN(a_highlight) { UNUSED; W; markup_or_tool(w, MK_HIGHLIGHT, T_HIGHLIGHT); }
WIN(a_underline) { UNUSED; W; markup_or_tool(w, MK_UNDERLINE, T_UNDERLINE); }
WIN(a_strike) { UNUSED; W; markup_or_tool(w, MK_STRIKE, T_STRIKE); }
WIN(a_note) { UNUSED; W; add_shape(w, MK_NOTE); }
WIN(a_signatures) { UNUSED; W; dlg_signatures(w); }

WIN(a_fonts)
{
    UNUSED;
    W;
    GtkWidget *d = gtk_font_chooser_dialog_new("Fonts", GTK_WINDOW(w->window));
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(d), w->font);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        g_free(w->font);
        w->font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(d));
        view_style_changed(w->view);
    }
    gtk_widget_destroy(d);
}

/* --- Window ------------------------------------------------------------------------------ */

WIN(a_minimize) { UNUSED; W; gtk_window_iconify(GTK_WINDOW(w->window)); }

WIN(a_zoom_window)
{
    UNUSED;
    W;
    if (gtk_window_is_maximized(GTK_WINDOW(w->window)))
        gtk_window_unmaximize(GTK_WINDOW(w->window));
    else
        gtk_window_maximize(GTK_WINDOW(w->window));
}

/* --- the menu bar ------------------------------------------------------------------------- */

static void accel(const char *action, const char *keys)
{
    const char *list[] = {keys, NULL};
    gtk_application_set_accels_for_action(app, action, list);
}

static void add(GMenu *m, const char *label, const char *action, const char *keys)
{
    GMenuItem *it = g_menu_item_new(label, action);
    if (keys) {
        g_menu_item_set_attribute(it, "accel", "s", keys);
        /* The bare ` of the magnifier is taken in the view (text fields keep it). */
        if (strcmp(keys, "grave"))
            accel(action, keys);
    }
    g_menu_append_item(m, it);
    g_object_unref(it);
}

static void add_target(GMenu *m, const char *label, const char *action, GVariant *target, const char *keys)
{
    GMenuItem *it = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(it, action, target);
    if (keys) {
        g_menu_item_set_attribute(it, "accel", "s", keys);
        char *detailed = g_action_print_detailed_name(action, target);
        accel(detailed, keys);
        g_free(detailed);
    }
    g_menu_append_item(m, it);
    g_object_unref(it);
}

static GMenu *section(GMenu *m)
{
    GMenu *s = g_menu_new();
    g_menu_append_section(m, NULL, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

static GMenu *submenu(GMenu *m, const char *label)
{
    GMenu *s = g_menu_new();
    g_menu_append_submenu(m, label, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

/* An item that can't do anything here (no action: shown greyed out). */
static void none(GMenu *m, const char *label)
{
    add(m, label, NULL, NULL);
}

static GMenuModel *build_menubar(void)
{
    GMenu *bar = g_menu_new(), *m, *s, *sub;

    m = submenu(bar, "Preview");
    s = section(m);
    add(s, "About Preview", "app.about", NULL);
    s = section(m);
    add(s, "Settings…", "app.settings", "<Primary>comma");
    s = section(m);
    sub = submenu(s, "Services");
    none(sub, "No Services Apply");
    s = section(m);
    add(s, "Hide Preview", "app.hide", "<Primary>h");
    add(s, "Hide Others", "app.hide-others", "<Alt><Primary>h");
    add(s, "Show All", "app.show-all", NULL);
    s = section(m);
    add(s, "Quit Preview", "app.quit", "<Primary>q");

    m = submenu(bar, "File");
    s = section(m);
    add(s, "New from Clipboard", "app.new-clipboard", "<Primary>n");
    add(s, "Open…", "app.open", "<Primary>o");
    sub = submenu(s, "Open Recent");
    m_recent_list = section(sub);
    add(section(sub), "Clear Menu", "app.clear-recent", NULL);
    s = section(m);
    add(s, "Close", "win.close", "<Primary>w");
    add(s, "Save", "win.save", "<Primary>s");
    add(s, "Duplicate", "win.duplicate", "<Shift><Primary>s");
    add(s, "Rename…", "win.rename", NULL);
    add(s, "Move To…", "win.move-to", NULL);
    sub = submenu(s, "Revert To");
    add(sub, "Last Opened", "win.revert", NULL);
    none(section(sub), "Browse All Versions…");
    s = section(m);
    add(s, "Export…", "win.export", NULL);
    add(s, "Export as PDF…", "win.export-pdf", NULL);
    s = section(m);
    add(s, "Enter Password…", "win.enter-password", NULL);
    s = section(m);
    sub = submenu(s, "Import from iPhone or iPad");
    none(sub, "No Devices Found");
    sub = submenu(s, "Import from Scanner");
    none(sub, "No Scanners Found");
    sub = submenu(s, "Take Screenshot");
    add_target(sub, "From Selection…", "app.screenshot", g_variant_new_int32(0), NULL);
    add_target(sub, "From Window…", "app.screenshot", g_variant_new_int32(1), NULL);
    add_target(sub, "From Entire Screen", "app.screenshot", g_variant_new_int32(2), NULL);
    s = section(m);
    sub = submenu(s, "Share");
    none(sub, "No Sharing Services");
    s = section(m);
    add(s, "Print…", "win.print", "<Primary>p");

    m = submenu(bar, "Edit");
    s = section(m);
    add(s, "Undo", "win.undo", "<Primary>z");
    add(s, "Redo", "win.redo", "<Shift><Primary>z");
    s = section(m);
    add(s, "Cut", "win.cut", "<Primary>x");
    add(s, "Copy", "win.copy", "<Primary>c");
    add(s, "Paste", "win.paste", "<Primary>v");
    add(s, "Delete", "win.delete", NULL);
    add(s, "Select All", "win.select-all", "<Primary>a");
    add(s, "Deselect All", "win.deselect", "<Alt><Primary>a");
    add(s, "Invert Selection", "win.invert-selection", "<Shift><Primary>i");
    s = section(m);
    sub = submenu(s, "Insert");
    add(sub, "Page from File…", "win.insert-file", NULL);
    none(sub, "Page from Scanner");
    add(sub, "Blank Page", "win.insert-blank", NULL);
    s = section(m);
    sub = submenu(s, "Find");
    add(sub, "Find…", "win.find", "<Primary>f");
    add(sub, "Find Next", "win.find-next", "<Primary>g");
    add(sub, "Find Previous", "win.find-prev", "<Shift><Primary>g");
    add(sub, "Use Selection for Find", "win.use-selection", "<Primary>e");
    add(sub, "Jump to Selection", "win.jump-selection", "<Primary>j");
    s = section(m);
    sub = submenu(s, "Spelling and Grammar");
    none(sub, "Check Spelling While Typing");
    sub = submenu(s, "Speech");
    none(sub, "Start Speaking");
    s = section(m);
    none(s, "Emoji & Symbols");

    m = submenu(bar, "View");
    m_toolbar_sec = s = section(m);
    add(s, "Hide Toolbar", "win.toolbar", "<Alt><Primary>t");
    add(s, "Customize Toolbar…", "win.customize", NULL);
    s = section(m);
    add_target(s, "Content Only", "win.sidebar", g_variant_new_string("none"), "<Alt><Primary>1");
    add_target(s, "Thumbnails", "win.sidebar", g_variant_new_string("thumbs"), "<Alt><Primary>2");
    add_target(s, "Table of Contents", "win.sidebar", g_variant_new_string("toc"), "<Alt><Primary>3");
    add_target(s, "Highlights and Notes", "win.sidebar", g_variant_new_string("notes"), "<Alt><Primary>4");
    add_target(s, "Bookmarks", "win.sidebar", g_variant_new_string("bookmarks"), "<Alt><Primary>5");
    add_target(s, "Contact Sheet", "win.sidebar", g_variant_new_string("contact"), "<Alt><Primary>6");
    s = section(m);
    add_target(s, "Continuous Scroll", "win.display", g_variant_new_string("continuous"), NULL);
    add_target(s, "Single Page", "win.display", g_variant_new_string("single"), NULL);
    add_target(s, "Two Pages", "win.display", g_variant_new_string("two"), NULL);
    s = section(m);
    add(s, "Actual Size", "win.actual", "<Primary>0");
    add(s, "Zoom to Fit", "win.fit", "<Primary>9");
    add(s, "Zoom In", "win.zoom-in", "<Primary>plus");
    {
        const char *zi[] = {"<Primary>plus", "<Primary>equal", "<Primary>KP_Add", NULL};
        gtk_application_set_accels_for_action(app, "win.zoom-in", zi);
    }
    add(s, "Zoom Out", "win.zoom-out", "<Primary>minus");
    add(s, "Zoom to Selection", "win.zoom-selection", "<Primary>asterisk");
    m_markup_sec = s = section(m);
    add(s, "Show Markup Toolbar", "win.markup-bar", "<Shift><Primary>a");
    m_fullscreen_sec = s = section(m);
    add(s, "Slideshow", "win.slideshow", "<Shift><Primary>f");
    add(s, "Enter Full Screen", "win.fullscreen", "F11");

    m = submenu(bar, "Go");
    s = section(m);
    add(s, "Back", "win.back", "<Primary>bracketleft");
    add(s, "Forward", "win.forward", "<Primary>bracketright");
    s = section(m);
    add(s, "Previous Page", "win.prev-page", "<Alt>Up");
    add(s, "Next Page", "win.next-page", "<Alt>Down");
    add(s, "First Page", "win.first-page", "<Alt>Home");
    add(s, "Last Page", "win.last-page", "<Alt>End");
    s = section(m);
    add(s, "Go to Page…", "win.go-to-page", "<Alt><Primary>g");

    m = submenu(bar, "Bookmarks");
    s = section(m);
    add(s, "Add Bookmark", "win.add-bookmark", "<Primary>d");
    m_bookmarks_list = section(m);

    m = submenu(bar, "Tools");
    s = section(m);
    add(s, "Adjust Color…", "win.adjust-color", "<Alt><Primary>c");
    add(s, "Adjust Size…", "win.adjust-size", NULL);
    add(s, "Crop", "win.crop", "<Primary>k");
    s = section(m);
    add(s, "Rotate Left", "win.rotate-left", "<Primary>l");
    add(s, "Rotate Right", "win.rotate-right", "<Primary>r");
    add(s, "Flip Horizontal", "win.flip-h", NULL);
    add(s, "Flip Vertical", "win.flip-v", NULL);
    s = section(m);
    none(s, "Assign Profile…");
    m_tools_sec = s = section(m);
    add(s, "Show Inspector", "win.inspector", "<Primary>i");
    add(s, "Show Magnifier", "win.magnifier", "grave");
    s = section(m);
    add(s, "Text Selection", "win.tool-text", NULL);
    add(s, "Rectangular Selection", "win.tool-rect", NULL);
    s = section(m);
    sub = submenu(s, "Annotate");
    GMenu *ss = section(sub);
    add(ss, "Highlight Text", "win.highlight", "<Super><Primary>h");
    add(ss, "Underline Text", "win.underline", "<Super><Primary>u");
    add(ss, "Strikethrough Text", "win.strike", NULL);
    ss = section(sub);
    add_target(ss, "Rectangle", "win.annotate", g_variant_new_string("rect"), NULL);
    add_target(ss, "Oval", "win.annotate", g_variant_new_string("oval"), NULL);
    add_target(ss, "Line", "win.annotate", g_variant_new_string("line"), NULL);
    add_target(ss, "Arrow", "win.annotate", g_variant_new_string("arrow"), NULL);
    add_target(ss, "Polygon", "win.annotate", g_variant_new_string("polygon"), NULL);
    add_target(ss, "Star", "win.annotate", g_variant_new_string("star"), NULL);
    ss = section(sub);
    add_target(ss, "Text", "win.annotate", g_variant_new_string("text"), "<Super><Primary>t");
    add_target(ss, "Speech Bubble", "win.annotate", g_variant_new_string("bubble"), NULL);
    add(ss, "Note", "win.note", "<Super><Primary>n");
    ss = section(sub);
    add(ss, "Signature…", "win.signatures", NULL);
    add_target(ss, "Loupe", "win.annotate", g_variant_new_string("loupe"), NULL);
    add(s, "Show Fonts", "win.fonts", "<Primary>t");

    m = submenu(bar, "Window");
    s = section(m);
    add(s, "Minimize", "win.minimize", "<Primary>m");
    add(s, "Zoom", "win.zoom-window", NULL);
    s = section(m);
    add(s, "Bring All to Front", "app.bring-all", NULL);
    m_window_list = section(m);

    m = submenu(bar, "Help");
    add(m, "Preview Help", "app.help", "<Shift><Primary>question");
    return G_MENU_MODEL(bar);
}

static const GActionEntry app_actions[] = {
    {"about", a_about, NULL, NULL, NULL, {0}},
    {"settings", a_settings, NULL, NULL, NULL, {0}},
    {"hide", a_hide, NULL, NULL, NULL, {0}},
    {"show-all", a_show_all, NULL, NULL, NULL, {0}},
    {"quit", a_quit, NULL, NULL, NULL, {0}},
    {"new-clipboard", a_new_clipboard, NULL, NULL, NULL, {0}},
    {"open", a_open, NULL, NULL, NULL, {0}},
    {"open-recent", a_open_recent, "s", NULL, NULL, {0}},
    {"clear-recent", a_clear_recent, NULL, NULL, NULL, {0}},
    {"screenshot", a_screenshot, "i", NULL, NULL, {0}},
    {"window", a_window, "i", NULL, NULL, {0}},
    {"bring-all", a_bring_all, NULL, NULL, NULL, {0}},
    {"help", a_help, NULL, NULL, NULL, {0}},
};

static const GActionEntry win_actions[] = {
    {"close", a_close, NULL, NULL, NULL, {0}},
    {"save", a_save, NULL, NULL, NULL, {0}},
    {"duplicate", a_duplicate, NULL, NULL, NULL, {0}},
    {"rename", a_rename, NULL, NULL, NULL, {0}},
    {"move-to", a_move_to, NULL, NULL, NULL, {0}},
    {"revert", a_revert, NULL, NULL, NULL, {0}},
    {"export", a_export, NULL, NULL, NULL, {0}},
    {"export-pdf", a_export_pdf, NULL, NULL, NULL, {0}},
    {"enter-password", a_enter_password, NULL, NULL, NULL, {0}},
    {"print", a_print, NULL, NULL, NULL, {0}},
    {"undo", a_undo, NULL, NULL, NULL, {0}},
    {"redo", a_redo, NULL, NULL, NULL, {0}},
    {"cut", a_cut, NULL, NULL, NULL, {0}},
    {"copy", a_copy, NULL, NULL, NULL, {0}},
    {"paste", a_paste, NULL, NULL, NULL, {0}},
    {"delete", a_delete, NULL, NULL, NULL, {0}},
    {"select-all", a_select_all, NULL, NULL, NULL, {0}},
    {"deselect", a_deselect, NULL, NULL, NULL, {0}},
    {"invert-selection", a_invert, NULL, NULL, NULL, {0}},
    {"insert-file", a_insert_file, NULL, NULL, NULL, {0}},
    {"insert-blank", a_insert_blank, NULL, NULL, NULL, {0}},
    {"find", a_find, NULL, NULL, NULL, {0}},
    {"find-next", a_find_next, NULL, NULL, NULL, {0}},
    {"find-prev", a_find_prev, NULL, NULL, NULL, {0}},
    {"use-selection", a_use_selection, NULL, NULL, NULL, {0}},
    {"jump-selection", a_jump_selection, NULL, NULL, NULL, {0}},
    {"toolbar", a_toolbar, NULL, NULL, NULL, {0}},
    {"customize", a_customize, NULL, NULL, NULL, {0}},
    {"sidebar", a_sidebar, "s", "'none'", NULL, {0}},
    {"display", a_display, "s", "'continuous'", NULL, {0}},
    {"actual", a_actual, NULL, NULL, NULL, {0}},
    {"fit", a_fit, NULL, NULL, NULL, {0}},
    {"zoom-in", a_zoom_in, NULL, NULL, NULL, {0}},
    {"zoom-out", a_zoom_out, NULL, NULL, NULL, {0}},
    {"zoom-selection", a_zoom_selection, NULL, NULL, NULL, {0}},
    {"markup-bar", a_markup_bar, NULL, NULL, NULL, {0}},
    {"slideshow", a_slideshow, NULL, NULL, NULL, {0}},
    {"fullscreen", a_fullscreen, NULL, NULL, NULL, {0}},
    {"back", a_back, NULL, NULL, NULL, {0}},
    {"forward", a_forward, NULL, NULL, NULL, {0}},
    {"prev-page", a_prev_page, NULL, NULL, NULL, {0}},
    {"next-page", a_next_page, NULL, NULL, NULL, {0}},
    {"first-page", a_first_page, NULL, NULL, NULL, {0}},
    {"last-page", a_last_page, NULL, NULL, NULL, {0}},
    {"go-to-page", a_go_to_page, NULL, NULL, NULL, {0}},
    {"add-bookmark", a_add_bookmark, NULL, NULL, NULL, {0}},
    {"go-bookmark", a_go_bookmark, "i", NULL, NULL, {0}},
    {"adjust-color", a_adjust_color, NULL, NULL, NULL, {0}},
    {"adjust-size", a_adjust_size, NULL, NULL, NULL, {0}},
    {"crop", a_crop, NULL, NULL, NULL, {0}},
    {"rotate-left", a_rotate_left, NULL, NULL, NULL, {0}},
    {"rotate-right", a_rotate_right, NULL, NULL, NULL, {0}},
    {"flip-h", a_flip_h, NULL, NULL, NULL, {0}},
    {"flip-v", a_flip_v, NULL, NULL, NULL, {0}},
    {"inspector", a_inspector, NULL, NULL, NULL, {0}},
    {"magnifier", a_magnifier, NULL, NULL, NULL, {0}},
    {"tool-text", a_tool_text, NULL, NULL, NULL, {0}},
    {"tool-rect", a_tool_rect, NULL, NULL, NULL, {0}},
    {"annotate", a_annotate, "s", NULL, NULL, {0}},
    {"highlight", a_highlight, NULL, NULL, NULL, {0}},
    {"underline", a_underline, NULL, NULL, NULL, {0}},
    {"strike", a_strike, NULL, NULL, NULL, {0}},
    {"note", a_note, NULL, NULL, NULL, {0}},
    {"signatures", a_signatures, NULL, NULL, NULL, {0}},
    {"fonts", a_fonts, NULL, NULL, NULL, {0}},
    {"minimize", a_minimize, NULL, NULL, NULL, {0}},
    {"zoom-window", a_zoom_window, NULL, NULL, NULL, {0}},
};

/* --- the toolbars -------------------------------------------------------------------------- */

static GtkWidget *tool_button(GtkWidget *child, const char *tip, gboolean toggle)
{
    GtkWidget *b = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(b), child);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    return b;
}

static GtkWidget *sym_button(enum symbol s, const char *tip, const char *action)
{
    GtkWidget *b = tool_button(symbol_new(s, 16), tip, FALSE);
    if (action)
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

/* A toolbar item: its button(s), and the label shown under them when the
   toolbar shows text (View > Customize Toolbar). */
static GtkWidget *tb_item(struct win *w, int idx, GtkWidget *content, const char *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), content, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_scale_new(.8));
    gtk_label_set_attributes(GTK_LABEL(l), at);
    pango_attr_list_unref(at);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(l, TRUE);
    w->ui->tb[idx] = box;
    w->ui->tb_label[idx] = l;
    return box;
}

static GtkWidget *linked(GtkWidget *a, GtkWidget *b)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "linked");
    gtk_box_pack_start(GTK_BOX(box), a, FALSE, FALSE, 0);
    if (b)
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *menu_button(enum symbol s, const char *tip, GMenuModel *menu)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_button_set_relief(GTK_BUTTON(mb), GTK_RELIEF_NONE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
    if ((int)s >= 0)
        gtk_box_pack_start(GTK_BOX(box), symbol_new(s, 16), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), symbol_new(SY_CHEVRON, 8), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(mb), box);
    gtk_widget_set_tooltip_text(mb, tip);
    if (menu)
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), menu);
    gtk_widget_set_focus_on_click(mb, FALSE);
    return mb;
}

static void markup_toggled(GtkToggleButton *b, gpointer data)
{
    struct win *w = data;
    if (gtk_toggle_button_get_active(b) != gtk_revealer_get_reveal_child(GTK_REVEALER(w->markup_revealer)))
        a_markup_bar(NULL, NULL, w);
}

static void hl_colour(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    const char *c = g_variant_get_string(p, NULL);
    if (!strcmp(c, "underline")) {
        markup_or_tool(w, MK_UNDERLINE, T_UNDERLINE);
        return;
    }
    if (!strcmp(c, "strike")) {
        markup_or_tool(w, MK_STRIKE, T_STRIKE);
        return;
    }
    gdk_rgba_parse(&w->highlight, c);
    w->highlight.alpha = .45;
    markup_or_tool(w, MK_HIGHLIGHT, T_HIGHLIGHT);
}

static void tool_toggled(GtkToggleButton *b, gpointer data)
{
    struct win *w = data;
    if (w->ui->syncing)
        return;
    int t = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tool"));
    if (gtk_toggle_button_get_active(b))
        win_set_tool(w, t);
    else
        win_set_tool(w, is_pdf(w) ? T_TEXT : T_RECT_SEL);   /* off again: back to selecting */
}

static GtkWidget *tool_toggle(struct win *w, enum symbol s, const char *tip, enum tool t)
{
    GtkWidget *b = tool_button(symbol_new(s, 16), tip, TRUE);
    g_object_set_data(G_OBJECT(b), "tool", GINT_TO_POINTER(t));
    g_signal_connect(b, "toggled", G_CALLBACK(tool_toggled), w);
    w->ui->tool_btn[t] = b;
    return b;
}

static void sel_kind(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    const char *s = g_variant_get_string(p, NULL);
    win_set_tool(w, !strcmp(s, "rect") ? T_RECT_SEL : !strcmp(s, "ellipse") ? T_ELLIPSE_SEL
                    : !strcmp(s, "lasso") ? T_LASSO_SEL : T_TEXT);
}

static void shape_kind(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    a_annotate(NULL, p, w);
}

/* A swatch in a button: the colour a markup style button stands for. */
static gboolean swatch_btn_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    GdkRGBA *c = data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    cairo_rectangle(cr, w / 2.0 - S(5), h - S(5), S(10), S(3));
    if (c->alpha == 0) {
        cairo_set_source_rgb(cr, .85, .1, .1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    } else {
        gdk_cairo_set_source_rgba(cr, c);
        cairo_fill(cr);
    }
    return FALSE;
}

static void style_done(struct win *w)
{
    view_style_changed(w->view);
    gtk_widget_queue_draw(w->ui->stroke_btn);
    gtk_widget_queue_draw(w->ui->fill_btn);
}

static void open_color(GtkButton *b, gpointer data)
{
    struct win *w = data;
    GdkRGBA *target = g_object_get_data(G_OBJECT(b), "target");
    GtkWidget *pop = dlg_color_popover(GTK_WIDGET(b), target, style_done, w);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static GtkWidget *color_btn(struct win *w, enum symbol s, const char *tip, GdkRGBA *target)
{
    GtkWidget *b = tool_button(symbol_new(s, 16), tip, FALSE);
    g_object_set_data(G_OBJECT(b), "target", target);
    g_signal_connect(b, "clicked", G_CALLBACK(open_color), w);
    g_signal_connect_after(b, "draw", G_CALLBACK(swatch_btn_draw), target);
    return b;
}

static void width_pick(GtkButton *b, gpointer data)
{
    struct win *w = data;
    double *v = g_object_get_data(G_OBJECT(b), "width");
    if (v)
        w->line_width = *v;
    gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER)));
    style_done(w);
}

static gboolean width_draw(GtkWidget *wd, cairo_t *cr, gpointer data)
{
    double lw = *(double *)data;
    int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
    GdkRGBA c;
    gtk_style_context_get_color(gtk_widget_get_style_context(wd), GTK_STATE_FLAG_NORMAL, &c);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_set_line_width(cr, lw);
    cairo_move_to(cr, S(4), h / 2.0);
    cairo_line_to(cr, w - S(4), h / 2.0);
    cairo_stroke(cr);
    return TRUE;
}

/* Shape Style: line widths, and dashes and shadow for the selected mark. */
static GtkWidget *style_popover(struct win *w, GtkWidget *rel)
{
    static double widths[] = {.5, 1, 2, 3, 5, 7, 10};
    GtkWidget *pop = gtk_popover_new(rel);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(2));
    gtk_container_set_border_width(GTK_CONTAINER(box), S(6));
    for (guint i = 0; i < G_N_ELEMENTS(widths); i++) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, S(110), S(14));
        g_signal_connect(da, "draw", G_CALLBACK(width_draw), &widths[i]);
        gtk_container_add(GTK_CONTAINER(b), da);
        g_object_set_data(G_OBJECT(b), "width", &widths[i]);
        g_signal_connect(b, "clicked", G_CALLBACK(width_pick), w);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_widget_show_all(box);
    return pop;
}

static void open_style(GtkButton *b, gpointer data)
{
    gtk_popover_popup(GTK_POPOVER(style_popover(data, GTK_WIDGET(b))));
}

static void font_set(GtkFontButton *fb, gpointer data)
{
    struct win *w = data;
    g_free(w->font);
    w->font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(fb));
    style_done(w);
}

static void text_color_done(struct win *w)
{
    style_done(w);
}

static void open_text_color(GtkButton *b, gpointer data)
{
    struct win *w = data;
    gtk_popover_popup(GTK_POPOVER(dlg_color_popover(GTK_WIDGET(b), &w->text_color, text_color_done, w)));
}

/* Text Style: font, colour. */
static void open_text_style(GtkButton *b, gpointer data)
{
    struct win *w = data;
    GtkWidget *pop = gtk_popover_new(GTK_WIDGET(b));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
    gtk_container_set_border_width(GTK_CONTAINER(box), S(8));
    GtkWidget *fb = gtk_font_button_new_with_font(w->font);
    g_signal_connect(fb, "font-set", G_CALLBACK(font_set), w);
    GtkWidget *cb = gtk_button_new_with_label("Color…");
    g_signal_connect(cb, "clicked", G_CALLBACK(open_text_color), w);
    gtk_box_pack_start(GTK_BOX(box), fb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cb, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(pop), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void sign_pick(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    W;
    w->sign_index = g_variant_get_int32(p);
    win_set_tool(w, T_SIGN);
}

/* The Sign button's menu: the saved signatures, then managing them. */
static void sign_menu_fill(GtkMenuButton *mb, gpointer data)
{
    (void)data;
    GMenu *m = g_menu_new(), *s = section(m);
    GPtrArray *sigs = signatures_load();
    for (guint i = 0; i < sigs->len; i++) {
        char *l = g_strdup_printf("Signature %u", i + 1);
        add_target(s, l, "win.sign-pick", g_variant_new_int32(i), NULL);
        g_free(l);
    }
    if (!sigs->len)
        none(s, "No Signatures");
    g_ptr_array_unref(sigs);
    add(section(m), "Create Signature…", "win.signatures", NULL);
    gtk_menu_button_set_menu_model(mb, G_MENU_MODEL(m));
    g_object_unref(m);
}

static gboolean sign_press(GtkWidget *wd, GdkEvent *e, gpointer data)
{
    (void)e;
    sign_menu_fill(GTK_MENU_BUTTON(wd), data);
    return FALSE;
}

static GtkWidget *build_markup_bar(struct win *w)
{
    struct win_ui *u = w->ui;
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "toolbar");
    g_object_set(bar, "margin-start", S(8), "margin-end", S(8), NULL);

    GMenu *selm = g_menu_new();
    add_target(selm, "Text Selection", "win.sel-kind", g_variant_new_string("text"), NULL);
    add_target(selm, "Rectangular Selection", "win.sel-kind", g_variant_new_string("rect"), NULL);
    add_target(selm, "Elliptical Selection", "win.sel-kind", g_variant_new_string("ellipse"), NULL);
    add_target(selm, "Lasso Selection", "win.sel-kind", g_variant_new_string("lasso"), NULL);
    GtkWidget *sel = menu_button(SY_RECT_SEL, "Selection Tools", G_MENU_MODEL(selm));
    g_object_unref(selm);
    gtk_box_pack_start(GTK_BOX(bar), sel, FALSE, FALSE, 0);
    u->bar_alpha = tool_toggle(w, SY_ALPHA, "Instant Alpha", T_ALPHA);
    gtk_box_pack_start(GTK_BOX(bar), u->bar_alpha, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), tool_toggle(w, SY_SKETCH, "Sketch", T_SKETCH), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), tool_toggle(w, SY_DRAW, "Draw", T_DRAW), FALSE, FALSE, 0);

    GMenu *shm = g_menu_new();
    static const char *const sh[][2] = {{"Line", "line"}, {"Arrow", "arrow"}, {"Rectangle", "rect"},
                                        {"Rounded Rectangle", "rrect"}, {"Oval", "oval"},
                                        {"Speech Bubble", "bubble"}, {"Star", "star"}, {"Polygon", "polygon"},
                                        {"Loupe", "loupe"}};
    for (guint i = 0; i < G_N_ELEMENTS(sh); i++)
        add_target(shm, sh[i][0], "win.shape-kind", g_variant_new_string(sh[i][1]), NULL);
    gtk_box_pack_start(GTK_BOX(bar), menu_button(SY_SHAPES, "Shapes", G_MENU_MODEL(shm)), FALSE, FALSE, 0);
    g_object_unref(shm);
    GtkWidget *tb = sym_button(SY_TEXTBOX, "Text", NULL);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(tb), "win.annotate");
    gtk_actionable_set_action_target_value(GTK_ACTIONABLE(tb), g_variant_new_string("text"));
    gtk_box_pack_start(GTK_BOX(bar), tb, FALSE, FALSE, 0);
    GtkWidget *sign = menu_button(SY_SIGN, "Sign", NULL);
    g_signal_connect(sign, "button-press-event", G_CALLBACK(sign_press), w);
    sign_menu_fill(GTK_MENU_BUTTON(sign), w);
    gtk_box_pack_start(GTK_BOX(bar), sign, FALSE, FALSE, 0);
    u->bar_note = sym_button(SY_NOTE, "Note", "win.note");
    gtk_box_pack_start(GTK_BOX(bar), u->bar_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, S(6));
    GtkWidget *style = sym_button(SY_SHAPE_STYLE, "Shape Style", NULL);
    g_signal_connect(style, "clicked", G_CALLBACK(open_style), w);
    gtk_box_pack_start(GTK_BOX(bar), style, FALSE, FALSE, 0);
    u->stroke_btn = color_btn(w, SY_BORDER, "Border Color", &w->stroke);
    u->fill_btn = color_btn(w, SY_FILL, "Fill Color", &w->fill);
    gtk_box_pack_start(GTK_BOX(bar), u->stroke_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), u->fill_btn, FALSE, FALSE, 0);
    GtkWidget *font = sym_button(SY_FONT, "Text Style", NULL);
    g_signal_connect(font, "clicked", G_CALLBACK(open_text_style), w);
    gtk_box_pack_start(GTK_BOX(bar), font, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, S(6));
    u->bar_color = sym_button(SY_ADJUST_COLOR, "Adjust Color", "win.adjust-color");
    u->bar_size = sym_button(SY_ADJUST_SIZE, "Adjust Size", "win.adjust-size");
    u->bar_crop = sym_button(SY_CROP, "Crop", "win.crop");
    gtk_box_pack_start(GTK_BOX(bar), u->bar_color, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), u->bar_size, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), u->bar_crop, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(u->bar_crop, TRUE);
    return bar;
}

static GtkWidget *build_toolbar(struct win *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(4));
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "toolbar");
    g_object_set(bar, "margin-start", S(8), "margin-end", S(8), "margin-top", S(4), "margin-bottom", S(4), NULL);

    GMenu *sm = g_menu_new();
    add_target(sm, "Hide Sidebar", "win.sidebar", g_variant_new_string("none"), NULL);
    GMenu *s2 = section(sm);
    add_target(s2, "Thumbnails", "win.sidebar", g_variant_new_string("thumbs"), NULL);
    add_target(s2, "Table of Contents", "win.sidebar", g_variant_new_string("toc"), NULL);
    add_target(s2, "Highlights and Notes", "win.sidebar", g_variant_new_string("notes"), NULL);
    add_target(s2, "Bookmarks", "win.sidebar", g_variant_new_string("bookmarks"), NULL);
    add_target(s2, "Contact Sheet", "win.sidebar", g_variant_new_string("contact"), NULL);
    GtkWidget *sb = menu_button(SY_SIDEBAR, "View", G_MENU_MODEL(sm));
    g_object_unref(sm);
    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_SIDEBAR, sb, "View"), FALSE, FALSE, 0);

    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    w->title = gtk_label_new(NULL);
    w->subtitle = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(w->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(w->title, GTK_ALIGN_START);
    gtk_widget_set_halign(w->subtitle, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->subtitle), "dim-label");
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_scale_new(.85));
    gtk_label_set_attributes(GTK_LABEL(w->subtitle), at);
    pango_attr_list_unref(at);
    gtk_box_pack_start(GTK_BOX(tbox), w->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tbox), w->subtitle, FALSE, FALSE, 0);
    gtk_widget_set_valign(tbox, GTK_ALIGN_CENTER);
    g_object_set(tbox, "margin-start", S(8), NULL);
    gtk_box_pack_start(GTK_BOX(bar), tbox, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_INFO, sym_button(SY_INFO, "Info", "win.inspector"), "Inspector"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       tb_item(w, TB_ZOOM, linked(sym_button(SY_ZOOM_OUT, "Zoom Out", "win.zoom-out"),
                                                  sym_button(SY_ZOOM_IN, "Zoom In", "win.zoom-in")), "Zoom"),
                       FALSE, FALSE, 0);
    GMenu *share = g_menu_new();
    none(share, "No Sharing Services");
    GtkWidget *shb = menu_button(SY_SHARE, "Share", G_MENU_MODEL(share));
    g_object_unref(share);
    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_SHARE, shb, "Share"), FALSE, FALSE, 0);

    GMenu *hm = g_menu_new(), *hs = section(hm);
    add_target(hs, "Yellow", "win.hl-color", g_variant_new_string("#ffd60a"), NULL);
    add_target(hs, "Green", "win.hl-color", g_variant_new_string("#5ee05a"), NULL);
    add_target(hs, "Blue", "win.hl-color", g_variant_new_string("#6ec8ff"), NULL);
    add_target(hs, "Pink", "win.hl-color", g_variant_new_string("#ff8ac9"), NULL);
    add_target(hs, "Purple", "win.hl-color", g_variant_new_string("#c79bff"), NULL);
    hs = section(hm);
    add_target(hs, "Underline", "win.hl-color", g_variant_new_string("underline"), NULL);
    add_target(hs, "Strikethrough", "win.hl-color", g_variant_new_string("strike"), NULL);
    GtkWidget *hb = sym_button(SY_HIGHLIGHT, "Highlight", "win.highlight");
    GtkWidget *hmb = menu_button(-1, "Highlight Color", G_MENU_MODEL(hm));
    g_object_unref(hm);
    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_HIGHLIGHT, linked(hb, hmb), "Highlight"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       tb_item(w, TB_ROTATE, sym_button(SY_ROTATE, "Rotate", "win.rotate-left"), "Rotate"), FALSE,
                       FALSE, 0);
    GtkWidget *mk = tool_button(symbol_new(SY_MARKUP, 16), "Markup", TRUE);
    g_signal_connect(mk, "toggled", G_CALLBACK(markup_toggled), w);
    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_MARKUP, mk, "Markup"), FALSE, FALSE, 0);
    w->ui->markup_toggle = mk;
    w->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->search), "Search");
    gtk_entry_set_width_chars(GTK_ENTRY(w->search), 16);
    g_signal_connect(w->search, "search-changed", G_CALLBACK(search_changed), w);
    g_signal_connect(w->search, "activate", G_CALLBACK(search_activate), w);
    g_signal_connect(w->search, "stop-search", G_CALLBACK(search_stop), w);
    gtk_box_pack_start(GTK_BOX(bar), tb_item(w, TB_SEARCH, w->search, "Search"), FALSE, FALSE, 0);
    return bar;
}

/* --- windows -------------------------------------------------------------------------------- */

static void win_close(struct win *w)
{
    gtk_widget_destroy(GTK_WIDGET(w->window));
}

static gboolean on_delete(GtkWidget *wd, GdkEvent *e, gpointer data)
{
    (void)wd, (void)e;
    struct win *w = data;
    if (!dlg_close_changed(w))
        return TRUE;
    w->doc->edits = 0;
    return FALSE;
}

static void on_destroy(GtkWidget *wd, gpointer data)
{
    (void)wd;
    struct win *w = data;
    windows = g_list_remove(windows, w);
    if (w->ui->search_timer)
        g_source_remove(w->ui->search_timer);
    if (w->inspector)
        gtk_widget_destroy(w->inspector);
    if (w->adjust_color)
        gtk_widget_destroy(w->adjust_color);
    doc_free(w->doc);
    if (w->ui->built)
        g_ptr_array_unref(w->ui->built);
    g_array_unref(w->ui->back);
    g_array_unref(w->ui->fwd);
    g_free(w->ui);
    g_free(w->font);
    g_free(w);
    window_menu();
}

static void on_active(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o, (void)ps;
    struct win *w = data;
    if (gtk_window_is_active(GTK_WINDOW(w->window))) {
        bookmarks_menu(w);
        menus_for(w);
    }
}

static void on_drop(GtkWidget *wd, GdkDragContext *c, int x, int y, GtkSelectionData *sd, guint info, guint t,
                    gpointer data)
{
    (void)wd, (void)x, (void)y, (void)info, (void)data;
    char **uris = gtk_selection_data_get_uris(sd);
    for (int i = 0; uris && uris[i]; i++) {
        char *f = g_filename_from_uri(uris[i], NULL, NULL);
        if (f)
            win_open_file(f);
        g_free(f);
    }
    g_strfreev(uris);
    gtk_drag_finish(c, TRUE, FALSE, t);
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

struct win *win_new(struct doc *d)
{
    struct win *w = g_new0(struct win, 1);
    struct win_ui *u = w->ui = g_new0(struct win_ui, 1);
    u->back = g_array_new(FALSE, FALSE, sizeof(int));
    u->fwd = g_array_new(FALSE, FALSE, sizeof(int));
    u->toolbar_shown = TRUE;
    w->doc = d;
    w->stroke = (GdkRGBA){.9, .16, .14, 1};
    w->fill = (GdkRGBA){0, 0, 0, 0};
    w->text_color = (GdkRGBA){0, 0, 0, 1};
    w->highlight = (GdkRGBA){1, .84, .04, .45};
    w->line_width = 2;
    w->font = g_strdup("DejaVu Sans 14");
    w->shape = MK_RECT;
    w->window = GTK_APPLICATION_WINDOW(gtk_application_window_new(app));
    gtk_application_window_set_show_menubar(w->window, TRUE);
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), win_actions, G_N_ELEMENTS(win_actions), w);
    static const GActionEntry extra[] = {
        {"hl-color", hl_colour, "s", NULL, NULL, {0}},
        {"sel-kind", sel_kind, "s", NULL, NULL, {0}},
        {"shape-kind", shape_kind, "s", NULL, NULL, {0}},
        {"sign-pick", sign_pick, "i", NULL, NULL, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), extra, G_N_ELEMENTS(extra), w);

    /* A window sized for the document: most of the screen, as Preview does. */
    GdkRectangle wa = {0, 0, 1024, 768};
    GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (mon)
        gdk_monitor_get_workarea(mon, &wa);
    gtk_window_set_default_size(GTK_WINDOW(w->window), MIN(wa.width * 3 / 4, S(1000)), wa.height * 9 / 10);
    cairo_surface_t *ic = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_t *cr = cairo_create(ic);
    app_icon_draw(cr, 64);
    cairo_destroy(cr);
    GdkPixbuf *icon = gdk_pixbuf_get_from_surface(ic, 0, 0, 64, 64);
    cairo_surface_destroy(ic);
    gtk_window_set_icon(GTK_WINDOW(w->window), icon);
    g_object_unref(icon);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(w->window), vbox);
    w->toolbar = build_toolbar(w);
    gtk_box_pack_start(GTK_BOX(vbox), w->toolbar, FALSE, FALSE, 0);
    w->markup_revealer = gtk_revealer_new();
    gtk_container_add(GTK_CONTAINER(w->markup_revealer), build_markup_bar(w));
    gtk_box_pack_start(GTK_BOX(vbox), w->markup_revealer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    w->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), w->paned, TRUE, TRUE, 0);
    w->sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->sidebar_box), "preview-thumbs");
    gtk_widget_set_size_request(w->sidebar_box, S(170), -1);
    w->sidebar_stack = gtk_stack_new();
    gtk_box_pack_start(GTK_BOX(w->sidebar_box), w->sidebar_stack, TRUE, TRUE, 0);

    w->thumbs = gtk_list_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(w->thumbs), "preview-thumbs");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->thumbs), GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(w->thumbs), TRUE);
    g_signal_connect(w->thumbs, "selected-rows-changed", G_CALLBACK(thumbs_selected), w);
    g_signal_connect(w->thumbs, "key-press-event", G_CALLBACK(thumbs_key), w);
    GtkTargetEntry drops[] = {page_target, uri_target};
    gtk_drag_dest_set(w->thumbs, GTK_DEST_DEFAULT_ALL, drops, 2, GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect(w->thumbs, "drag-data-received", G_CALLBACK(thumbs_drop), w);
    gtk_stack_add_named(GTK_STACK(w->sidebar_stack), scrolled(w->thumbs), "thumbs");

    u->toc_view = gtk_tree_view_new();
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(u->toc_view), FALSE);
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(u->toc_view), -1, "", cell, "text", TOC_TITLE, NULL);
    g_signal_connect(u->toc_view, "row-activated", G_CALLBACK(toc_activated), w);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(u->toc_view)), "changed", G_CALLBACK(toc_selected), w);
    gtk_stack_add_named(GTK_STACK(w->sidebar_stack), scrolled(u->toc_view), "toc");
    w->toc = u->toc_view;

    u->notes_list = gtk_list_box_new();
    g_signal_connect(u->notes_list, "row-activated", G_CALLBACK(notes_activated), w);
    gtk_stack_add_named(GTK_STACK(w->sidebar_stack), scrolled(u->notes_list), "notes");
    w->notes = u->notes_list;

    u->bookmarks_list = gtk_list_box_new();
    g_signal_connect(u->bookmarks_list, "row-activated", G_CALLBACK(bookmark_activated), w);
    g_signal_connect(u->bookmarks_list, "key-press-event", G_CALLBACK(bookmarks_key), w);
    gtk_stack_add_named(GTK_STACK(w->sidebar_stack), scrolled(u->bookmarks_list), "bookmarks");
    w->bookmarks = u->bookmarks_list;

    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    u->results_label = gtk_label_new("");
    g_object_set(u->results_label, "margin", S(6), NULL);
    u->results_list = gtk_list_box_new();
    g_signal_connect(u->results_list, "row-activated", G_CALLBACK(result_activated), w);
    gtk_box_pack_start(GTK_BOX(rbox), u->results_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbox), scrolled(u->results_list), TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(w->sidebar_stack), rbox, "results");
    w->results = u->results_list;
    gtk_paned_pack1(GTK_PANED(w->paned), w->sidebar_box, FALSE, FALSE);

    w->view = view_new(w);
    w->content_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(w->content_stack), view_widget(w->view), "pages");
    w->contact = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(w->contact), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(w->contact), FALSE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(w->contact), 30);
    g_signal_connect(w->contact, "child-activated", G_CALLBACK(contact_activated), w);
    GtkWidget *csw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(csw), w->contact);
    gtk_stack_add_named(GTK_STACK(w->content_stack), csw, "contact");
    gtk_paned_pack2(GTK_PANED(w->paned), w->content_stack, TRUE, FALSE);

    GtkTargetEntry files[] = {uri_target};
    gtk_drag_dest_set(view_widget(w->view), GTK_DEST_DEFAULT_ALL, files, 1, GDK_ACTION_COPY);
    g_signal_connect(view_widget(w->view), "drag-data-received", G_CALLBACK(on_drop), w);
    g_signal_connect(w->window, "delete-event", G_CALLBACK(on_delete), w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
    g_signal_connect(w->window, "notify::is-active", G_CALLBACK(on_active), w);

    windows = g_list_append(windows, w);
    thumbs_rebuild(w);
    gtk_widget_show_all(GTK_WIDGET(w->window));
    gtk_widget_set_visible(u->bar_crop, FALSE);
    win_apply_toolbar(w);
    /* PDFs of several pages open with thumbnails, as Preview does; a single
       page or picture shows just the page. */
    win_set_sidebar(w, NPAGES(d) > 1 ? SB_THUMBS : SB_NONE);
    w->last_sidebar = SB_THUMBS;
    win_set_tool(w, d->kind == DOC_PDF ? T_TEXT : T_RECT_SEL);
    gtk_widget_grab_focus(view_widget(w->view));
    window_menu();
    win_update(w);
    return w;
}

void win_set_doc(struct win *w, struct doc *d)
{
    doc_free(w->doc);
    w->doc = d;
    w->page = MIN(w->page, NPAGES(d) - 1);
    view_deselect(w->view);
    if (w->ui->built) {
        g_ptr_array_unref(w->ui->built);
        w->ui->built = NULL;
    }
    win_doc_changed(w);
    window_menu();
}

struct win *win_open_file(const char *path)
{
    char *abs = g_canonicalize_filename(path, NULL);
    for (GList *l = windows; l; l = l->next) {
        struct win *w = l->data;
        if (w->doc->path && !strcmp(w->doc->path, abs)) {
            gtk_window_present(GTK_WINDOW(w->window));
            g_free(abs);
            return w;
        }
    }
    GError *err = NULL;
    char *pw = NULL;
    struct doc *d = NULL;
    struct win *parent = active_win();
    for (gboolean again = FALSE;; again = TRUE) {
        d = doc_open(abs, pw, &err);
        if (d || !g_error_matches(err, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED))
            break;
        g_clear_error(&err);
        g_free(pw);
        pw = dlg_password(parent ? GTK_WINDOW(parent->window) : NULL, abs, again);
        if (!pw)
            break;
    }
    g_free(pw);
    struct win *w = NULL;
    if (d) {
        w = win_new(d);
        char *uri = g_filename_to_uri(abs, NULL, NULL);
        gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
        g_free(uri);
        recent_menu();
    } else if (err) {
        char *name = g_filename_display_basename(abs);
        char *msg = g_strdup_printf("The file “%s” couldn't be opened.", name);
        win_error(parent, msg, err);
        g_free(msg), g_free(name);
    }
    g_clear_error(&err);
    g_free(abs);
    return w;
}

/* --- the application ------------------------------------------------------------------ */

static void load_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".preview-thumbs, .preview-thumbs list, .preview-thumbs viewport {"
        "  background-color: #e8e8ed;"
        "}"
        ".preview-thumbs row, .preview-thumbs row:hover,"
        ".preview-thumbs row:selected, .preview-thumbs row:selected:focus {"
        "  background-color: transparent;"
        "  background-image: none;"
        "  border: none;"
        "  box-shadow: none;"
        "  outline: none;"
        "}",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void on_startup(GApplication *a, gpointer data)
{
    (void)a, (void)data;
    read_scale();
    prefs_load();
    load_css();
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), NULL);
    gtk_application_set_menubar(app, build_menubar());
    recent_menu();
    gtk_window_set_default_icon_name("preview");
}

static char **cli_files;
static int cli_n;

static void on_activate(GApplication *a, gpointer data)
{
    int i;
    (void)a, (void)data;
    /* GApplication's "open" needs a session bus. MyOS has no dbus-launch,
       so a file on the command line (WritePerfect Preview) often never
       arrives. Open those paths here if "open" did not. */
    if (!windows && cli_n) {
        for (i = 0; i < cli_n; i++)
            win_open_file(cli_files[i]);
    }
    if (windows)
        return;
    if (cli_n) {
        g_application_quit(G_APPLICATION(app));
        return;
    }
    /* Started with nothing to open: the Open panel, as on the Mac. */
    a_open(NULL, NULL, NULL);
    if (!windows)
        g_application_quit(G_APPLICATION(app));
}

static void on_open(GApplication *a, GFile **files, int n, const char *hint, gpointer data)
{
    (void)a, (void)hint, (void)data;
    for (int i = 0; i < n; i++) {
        char *p = g_file_get_path(files[i]);
        if (p)
            win_open_file(p);
        g_free(p);
    }
}

int main(int argc, char **argv)
{
    int i;
    cli_files = g_new0(char *, argc);
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-')
            cli_files[cli_n++] = g_strdup(argv[i]);
    }
    /* No D-Bus on MyOS: every start is its own process (NON_UNIQUE).
       HANDLES_OPEN is off on purpose — GApplication delivers those files
       over the session bus, which we do not have. argv files are opened
       in on_activate from cli_files. */
    app = gtk_application_new("org.myos.Preview", G_APPLICATION_NON_UNIQUE);
    g_set_application_name("Preview");
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    /* GApplication only sees the options: given file names without
       HANDLES_OPEN it refuses them ("This application can not open files")
       and exits before activate — a double-clicked PDF never opened. The
       files are cli_files, opened in on_activate. */
    char **gargv = g_new0(char *, argc + 1);
    int gargc = 0;
    gargv[gargc++] = argv[0];
    for (i = 1; i < argc; i++)
        if (argv[i][0] == '-')
            gargv[gargc++] = argv[i];
    int r = g_application_run(G_APPLICATION(app), gargc, gargv);
    g_free(gargv);
    doc_cleanup();
    g_object_unref(app);
    return r;
}
