/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * preview.h — Preview, MyOS's copy of Apple's Preview: what the parts share.
 *
 * The parts:
 *   main.c     the application: windows, the menu bar (Preview's own menus,
 *              item for item), the toolbar, the sidebar, the actions
 *   doc.c      a document: opening every format Preview opens, saving,
 *              exporting to the formats Preview exports
 *   codecs.c   the formats GTK can't read or write itself: multi-page TIFF,
 *              JPEG 2000, OpenEXR, Photoshop, HEIC (write)
 *   view.c     the pages on screen: layout, zoom, rendering, selection,
 *              search, links, the magnifier, the markup tools
 *   marks.c    markup (shapes, text, notes, sketches, signatures): drawing
 *              them, and turning them into PDF annotations on save
 *   imgops.c   image edits: rotate, flip, crop, resize, colour, instant alpha
 *   dialogs.c  Export, Adjust Size, Adjust Colour, Inspector, Settings...
 *   icons.c    the toolbar's symbols, drawn (sharp at every desktop scale)
 *
 * The model, in one paragraph: a document is a list of pages and a list of
 * marks. Neither is ever changed in place — an edit makes new page or mark
 * objects and a new list — so the state before an edit is still whole, and
 * Undo is putting the old lists back (struct state, doc_commit). Pages and
 * marks are reference-counted so that costs a few pointers per step.
 */
#ifndef PREVIEW_H
#define PREVIEW_H

#include <gtk/gtk.h>
#include <poppler.h>

/* --- pages ------------------------------------------------------------------- */

/* One page as the window shows it. A PDF page points into a PopplerDocument
   (the one opened, or one a page was inserted from); an image page holds its
   pixels. `id` stays the same when an edit makes a new version of the page
   (rotated, cropped...), so marks know which page they belong to. */
struct page {
    int refs;
    unsigned id;
    PopplerDocument *pdf;       /* (a reference) NULL for an image page */
    char *src;                  /* the PDF file the page is in (qpdf reads it) */
    int index;                  /* its number in that PDF, from 0 */
    int rotation;               /* the user's turn on top of the PDF's: 0..270 */
    GdkPixbuf *pix;             /* an image page's pixels */
    double w, h;                /* unrotated: points (PDF) or pixels (image) */
};

struct page *page_ref(struct page *p);
void page_unref(struct page *p);
struct page *page_copy(const struct page *p);      /* a new version, same id */
unsigned page_new_id(void);

/* The page's size as shown, turned by the user's rotation. (A PDF's own
   /Rotate is already in w and h: poppler renders the page turned.) */
void page_size(const struct page *p, double *w, double *h);
/* From the page's unrotated space to the shown one, at cr's scale. */
void page_transform(cairo_t *cr, const struct page *p);
/* The page and its marks, shown turned, at cr's scale. */
void page_render(cairo_t *cr, const struct page *p, GPtrArray *marks);
/* PDF pages made from nothing, for Insert > Blank Page and for pictures put
   into a PDF (temporary one-page PDFs that qpdf can merge in on save). */
struct page *page_blank_pdf(double w, double h);
struct page *page_pdf_from_pixbuf(GdkPixbuf *pix, double dpi);
void doc_cleanup(void);                           /* delete the temporary files */

/* --- marks (markup) ------------------------------------------------------------- */

enum mark_kind {
    MK_HIGHLIGHT, MK_UNDERLINE, MK_STRIKE,         /* on a PDF's text */
    MK_LINE, MK_ARROW, MK_RECT, MK_RRECT, MK_OVAL, MK_BUBBLE, MK_STAR,
    MK_POLYGON, MK_LOUPE, MK_TEXT, MK_NOTE, MK_SKETCH, MK_SIGNATURE,
};

/* Geometry is a box in the page's unrotated space (points or pixels, top
   left 0,0) plus shape details relative to it (0..1), so moving or resizing
   a mark only changes the box. */
struct mark {
    int refs;
    enum mark_kind kind;
    unsigned page;              /* the page's id */
    double x, y, w, h;
    double ax, ay, bx, by;      /* line and arrow ends, 0..1 in the box */
    GArray *pts;                /* sketch/signature: 0..1 pairs, NAN breaks strokes */
    GArray *quads;              /* text markup: absolute boxes (x, y, w, h)... */
    GdkRGBA stroke, fill;       /* fill alpha 0: none */
    double width;
    int dash, shadow;
    char *text;                 /* text box, note, speech bubble */
    char *font;                 /* Pango font, e.g. "DejaVu Sans 14" */
    GdkRGBA text_color;
    int align;                  /* PangoAlignment */
    int sides;                  /* star points, polygon sides */
    double magnify;             /* loupe */
};

struct mark *mark_new(enum mark_kind kind, unsigned page);
struct mark *mark_ref(struct mark *m);
void mark_unref(struct mark *m);
struct mark *mark_copy(const struct mark *m);
void mark_draw(cairo_t *cr, const struct mark *m, const struct page *pg, cairo_surface_t *under);
int mark_hit(const struct mark *m, double x, double y, double slop);
const char *mark_name(enum mark_kind k);
/* Put the marks of `pg` into PDF annotations on its PopplerPage. */
void marks_to_pdf(GPtrArray *marks, const struct page *pg, PopplerPage *pp);
/* Paint the marks of `pg` into its pixels (an image's Save). */
GdkPixbuf *marks_flatten(GPtrArray *marks, const struct page *pg);

/* --- documents ------------------------------------------------------------------ */

struct state {
    int refs;
    GPtrArray *pages;           /* struct page* (each a reference) */
    GPtrArray *marks;           /* struct mark* */
};

enum doc_kind { DOC_PDF, DOC_IMAGE };

struct doc {
    enum doc_kind kind;
    char *path;                 /* NULL: untitled (from the clipboard, a screenshot) */
    char *name;                 /* what the title bar says */
    char *format;               /* "pdf", or the image format: "png", "jpeg", "tiff"... */
    gboolean writable;          /* Save can write this format back */
    PopplerDocument *pdf;       /* the PDF as opened */
    char *password;
    struct state *st;
    GPtrArray *undo, *redo;     /* struct state* */
    int edits;                  /* changes since the last save (0: clean) */
    char *backup;               /* the file as opened, kept for Revert (or NULL) */
    double dpi;                 /* images: pixels per inch (72 if unknown) */
    int frames;                 /* animated GIF: frames (each is a page) */
};

struct doc *doc_open(const char *path, const char *password, GError **err);
struct doc *doc_new_image(GdkPixbuf *pix, const char *name);  /* untitled */
struct doc *doc_duplicate(struct doc *d);
void doc_free(struct doc *d);
gboolean doc_needs_password(const char *path);

/* An edit: `st` (a new state) becomes current; the old one goes on Undo. */
void doc_commit(struct doc *d, struct state *st);
gboolean doc_undo(struct doc *d);
gboolean doc_redo(struct doc *d);
struct state *state_copy(const struct state *s);   /* new lists, same objects */
void state_unref(struct state *s);
#define NPAGES(d) ((int)(d)->st->pages->len)
#define PAGE(d, i) ((struct page *)g_ptr_array_index((d)->st->pages, (i)))
#define MARK(d, i) ((struct mark *)g_ptr_array_index((d)->st->marks, (i)))

GdkPixbuf *doc_page_pixels(struct doc *d, int i);      /* with its marks painted in */
void doc_set_pixels(struct doc *d, int i, GdkPixbuf *pix);
gboolean doc_save(struct doc *d, GError **err);
gboolean doc_revert(struct doc *d, GError **err);

/* Export (File > Export...). `page` is the page an image format gets. */
enum export_format {
    EX_HEIC, EX_JPEG, EX_JP2, EX_EXR, EX_PDF, EX_PNG, EX_TIFF,
    EX_BMP, EX_GIF, EX_ICO, EX_TGA, EX_WEBP,      /* "Show All" (Option on the Mac) */
    EX_COUNT
};
struct export_opts {
    enum export_format fmt;
    double quality;             /* 0..1: JPEG, HEIC, JPEG 2000 */
    gboolean alpha;
    double dpi;                 /* rendering a PDF page into pixels */
    int tiff_compression;       /* 0 none, 1 LZW, 2 PackBits, 3 ZIP */
    char *password;             /* PDF: encrypt (qpdf) */
};
extern const char *const export_names[EX_COUNT];
extern const char *const export_exts[EX_COUNT];
gboolean doc_export(struct doc *d, int page, const char *path, const struct export_opts *o,
                    GError **err);
GdkPixbuf *page_pixbuf(const struct page *p, GPtrArray *marks, double scale);

/* --- codecs.c: what GTK's image loaders can't do ------------------------------------ */

GPtrArray *codec_read_tiff(const char *path, double *dpi, GError **err);  /* GdkPixbuf* */
GdkPixbuf *codec_read_jp2(const char *path, GError **err);
GdkPixbuf *codec_read_exr(const char *path, GError **err);
GdkPixbuf *codec_read_psd(const char *path, GError **err);
GdkPixbuf *codec_read_heic(const char *path, GError **err);
gboolean codec_write_tiff(GPtrArray *pixbufs, const char *path, int compression, double dpi,
                          gboolean alpha, GError **err);
gboolean codec_write_jp2(GdkPixbuf *pix, const char *path, double quality, GError **err);
gboolean codec_write_exr(GdkPixbuf *pix, const char *path, GError **err);
gboolean codec_write_heic(GdkPixbuf *pix, const char *path, double quality, gboolean alpha,
                          GError **err);
gboolean codec_write_tga(GdkPixbuf *pix, const char *path, GError **err);
gboolean codec_write_gif(GdkPixbuf *pix, const char *path, GError **err);

/* --- imgops.c ------------------------------------------------------------------------ */

struct color_adjust {
    double black, white;                                /* levels: 0..1 (0 and 1: as is) */
    double exposure, contrast, highlights, shadows;     /* -1..1 */
    double saturation;                                  /* 0..2, 1 = as is */
    double temperature, tint;                           /* -1..1 */
    double sepia, sharpness;                            /* 0..1, -1..1 */
};
GdkPixbuf *img_rotate(GdkPixbuf *p, int degrees);
GdkPixbuf *img_flip(GdkPixbuf *p, gboolean horizontal);
GdkPixbuf *img_crop(GdkPixbuf *p, int x, int y, int w, int h, int shape, GArray *lasso);
GdkPixbuf *img_resize(GdkPixbuf *p, int w, int h);
GdkPixbuf *img_adjust(GdkPixbuf *p, const struct color_adjust *a);
void img_auto_levels(GdkPixbuf *p, struct color_adjust *a);
GdkPixbuf *img_instant_alpha(GdkPixbuf *p, int x, int y, double tolerance);

/* --- the window ------------------------------------------------------------------------ */

enum sidebar_mode { SB_NONE, SB_THUMBS, SB_TOC, SB_NOTES, SB_BOOKMARKS, SB_CONTACT };
enum display_mode { DM_CONTINUOUS, DM_SINGLE, DM_TWO };
enum tool {
    T_TEXT, T_RECT_SEL, T_ELLIPSE_SEL, T_LASSO_SEL, T_ALPHA,
    T_SKETCH, T_DRAW, T_SHAPE, T_TEXTBOX, T_NOTE, T_SIGN,
    T_HIGHLIGHT, T_UNDERLINE, T_STRIKE,
};

struct view;

struct win {
    GtkApplicationWindow *window;
    struct doc *doc;
    struct view *view;
    GtkWidget *toolbar, *markup_revealer, *title, *subtitle, *search;
    GtkWidget *paned, *sidebar_stack, *thumbs, *toc, *notes, *bookmarks, *results;
    GtkWidget *sidebar_box, *content_stack, *contact, *lock_box;
    GtkWidget *tool_buttons[16];
    enum sidebar_mode sidebar;
    enum sidebar_mode last_sidebar;
    gboolean fullscreen;
    int page;                   /* the current page */
    int last_saved_page;
    /* markup style for new marks */
    GdkRGBA stroke, fill, text_color, highlight;
    double line_width;
    char *font;
    enum mark_kind shape;
    int sign_index;
    GdkRGBA stroke_pending;
    GtkWidget *inspector;
    GtkWidget *adjust_color;
    struct win_ui *ui;          /* main.c's own widgets and state */
};

extern GtkApplication *app;
extern double px_per_pt;        /* screen pixels per PDF point at 100% */
extern double k;                /* the desktop scale, for our own sizes */
#define S(x) ((int)((x) * k + 0.5))

/* Settings (Preview > Settings...), in ~/.config/myos/preview.conf */
struct prefs {
    GdkRGBA background;
    gboolean image_pixels_100;  /* images: 100% = one image pixel per screen pixel */
    gboolean pdf_fit;           /* PDFs open zoomed to fit (else 100%) */
    enum display_mode pdf_mode;
    gboolean smooth;
    gboolean open_together;     /* several files at once: one window */
    guint toolbar_hidden;       /* bit i: toolbar_items[i] is hidden (View > Customize Toolbar) */
    gboolean toolbar_text;      /* labels under the toolbar icons */
};
extern struct prefs prefs;
extern const char *const toolbar_items[];
extern const int n_toolbar_items;
void win_apply_toolbar(struct win *w);
void prefs_load(void);
void prefs_save(void);

struct win *win_open_file(const char *path);
struct win *win_new(struct doc *d);
void win_set_doc(struct win *w, struct doc *d);  /* the window shows d now (and frees the old) */
void win_update(struct win *w);            /* title, menus' enabled state, sidebar */
void win_doc_changed(struct win *w);       /* pages or marks changed */
void win_set_page(struct win *w, int page);
void win_set_tool(struct win *w, enum tool t);
void win_set_sidebar(struct win *w, enum sidebar_mode m);
void win_error(struct win *w, const char *what, GError *err);
struct win *win_of(GtkWidget *widget);
GList *win_list(void);
char *config_path(const char *name);       /* ~/.config/myos/<name> */

/* --- view.c ------------------------------------------------------------------------ */

struct view *view_new(struct win *w);
GtkWidget *view_widget(struct view *v);
void view_reload(struct view *v);          /* the document's pages changed */
void view_redraw(struct view *v);
void view_set_mode(struct view *v, enum display_mode m);
enum display_mode view_mode(struct view *v);
void view_zoom(struct view *v, double factor);  /* multiply */
void view_zoom_fit(struct view *v);
void view_zoom_actual(struct view *v);
void view_zoom_selection(struct view *v);
double view_get_zoom(struct view *v);
void view_go_page(struct view *v, int page);
void view_go_dest(struct view *v, PopplerDest *dest);
int view_current_page(struct view *v);
void view_set_tool(struct view *v, enum tool t);
enum tool view_tool(struct view *v);
gboolean view_has_selection(struct view *v);      /* text or area */
gboolean view_has_area(struct view *v);           /* rectangular etc. selection */
gboolean view_has_mark(struct view *v);
char *view_selected_text(struct view *v);
GdkPixbuf *view_selected_image(struct view *v);
gboolean view_area(struct view *v, int *page, double r[4], int *shape, GArray **lasso);
void view_select_all(struct view *v);
void view_deselect(struct view *v);
void view_invert_selection(struct view *v);
void view_delete_selected_mark(struct view *v);
void view_select_mark(struct view *v, struct mark *m);
struct mark *view_selected_mark(struct view *v);
double view_screen_scale(struct view *v);          /* screen pixels per page unit now */
void view_set_magnifier(struct view *v, gboolean on);
gboolean view_magnifier(struct view *v);
void view_markup_text(struct view *v, enum mark_kind k);  /* highlight/underline/strike the selection */
void view_add_mark(struct view *v, struct mark *m);       /* centred on the current page */
void view_style_changed(struct view *v);                  /* stroke/fill/font for the selected mark */
/* search */
int view_search(struct view *v, const char *text, gboolean whole_words, gboolean case_sensitive);
void view_search_next(struct view *v, int dir);
void view_search_clear(struct view *v);
int view_search_count(struct view *v);
int view_search_current(struct view *v);
void view_search_select(struct view *v, int i);
GList *view_search_results(struct view *v);       /* struct hit* */
struct hit { int page; PopplerRectangle r; char *context; };
void view_slideshow(struct win *w);
/* thumbnails for the sidebar and the contact sheet */
cairo_surface_t *thumb_get(struct win *w, struct page *p, int size, gboolean *pending);
void thumbs_forget(void);
void thumbs_fill_now(struct win *w, int from, int n);

/* --- dialogs.c ------------------------------------------------------------------------ */

gboolean dlg_export(struct win *w, gboolean pdf_only);   /* TRUE: written */
void dlg_print(struct win *w);
void dlg_adjust_size(struct win *w);
void dlg_adjust_color(struct win *w);
void dlg_inspector(struct win *w);
void dlg_settings(GtkWindow *parent);
void dlg_go_to_page(struct win *w);
char *dlg_password(GtkWindow *parent, const char *file, gboolean again);
void dlg_rename(struct win *w);
void dlg_move_to(struct win *w);
void dlg_signatures(struct win *w);
void dlg_about(GtkWindow *parent);
void dlg_help(GtkWindow *parent);
void dlg_customize_toolbar(struct win *w);
void dlg_screenshot(int kind);              /* 0 selection, 1 window, 2 screen */
gboolean dlg_close_changed(struct win *w);  /* may close? (saves or discards) */
void dlg_bookmark_name(struct win *w);
GtkWidget *dlg_color_popover(GtkWidget *relative, GdkRGBA *target, void (*done)(struct win *), struct win *w);
/* signatures: strokes in 0..1 boxes, kept in ~/.config/myos/preview-signatures */
GPtrArray *signatures_load(void);           /* GArray* of points */
void signatures_save(GPtrArray *sigs);
/* bookmarks, per file, in ~/.config/myos/preview-bookmarks */
struct bookmark { int page; char *name; };
GPtrArray *bookmarks_get(const char *path);   /* struct bookmark*, by page */
void bookmark_add(const char *path, int page, const char *name);
void bookmark_remove(const char *path, int page);

/* --- icons.c ------------------------------------------------------------------------ */

enum symbol {
    SY_SIDEBAR, SY_INFO, SY_ZOOM_OUT, SY_ZOOM_IN, SY_SHARE, SY_HIGHLIGHT, SY_ROTATE,
    SY_MARKUP, SY_SEARCH, SY_TEXT_SEL, SY_RECT_SEL, SY_ALPHA, SY_SKETCH, SY_DRAW,
    SY_SHAPES, SY_TEXTBOX, SY_SIGN, SY_NOTE, SY_SHAPE_STYLE, SY_BORDER, SY_FILL,
    SY_FONT, SY_ADJUST_COLOR, SY_ADJUST_SIZE, SY_CROP, SY_CHEVRON, SY_LOCK, SY_FORM,
    SY_ROTATE_RIGHT, SY_DOC, SY_BOOKMARK,
};
void symbol_draw(cairo_t *cr, enum symbol s, double size, const GdkRGBA *c);
GtkWidget *symbol_new(enum symbol s, int pts);
void app_icon_draw(cairo_t *cr, double size);   /* Preview's own icon */

#endif
