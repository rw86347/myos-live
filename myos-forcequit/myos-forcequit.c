/*
 * myos-forcequit — Force Quit, as on the Mac: the programs that have
 * windows, and a button that ends the chosen one at once (kill -9).
 *
 * Ctrl+Shift+Esc opens it (/etc/icewm/keys), as Task Manager does on
 * Windows; it is also in All Programs → System. IceWM's own "Kill Client"
 * only asks politely (SIGTERM, or it closes the X connection), which a
 * hung program ignores. SIGKILL can't be ignored.
 *
 * The list is the window manager's (_NET_CLIENT_LIST): one row per
 * program (_NET_WM_PID), named by its window class and its first window's
 * title. Windows that are part of the desktop (the icons, the Recycle Bin,
 * anything not on the taskbar) are left out, and so is this one. If the
 * program leads its own process group (started from the menu, it does),
 * the whole group goes, so a browser's helper processes go with it.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define ROWS_MAX 64

struct prog {
    pid_t pid;
    char name[64];                  /* "Chromium" */
    char title[128];                /* its first window's title */
};

static GtkWidget *win, *list, *quit_btn;
static struct prog progs[ROWS_MAX];
static int nprogs;

static Display *dpy(void) { return gdk_x11_get_default_xdisplay(); }

/* A window property as raw items, or NULL (free with XFree). */
static unsigned char *prop(Window w, const char *name, Atom type, unsigned long *n)
{
    Atom real;
    int fmt;
    unsigned long after;
    unsigned char *data = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy(), w, XInternAtom(dpy(), name, False), 0, 4096, False, type,
                           &real, &fmt, n, &after, &data) != Success || !data) {
        *n = 0;
        return NULL;
    }
    return data;
}

static int has_atom(Window w, const char *list_name, const char *atom)
{
    unsigned long n;
    Atom *a = (Atom *)prop(w, list_name, XA_ATOM, &n);
    Atom want = XInternAtom(dpy(), atom, False);
    int found = 0;
    for (unsigned long k = 0; a && k < n; k++)
        found |= a[k] == want;
    if (a)
        XFree(a);
    return found;
}

static void title_of(Window w, char *out, size_t size)
{
    unsigned long n;
    unsigned char *t = prop(w, "_NET_WM_NAME", XInternAtom(dpy(), "UTF8_STRING", False), &n);
    if (!t)
        t = prop(w, "WM_NAME", XA_STRING, &n);
    snprintf(out, size, "%.*s", (int)n, t ? (char *)t : "");
    if (t)
        XFree(t);
}

/* WM_CLASS is "instance\0Class\0": the Class, as programs name themselves. */
static void class_of(Window w, char *out, size_t size)
{
    unsigned long n;
    char *c = (char *)prop(w, "WM_CLASS", XA_STRING, &n);
    out[0] = 0;
    if (c && n) {
        size_t first = strnlen(c, n);
        const char *cls = first + 1 < n ? c + first + 1 : c;
        snprintf(out, size, "%s", cls);
    }
    if (c)
        XFree(c);
}

static void scan(void)
{
    unsigned long n;
    Window root = DefaultRootWindow(dpy());
    Window *w = (Window *)prop(root, "_NET_CLIENT_LIST", XA_WINDOW, &n);
    Window self = gtk_widget_get_window(win) ? gdk_x11_window_get_xid(gtk_widget_get_window(win)) : 0;
    nprogs = 0;
    for (unsigned long k = 0; w && k < n && nprogs < ROWS_MAX; k++) {
        if (w[k] == self || has_atom(w[k], "_NET_WM_STATE", "_NET_WM_STATE_SKIP_TASKBAR") ||
            has_atom(w[k], "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DESKTOP") ||
            has_atom(w[k], "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"))
            continue;
        unsigned long np;
        long *pid = (long *)prop(w[k], "_NET_WM_PID", XA_CARDINAL, &np);
        pid_t p = pid && np ? (pid_t)pid[0] : 0;
        if (pid)
            XFree(pid);
        if (p <= 1 || p == getpid())
            continue;               /* no pid: nothing we could kill */
        int seen = 0;
        for (int j = 0; j < nprogs; j++)
            seen |= progs[j].pid == p;
        if (seen)
            continue;
        struct prog *g = &progs[nprogs++];
        memset(g, 0, sizeof *g);
        g->pid = p;
        class_of(w[k], g->name, sizeof g->name);
        title_of(w[k], g->title, sizeof g->title);
        if (!g->name[0])
            snprintf(g->name, sizeof g->name, "%s", g->title[0] ? g->title : "(a program)");
    }
    if (w)
        XFree(w);
}

/* Fill the list again, keeping the selected program selected. */
static void refill(void)
{
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    pid_t keep = sel ? (pid_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel), "pid")) : 0;
    scan();
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    for (int i = 0; i < nprogs; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_container_set_border_width(GTK_CONTAINER(box), 6);
        GtkWidget *name = gtk_label_new(progs[i].name);
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        char sub[160];
        snprintf(sub, sizeof sub, "%s  (process %d)", progs[i].title, (int)progs[i].pid);
        GtkWidget *detail = gtk_label_new(sub);
        gtk_widget_set_halign(detail, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(detail), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), detail, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        g_object_set_data(G_OBJECT(row), "pid", GINT_TO_POINTER((int)progs[i].pid));
        g_object_set_data_full(G_OBJECT(row), "name", g_strdup(progs[i].name), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
        if (progs[i].pid == keep)
            gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_show_all(list);
    gtk_widget_set_sensitive(quit_btn, gtk_list_box_get_selected_row(GTK_LIST_BOX(list)) != NULL);
}

/* Every second: refill only if the programs changed (no flicker). */
static gboolean tick(gpointer unused)
{
    (void)unused;
    struct prog before[ROWS_MAX];
    int n = nprogs;
    memcpy(before, progs, sizeof before);
    scan();
    if (n != nprogs || memcmp(before, progs, (size_t)n * sizeof *progs))
        refill();
    return G_SOURCE_CONTINUE;
}

static void selected(GtkListBox *b, GtkListBoxRow *row, gpointer unused)
{
    (void)b, (void)unused;
    gtk_widget_set_sensitive(quit_btn, row != NULL);
}

static void force_quit(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    if (!row)
        return;
    /* Copies: the list may be refilled (rows destroyed) while the
       question below is open. */
    pid_t pid = (pid_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pid"));
    char name[64];
    snprintf(name, sizeof name, "%s", (const char *)g_object_get_data(G_OBJECT(row), "name"));
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, "Force quit “%s”?", name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "It ends at once: changes it has not saved are lost.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL,
                           "Force Quit", GTK_RESPONSE_ACCEPT, NULL);
    int go = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    if (!go)
        return;
    pid_t group = getpgid(pid);
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
        GtkWidget *e = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_CLOSE, "Can't end “%s”: %s.",
                                              name, strerror(errno));
        gtk_dialog_run(GTK_DIALOG(e));
        gtk_widget_destroy(e);
        return;
    }
    /* Its own process group (it leads one): the helpers go too. Never ours. */
    if (group == pid && group != getpgrp())
        kill(-group, SIGKILL);
    refill();
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    g_set_prgname("myos-forcequit");
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Force Quit");
    gtk_window_set_default_size(GTK_WINDOW(win), 420, 360);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(v), 12);
    GtkWidget *head = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(head),
                         "<b>If a program stops answering, choose it and click Force Quit.</b>");
    gtk_label_set_line_wrap(GTK_LABEL(head), TRUE);
    gtk_widget_set_halign(head, GTK_ALIGN_START);
    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    g_signal_connect(list, "row-selected", G_CALLBACK(selected), NULL);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), list);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    quit_btn = gtk_button_new_with_label("Force Quit");
    gtk_style_context_add_class(gtk_widget_get_style_context(quit_btn), "destructive-action");
    g_signal_connect(quit_btn, "clicked", G_CALLBACK(force_quit), NULL);
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), win);
    gtk_box_pack_end(GTK_BOX(buttons), quit_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), sc, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), v);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(win);
    refill();
    g_timeout_add_seconds(1, tick, NULL);
    gtk_main();
    return 0;
}
