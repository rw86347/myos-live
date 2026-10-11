/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * programs.c — All Programs, the cascading folder menu from Windows XP.
 *
 * Hover the All Programs row (or click it, or press Right) and the
 * Programs folder opens beside it. A folder in that list opens another
 * menu to its right; a program runs. The pointer has to rest on a folder
 * for a moment before its menu opens, so sweeping across the list does
 * not flash a menu for every row. That pause is XP's menu show delay.
 *
 * What is listed, in order:
 *   1. /usr/share/myos/programs            the tree shipped with MyOS
 *   2. ~/.local/share/myos/programs        the same tree for this user;
 *      a file replaces one with the same name, Hidden=true removes it
 *   3. /usr/share/applications and
 *      ~/.local/share/applications         a program not already in the
 *      tree, put in a folder by its category (Graphics, Internet, ...)
 *
 * A folder is a directory. A program is a .desktop file: Name, Exec, Icon.
 * Field codes (%f, %U, ...) are dropped; the command is what Exec names.
 */
#include "programs.h"
#include "xpic.h"

#include <Imlib2.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_NODES  384
#define MAX_KIDS   384
#define MAX_PANELS 8
#define MAX_ICONS  64
#define SHOW_MS    400          /* XP's default menu show delay */
#define ROW_PX     48           /* a row, at 100%: the size of the Start menu's own */
#define ICON_PX    32           /* a program's icon in it */
#define SCROLL_MS  140

struct node {
    char name[96];
    char exec[240];             /* empty when this is a folder */
    char icon[64];
    int first, n;               /* children, a slice of kids[] */
    int is_dir;
};

struct icon {
    char name[64];
    uint32_t *px;
    int w, h;
    int missing;                /* looked up, and there is no picture */
};

struct panel {
    Window win;
    Pixmap bg;
    int folder;
    int hover;                  /* row under the pointer, or -1 */
    int scroll;
    int x, y, w, h;
    int row_h, arrow, fit, n;
};

static struct programs_env env;
static struct node nodes[MAX_NODES];
static int kids[MAX_KIDS];
static int nn, nkids, root;
static struct icon icons[MAX_ICONS];
static int nicons;

static struct panel panels[MAX_PANELS];
static int npanels;
static int placed;
static int menu_x, menu_y, bar_limit;
static int anchor_x, anchor_y, anchor_w, anchor_h;

static int armed, arm_panel, arm_row;
static struct timeval arm_at;
static int scroll_panel, scroll_dir;
static struct timeval scroll_at;

static int sc(int n)
{
    int s = env.scale > 0 ? env.scale : 100;
    return (n * s + 50) / 100;
}

static int since_ms(const struct timeval *t)
{
    struct timeval n;
    gettimeofday(&n, NULL);
    return (int)(n.tv_sec - t->tv_sec) * 1000 + (int)(n.tv_usec - t->tv_usec) / 1000;
}

/* --- the tree ------------------------------------------------------------------- */

static int add_node(const char *name, const char *exec, const char *icon, int is_dir)
{
    if (nn >= MAX_NODES)
        return -1;
    struct node *nd = &nodes[nn];
    memset(nd, 0, sizeof *nd);
    snprintf(nd->name, sizeof nd->name, "%s", name ? name : "");
    snprintf(nd->exec, sizeof nd->exec, "%s", exec ? exec : "");
    snprintf(nd->icon, sizeof nd->icon, "%s", icon ? icon : "");
    nd->is_dir = is_dir;
    return nn++;
}

static void append_child(int folder, int child)
{
    if (folder < 0 || child < 0 || nkids >= MAX_KIDS)
        return;
    struct node *f = &nodes[folder];
    if (f->n > 0 && f->first + f->n != nkids) {
        if (nkids + f->n >= MAX_KIDS)
            return;
        int nf = nkids;
        for (int i = 0; i < f->n; i++)
            kids[nkids++] = kids[f->first + i];
        f->first = nf;
    }
    if (f->n == 0)
        f->first = nkids;
    kids[nkids++] = child;
    f->n++;
}

static int find_child(int folder, const char *name)
{
    struct node *f = &nodes[folder];
    for (int i = 0; i < f->n; i++) {
        struct node *c = &nodes[kids[f->first + i]];
        if (c->name[0] && !strcasecmp(c->name, name))
            return kids[f->first + i];
    }
    return -1;
}

/* Exec with %f and the other field codes taken out, and the spaces those
   left behind folded up. "firefox %u" and "firefox %U" are the same program. */
static void norm_exec(const char *in, char *out, size_t n)
{
    size_t w = 0;
    int sp = 1;
    for (const char *r = in; *r && w + 1 < n; r++) {
        if (*r == '%' && r[1]) {
            if (r[1] != '%') {
                r++;
                continue;
            }
            r++;
        }
        if (isspace((unsigned char)*r)) {
            if (sp)
                continue;
            sp = 1;
            out[w++] = ' ';
            continue;
        }
        sp = 0;
        out[w++] = *r;
    }
    while (w && out[w - 1] == ' ')
        w--;
    out[w] = 0;
}

static int exec_taken(const char *exec)
{
    char want[240];
    norm_exec(exec, want, sizeof want);
    if (!want[0])
        return 1;
    for (int i = 0; i < nn; i++) {
        char have[240];
        /* A hidden entry keeps its command and stays out of the menu, so the
           applications scan does not put that program straight back. */
        if (nodes[i].is_dir || !nodes[i].exec[0])
            continue;
        norm_exec(nodes[i].exec, have, sizeof have);
        if (!strcasecmp(have, want))
            return 1;
    }
    return 0;
}

static void unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            if (*r == 's' || *r == 'n' || *r == 't')
                *w++ = ' ';
            else
                *w++ = *r;
            r++;
        } else
            *w++ = *r++;
    }
    *w = 0;
}

static void trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
}

static int on(const char *v)
{
    return !strcmp(v, "true") || !strcmp(v, "1");
}

static int can_run(const char *cmd)
{
    if (!cmd || !cmd[0])
        return 0;
    if (strchr(cmd, '/'))
        return access(cmd, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin:/usr/local/bin";
    char buf[512];
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t n = colon ? (size_t)(colon - path) : strlen(path);
        if (n + strlen(cmd) + 2 < sizeof buf) {
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, cmd);
            if (access(buf, X_OK) == 0)
                return 1;
        }
        if (!colon)
            break;
        path = colon + 1;
    }
    return 0;
}

struct desk {
    char name[96];
    char exec[240];
    char icon[64];
    char cats[192];
    char tryexec[96];
    int hidden, nodisplay, terminal, is_app;
};

static int read_desktop(const char *path, struct desk *d)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    memset(d, 0, sizeof *d);
    d->is_app = 1;
    int in = 0, seen = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == 0)
            continue;
        if (line[0] == '[') {
            in = !strncmp(line, "[Desktop Entry]", 15);
            if (in)
                seen = 1;
            else if (seen)
                break;
            continue;
        }
        if (!in)
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        unescape(val);
        trim(val);
        if (!strcmp(key, "Name") && !d->name[0])
            snprintf(d->name, sizeof d->name, "%s", val);
        else if (!strcmp(key, "Exec"))
            snprintf(d->exec, sizeof d->exec, "%s", val);
        else if (!strcmp(key, "Icon"))
            snprintf(d->icon, sizeof d->icon, "%s", val);
        else if (!strcmp(key, "Categories"))
            snprintf(d->cats, sizeof d->cats, "%s", val);
        else if (!strcmp(key, "TryExec"))
            snprintf(d->tryexec, sizeof d->tryexec, "%s", val);
        else if (!strcmp(key, "NoDisplay"))
            d->nodisplay = on(val);
        else if (!strcmp(key, "Hidden"))
            d->hidden = on(val);
        else if (!strcmp(key, "Terminal"))
            d->terminal = on(val);
        else if (!strcmp(key, "Type"))
            d->is_app = !strcmp(val, "Application");
    }
    fclose(f);
    if (!d->is_app || d->nodisplay)
        return 0;
    /* A hidden entry still has to come through, so the user's copy can
       cross the system one off the menu. */
    if (d->tryexec[0] && !d->hidden && !can_run(d->tryexec))
        return 0;
    return d->hidden || d->exec[0] || d->name[0];
}

/* The command that actually runs. A Terminal=true program gets a terminal
   around it, with the command in single quotes so spaces stay one argument. */
static void launch_exec(const struct desk *d, char *out, size_t n)
{
    char plain[240];
    norm_exec(d->exec, plain, sizeof plain);
    if (!d->terminal || !strncmp(plain, "xterm", 5)) {
        snprintf(out, n, "%s", plain);
        return;
    }
    char q[300];
    size_t w = 0;
    q[w++] = '\'';
    for (const char *r = plain; *r && w + 6 < sizeof q; r++) {
        if (*r == '\'') {
            memcpy(q + w, "'\\''", 4);
            w += 4;
        } else
            q[w++] = *r;
    }
    q[w++] = '\'';
    q[w] = 0;
    snprintf(out, n, "xterm -e sh -c %s", q);
}

static int add_program(int folder, const struct desk *d)
{
    char exec[240];
    if (!d->exec[0] || exec_taken(d->exec))
        return -1;
    launch_exec(d, exec, sizeof exec);
    if (!exec[0])
        return -1;
    int c = add_node(d->name, exec, d->icon, 0);
    if (c >= 0)
        append_child(folder, c);
    return c;
}

static int ensure_folder(int parent, const char *name)
{
    int c = find_child(parent, name);
    if (c >= 0 && nodes[c].is_dir)
        return c;
    c = add_node(name, "", "folder", 1);
    if (c >= 0)
        append_child(parent, c);
    return c;
}

static int ends_with(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

static void filename_name(const char *file, char *out, size_t n)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    snprintf(out, n, "%s", base);
    if (ends_with(out, ".desktop"))
        out[strlen(out) - 8] = 0;
}

static void scan_dir(int folder, const char *path, int depth)
{
    DIR *d;
    struct dirent *de;
    if (depth > 8 || !(d = opendir(path)))
        return;
    while ((de = readdir(d))) {
        char full[512];
        struct stat st;
        if (de->d_name[0] == '.')
            continue;
        if (snprintf(full, sizeof full, "%s/%s", path, de->d_name) >= (int)sizeof full)
            continue;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            int c = ensure_folder(folder, de->d_name);
            if (c >= 0)
                scan_dir(c, full, depth + 1);
        } else if (ends_with(de->d_name, ".desktop")) {
            struct desk desk;
            if (!read_desktop(full, &desk) || desk.hidden || !desk.exec[0])
                continue;
            if (!desk.name[0])
                filename_name(de->d_name, desk.name, sizeof desk.name);
            add_program(folder, &desk);
        }
    }
    closedir(d);
}

/* The user's tree wins where the names match. Hidden=true crosses a name
   out of the menu, which is how you drop a program you don't want. */
static void merge_dir(int folder, const char *path, int depth)
{
    DIR *d;
    struct dirent *de;
    if (depth > 8 || !(d = opendir(path)))
        return;
    while ((de = readdir(d))) {
        char full[512];
        struct stat st;
        if (de->d_name[0] == '.')
            continue;
        if (snprintf(full, sizeof full, "%s/%s", path, de->d_name) >= (int)sizeof full)
            continue;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            int c = ensure_folder(folder, de->d_name);
            if (c >= 0)
                merge_dir(c, full, depth + 1);
            continue;
        }
        if (!ends_with(de->d_name, ".desktop"))
            continue;
        struct desk desk;
        if (!read_desktop(full, &desk))
            continue;
        if (!desk.name[0])
            filename_name(de->d_name, desk.name, sizeof desk.name);
        int c = find_child(folder, desk.name);
        if (desk.hidden) {
            if (c >= 0)
                nodes[c].name[0] = 0;
            continue;
        }
        if (!desk.exec[0])
            continue;
        if (c >= 0 && !nodes[c].is_dir) {
            char exec[240];
            launch_exec(&desk, exec, sizeof exec);
            snprintf(nodes[c].exec, sizeof nodes[c].exec, "%s", exec);
            snprintf(nodes[c].icon, sizeof nodes[c].icon, "%s", desk.icon);
        } else if (c < 0)
            add_program(folder, &desk);
    }
    closedir(d);
}

static void compact_folder(int folder)
{
    int first = nodes[folder].first, n = nodes[folder].n, w = first;
    for (int i = 0; i < n; i++) {
        int c = kids[first + i];
        if (!nodes[c].name[0])
            continue;
        if (nodes[c].is_dir) {
            compact_folder(c);
            if (!nodes[c].n)
                continue;           /* nothing left in it: no "Empty" folder */
        }
        kids[w++] = c;
    }
    nodes[folder].n = w - first;
}

static int cmp_kid(const void *a, const void *b)
{
    const struct node *na = &nodes[*(const int *)a];
    const struct node *nb = &nodes[*(const int *)b];
    int c = strcasecmp(na->name, nb->name);
    if (c)
        return c;
    return nb->is_dir - na->is_dir;
}

static void sort_folder(int folder)
{
    if (nodes[folder].n > 1)
        qsort(kids + nodes[folder].first, (size_t)nodes[folder].n, sizeof(int), cmp_kid);
    for (int i = 0; i < nodes[folder].n; i++) {
        int c = kids[nodes[folder].first + i];
        if (nodes[c].is_dir)
            sort_folder(c);
    }
}

/* Where a .desktop from the flat applications directory belongs. The names
   are the ones XP used for the same kinds of program. */
static int category_folder(const char *cats)
{
    static const struct { const char *key, *folder; } map[] = {
        { "Game", "Games" },
        { "Graphics", "Graphics" },
        { "Viewer", "Graphics" },
        { "WebBrowser", "Internet" },
        { "Network", "Internet" },
        { "Office", "Office" },
        { "WordProcessor", "Office" },
        { "AudioVideo", "Entertainment" },
        { "Audio", "Entertainment" },
        { "Video", "Entertainment" },
        { "Settings", "System Tools" },
        { "System", "System Tools" },
        { "Development", "Programming" },
        { "TextEditor", "Accessories" },
        { "TerminalEmulator", "Accessories" },
        { "Utility", "Accessories" },
    };
    char buf[192];
    if (!cats || !cats[0])
        return root;
    snprintf(buf, sizeof buf, "%s", cats);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        while (*tok == ' ')
            tok++;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(tok, map[i].key))
                return ensure_folder(root, map[i].folder);
    }
    return root;
}

/* A program of this name is in the tree already (shown, or hidden on
   purpose): the applications directory doesn't add another. Their commands
   can differ in a detail ("mplayer --" and "mplayer"), which exec_taken
   alone let through as a second entry. */
static int name_taken(const char *name)
{
    for (int i = 0; i < nn; i++)
        if (!nodes[i].is_dir && !strcasecmp(nodes[i].name, name))
            return 1;
    return 0;
}

static void scan_applications(const char *dir)
{
    DIR *d;
    struct dirent *de;
    if (!(d = opendir(dir)))
        return;
    while ((de = readdir(d))) {
        char full[512];
        struct desk desk;
        if (de->d_name[0] == '.')
            continue;
        if (!ends_with(de->d_name, ".desktop"))
            continue;
        if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >= (int)sizeof full)
            continue;
        if (!read_desktop(full, &desk) || desk.hidden || !desk.exec[0])
            continue;
        if (!desk.name[0])
            filename_name(de->d_name, desk.name, sizeof desk.name);
        if (name_taken(desk.name))
            continue;
        add_program(category_folder(desk.cats), &desk);
    }
    closedir(d);
}

static void user_path(char *out, size_t n, const char *tail)
{
    struct passwd *pw = getpwuid(getuid());
    const char *home = pw && pw->pw_dir ? pw->pw_dir : "/home";
    snprintf(out, n, "%s/%s", home, tail);
}

void programs_load(const struct programs_env *e)
{
    char path[512];
    env = *e;
    nn = nkids = 0;
    root = add_node("", "", "folder", 1);
    if (root < 0)
        return;
    scan_dir(root, "/usr/share/myos/programs", 0);
    user_path(path, sizeof path, ".local/share/myos/programs");
    merge_dir(root, path, 0);
    compact_folder(root);
    scan_applications("/usr/share/applications");
    user_path(path, sizeof path, ".local/share/applications");
    scan_applications(path);
    /* Again: a category folder made for a program that then turned out to
       be listed already (or hidden) would otherwise stay, empty. */
    compact_folder(root);
    sort_folder(root);
    programs_close();
}

/* --- pictures -------------------------------------------------------------------- */

static const char *theme_icon(const char *name)
{
    const char *base;
    static const struct { const char *from, *to; } map[] = {
        { "firefox", "web-browser" },
        { "chromium", "web-browser" },
        { "myos-browser", "web-browser" },
        { "Navigator", "web-browser" },
        { "preferences-system", "settings" },
        { "utilities-terminal", "terminal" },
        { "xterm", "terminal" },
        { "accessories-text-editor", "text-editor" },
        { "geany", "text-editor" },
        { "abiword", "text-editor" },
        { "system-file-manager", "my-files" },
        { "user-home", "my-files" },
        { "folder", "folder" },
    };
    if (!name || !name[0])
        return "binary-file";
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(base, map[i].from))
            return map[i].to;
    return base;
}

static struct icon *load_icon(const char *name)
{
    char path[256];
    const char *theme = theme_icon(name);
    const char *base = name ? (strrchr(name, '/') ? strrchr(name, '/') + 1 : name) : theme;
    for (int i = 0; i < nicons; i++)
        if (!strcmp(icons[i].name, theme))
            return icons[i].missing ? NULL : &icons[i];
    if (nicons >= MAX_ICONS)
        return NULL;
    struct icon *ic = &icons[nicons++];
    memset(ic, 0, sizeof *ic);
    snprintf(ic->name, sizeof ic->name, "%s", theme);
    const char *tries[8];
    int nt = 0;
    snprintf(path, sizeof path, "/usr/share/myos/icons/48/%s.png", theme);
    tries[nt++] = path;
    char path2[256], path3[256], path4[256], path5[256];
    snprintf(path2, sizeof path2, "/usr/share/myos/icons/48/%s.png", base);
    tries[nt++] = path2;
    snprintf(path3, sizeof path3, "/usr/share/icons/hicolor/48x48/apps/%s.png", base);
    tries[nt++] = path3;
    snprintf(path4, sizeof path4, "/usr/share/icons/hicolor/64x64/apps/%s.png", base);
    tries[nt++] = path4;
    char path6[256], path7[256];
    snprintf(path6, sizeof path6, "/usr/share/icons/hicolor/128x128/apps/%s.png", base);
    tries[nt++] = path6;
    snprintf(path7, sizeof path7, "/usr/share/icons/hicolor/32x32/apps/%s.png", base);
    tries[nt++] = path7;
    snprintf(path5, sizeof path5, "/usr/share/pixmaps/%s.png", base);
    tries[nt++] = path5;
    Imlib_Image im = NULL;
    for (int i = 0; i < nt && !im; i++)
        im = imlib_load_image(tries[i]);
    if (!im) {
        ic->missing = 1;
        return NULL;
    }
    imlib_context_set_image(im);
    ic->w = imlib_image_get_width();
    ic->h = imlib_image_get_height();
    ic->px = malloc((size_t)ic->w * ic->h * 4);
    if (!ic->px) {
        imlib_free_image();
        ic->missing = 1;
        return NULL;
    }
    memcpy(ic->px, imlib_image_get_data_for_reading_only(), (size_t)ic->w * ic->h * 4);
    imlib_free_image();
    return ic;
}

static void blend(uint32_t *dst, int dw, int dh, struct icon *ic, int ox, int oy, int size)
{
    if (!ic || !ic->px || ic->w < 1 || ic->h < 1 || size <= 0)
        return;
    for (int j = 0; j < size; j++) {
        int sy = j * ic->h / size;
        if (oy + j < 0 || oy + j >= dh)
            continue;
        for (int i = 0; i < size; i++) {
            int sx = i * ic->w / size;
            if (ox + i < 0 || ox + i >= dw)
                continue;
            uint32_t s = ic->px[(size_t)sy * ic->w + sx], a = s >> 24;
            uint32_t *d = &dst[(size_t)(oy + j) * dw + ox + i];
            if (!a)
                continue;
            if (a == 255) {
                *d = s | 0xff000000u;
                continue;
            }
            uint32_t r = ((s >> 16 & 0xff) * a + (*d >> 16 & 0xff) * (255 - a)) / 255;
            uint32_t g = ((s >> 8 & 0xff) * a + (*d >> 8 & 0xff) * (255 - a)) / 255;
            uint32_t b = ((s & 0xff) * a + (*d & 0xff) * (255 - a)) / 255;
            *d = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
}

static void fill(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t c)
{
    for (int j = 0; j < rh; j++) {
        if (y + j < 0 || y + j >= h)
            continue;
        for (int i = 0; i < rw; i++) {
            if (x + i < 0 || x + i >= w)
                continue;
            px[(size_t)(y + j) * w + x + i] = c;
        }
    }
}

/* Menu highlight, top #3aa7ae to bottom #1f7f86, from the theme. */
static uint32_t highlight(int t, int n)
{
    if (n < 1)
        n = 1;
    int r = 0x3a + (0x1f - 0x3a) * t / n;
    int g = 0xa7 + (0x7f - 0xa7) * t / n;
    int b = 0xae + (0x86 - 0xae) * t / n;
    return 0xff000000u | (r & 255) << 16 | (g & 255) << 8 | (b & 255);
}

static void triangle(uint32_t *px, int w, int h, int cx, int cy, int size, int dir, uint32_t c)
{
    for (int j = -size; j <= size; j++)
        for (int i = -size; i <= size; i++) {
            int on = 0;
            if (dir == 0)                       /* right */
                on = i >= 0 && abs(j) <= size - i;
            else if (dir < 0)                   /* up */
                on = j <= 0 && abs(i) <= size + j;
            else                                /* down */
                on = j >= 0 && abs(i) <= size - j;
            if (!on || cx + i < 0 || cy + j < 0 || cx + i >= w || cy + j >= h)
                continue;
            px[(size_t)(cy + j) * w + cx + i] = c;
        }
}

static int text_width(const char *s)
{
    XGlyphInfo ext;
    if (!env.font || !s)
        return 0;
    XftTextExtentsUtf8(env.dpy, env.font, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}

static int child_at(int folder, int row)
{
    if (row < 0 || row >= nodes[folder].n)
        return -1;
    return kids[nodes[folder].first + row];
}

static void layout(struct panel *p, int max_h)
{
    int widest = 0;
    p->row_h = sc(ROW_PX);
    if (p->row_h < 36)
        p->row_h = 36;
    p->n = nodes[p->folder].n;
    for (int i = 0; i < p->n; i++) {
        int tw = text_width(nodes[child_at(p->folder, i)].name);
        if (tw > widest)
            widest = tw;
    }
    if (p->n == 0)
        widest = text_width("Empty");
    int pw = sc(8) + sc(ICON_PX) + sc(10) + widest + sc(8) + sc(14) + sc(6);
    if (pw < sc(240))
        pw = sc(240);
    if (pw > sc(460))
        pw = sc(460);
    int rows = p->n > 0 ? p->n : 1;
    int max_rows = max_h / p->row_h;
    if (max_rows < 3)
        max_rows = 3;
    if (rows > max_rows) {
        p->arrow = p->row_h;
        p->fit = max_rows - 2;
        if (p->fit < 1)
            p->fit = 1;
    } else {
        p->arrow = 0;
        p->fit = rows;
    }
    if (p->scroll > p->n - p->fit)
        p->scroll = p->n - p->fit;
    if (p->scroll < 0)
        p->scroll = 0;
    p->w = pw + 2;
    p->h = (p->n > 0 ? p->fit : 1) * p->row_h + 2 * p->arrow + 2;
}

static void paint(struct panel *p)
{
    int w = p->w, h = p->h;
    uint32_t *px = calloc((size_t)w * h, 4);
    if (!px)
        return;
    fill(px, w, h, 0, 0, w, h, 0xff17666c);          /* the highlight's border */
    fill(px, w, h, 1, 1, w - 2, h - 2, 0xfffbfbf9);  /* panel white */
    int icon = sc(ICON_PX);
    int body = p->arrow;
    if (p->arrow) {
        triangle(px, w, h, w / 2, p->arrow / 2, sc(4), -1, p->scroll > 0 ? 0xff1f6d72 : 0xffc5c1b8);
        triangle(px, w, h, w / 2, h - p->arrow / 2, sc(4), 1,
                 p->scroll + p->fit < p->n ? 0xff1f6d72 : 0xffc5c1b8);
    }
    int shown = p->n > 0 ? p->fit : 1;
    for (int i = 0; i < shown; i++) {
        int y = 1 + body + i * p->row_h;
        int row = p->scroll + i;
        int hot = p->n > 0 && row == p->hover;
        if (hot)
            for (int t = 0; t < p->row_h && y + t < h - 1; t++)
                fill(px, w, h, 1, y + t, w - 2, 1, highlight(t, p->row_h - 1));
        if (p->n > 0) {
            struct node *nd = &nodes[child_at(p->folder, row)];
            const char *iname = nd->is_dir ? "folder" : nd->icon;
            struct icon *ic = load_icon(iname);
            if (!ic && !nd->is_dir)
                ic = load_icon("run");      /* no picture of its own: a program's */
            blend(px, w, h, ic, sc(8), y + (p->row_h - icon) / 2, icon);
            if (nd->is_dir)
                triangle(px, w, h, w - sc(12), y + p->row_h / 2, sc(4), 0,
                         hot ? 0xffffffff : 0xff1f6d72);
        }
    }
    Pixmap old = p->bg;
    p->bg = xpic_pixmap(env.dpy, env.vis, env.depth, env.root, px, w, h);
    free(px);
    if (!p->bg) {
        p->bg = old;
        return;
    }
    if (env.font) {
        XftDraw *draw = XftDrawCreate(env.dpy, p->bg, env.vis, env.cmap);
        XRenderColor rc[3] = {
            { 0xffff, 0xffff, 0xffff, 0xffff },
            { 0x1d1d, 0x2424, 0x3030, 0xffff },
            { 0x5b5b, 0x6565, 0x7373, 0xffff },
        };
        XftColor col[3];
        for (int i = 0; i < 3; i++)
            XftColorAllocValue(env.dpy, env.vis, env.cmap, &rc[i], &col[i]);
        for (int i = 0; i < shown; i++) {
            int y = 1 + body + i * p->row_h;
            int row = p->scroll + i;
            const char *s = p->n > 0 ? nodes[child_at(p->folder, row)].name : "Empty";
            int hot = p->n > 0 && row == p->hover;
            int ci = p->n == 0 ? 2 : (hot ? 0 : 1);
            XRectangle clip = { (short)(sc(8) + icon + sc(10)), (short)y,
                                (unsigned short)(w - sc(40) - icon), (unsigned short)p->row_h };
            if (clip.width > 0) {
                XftDrawSetClipRectangles(draw, 0, 0, &clip, 1);
                XftDrawStringUtf8(draw, &col[ci], env.font, clip.x,
                                  y + p->row_h / 2 + env.font->ascent / 2 - 1,
                                  (const FcChar8 *)s, (int)strlen(s));
            }
        }
        for (int i = 0; i < 3; i++)
            XftColorFree(env.dpy, env.vis, env.cmap, &col[i]);
        XftDrawDestroy(draw);
    }
    if (!p->win) {
        XSetWindowAttributes a = {
            .override_redirect = True,
            .background_pixmap = p->bg,
            .event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask,
        };
        p->win = XCreateWindow(env.dpy, env.root, p->x, p->y, (unsigned)w, (unsigned)h, 0,
                               env.depth, InputOutput, env.vis,
                               CWOverrideRedirect | CWBackPixmap | CWEventMask, &a);
        if (p->win) {
            XStoreName(env.dpy, p->win, "All Programs");
            XMapRaised(env.dpy, p->win);
        } else {
            XFreePixmap(env.dpy, p->bg);
            p->bg = None;
        }
    } else {
        XMoveResizeWindow(env.dpy, p->win, p->x, p->y, (unsigned)w, (unsigned)h);
        XSetWindowBackgroundPixmap(env.dpy, p->win, p->bg);
        XClearWindow(env.dpy, p->win);
    }
    if (old)
        XFreePixmap(env.dpy, old);
}

static void place(struct panel *p, int x_left, int x_right, int y)
{
    int x = x_right - sc(4);
    if (x + p->w > env.sw - 4)
        x = x_left - p->w + sc(4);
    if (x < 4)
        x = 4;
    if (y + p->h > bar_limit - 4)
        y = bar_limit - 4 - p->h;
    if (y < 4)
        y = 4;
    p->x = x;
    p->y = y;
}

static int max_panel_h(void)
{
    int m = bar_limit - 8;
    if (m > env.sh - 8)
        m = env.sh - 8;
    return m > sc(80) ? m : sc(80);
}

static void open_folder(int folder, int x_left, int x_right, int y)
{
    struct panel *p;
    if (folder < 0 || npanels >= MAX_PANELS)
        return;
    p = &panels[npanels];
    memset(p, 0, sizeof *p);
    p->folder = folder;
    p->hover = -1;
    layout(p, max_panel_h());
    place(p, x_left, x_right, y);
    paint(p);
    if (p->win)
        npanels++;
}

static void close_from(int i)
{
    if (i < 0)
        i = 0;
    while (npanels > i) {
        struct panel *p = &panels[--npanels];
        if (p->win)
            XDestroyWindow(env.dpy, p->win);
        if (p->bg)
            XFreePixmap(env.dpy, p->bg);
        memset(p, 0, sizeof *p);
    }
    if (npanels == 0) {
        armed = 0;
        scroll_dir = 0;
    } else if (arm_panel >= npanels)
        armed = 0;
    XFlush(env.dpy);
}

void programs_close(void)
{
    if (env.dpy)
        close_from(0);
    else
        npanels = 0;
}

static void open_root(void)
{
    struct panel *p;
    if (!placed || root < 0)
        return;
    close_from(0);
    /* The menu grows up from the All Programs row, as XP's did when the
       list was taller than the space under the row. */
    p = &panels[0];
    memset(p, 0, sizeof *p);
    p->folder = root;
    p->hover = -1;
    layout(p, max_panel_h());
    /* Top lines up with All Programs. place() slides it up if the list
       would run off the bottom of the screen, which it usually does: the
       row sits just above Log off. */
    place(p, anchor_x, anchor_x + anchor_w, anchor_y);
    paint(p);
    if (p->win)
        npanels = 1;
    armed = 0;
}

static void open_child(int panel, int row)
{
    struct panel *parent = &panels[panel];
    int child = child_at(parent->folder, row);
    int y;
    if (child < 0 || !nodes[child].is_dir)
        return;
    close_from(panel + 1);
    y = parent->y + 1 + parent->arrow + (row - parent->scroll) * parent->row_h;
    open_folder(child, parent->x, parent->x + parent->w, y);
    armed = 0;
}

static void reveal(struct panel *p)
{
    if (p->hover < 0)
        return;
    if (p->hover < p->scroll)
        p->scroll = p->hover;
    if (p->hover >= p->scroll + p->fit)
        p->scroll = p->hover - p->fit + 1;
    if (p->scroll < 0)
        p->scroll = 0;
}

static int row_at(struct panel *p, int y)
{
    int top, rows;
    if (p->arrow) {
        if (y < p->arrow)
            return -2;
        if (y >= p->h - p->arrow)
            return -3;
    }
    top = 1 + p->arrow;
    if (y < top)
        return -1;
    rows = (y - top) / p->row_h;
    if (rows < 0 || rows >= p->fit)
        return -1;
    rows += p->scroll;
    if (p->n == 0 || rows >= p->n)
        return -1;
    return rows;
}

static void rest(struct panel *p, int panel, int row)
{
    int child;
    if (row == p->hover)
        return;
    p->hover = row;
    paint(p);
    child = child_at(p->folder, row);
    if (child >= 0 && nodes[child].is_dir) {
        if (panel + 1 < npanels && panels[panel + 1].folder == child) {
            if (armed && arm_panel == panel)
                armed = 0;
            close_from(panel + 2);
        } else {
            armed = 1;
            arm_panel = panel;
            arm_row = row;
            gettimeofday(&arm_at, NULL);
        }
    } else {
        armed = 0;
        close_from(panel + 1);
    }
}

static int panel_of(Window w)
{
    for (int i = 0; i < npanels; i++)
        if (panels[i].win == w)
            return i;
    return -1;
}

static void scroll_by(struct panel *p, int dir)
{
    int next = p->scroll + dir;
    if (next < 0 || next > p->n - p->fit)
        return;
    p->scroll = next;
    paint(p);
}

void programs_place(int mx, int my, int mw, int mh, int ix, int iy, int iw, int ih)
{
    (void)mw;
    menu_x = mx;
    menu_y = my;
    bar_limit = my + mh;
    anchor_x = mx + ix;
    anchor_y = my + iy;
    anchor_w = iw;
    anchor_h = ih;
    placed = iw > 0 && ih > 0;
}

void programs_hover_item(int on)
{
    if (!placed)
        return;
    if (on == 1) {
        if (npanels == 0 && !(armed && arm_panel < 0)) {
            armed = 1;
            arm_panel = -1;
            arm_row = -1;
            gettimeofday(&arm_at, NULL);
        }
        return;
    }
    if (on == 0) {
        armed = 0;
        programs_close();
        return;
    }
    /* On the menu, but not on a row: cancel a not-yet-open flyout. */
    if (npanels == 0 && armed && arm_panel < 0)
        armed = 0;
}

void programs_toggle(void)
{
    if (!placed)
        return;
    if (npanels > 0)
        programs_close();
    else
        open_root();
}

int programs_open(void)
{
    return npanels > 0;
}

int programs_owns(Window w)
{
    return panel_of(w) >= 0;
}

int programs_hit(int x, int y)
{
    for (int i = 0; i < npanels; i++)
        if (x >= panels[i].x && y >= panels[i].y && x < panels[i].x + panels[i].w &&
            y < panels[i].y + panels[i].h)
            return 1;
    return 0;
}

int programs_arm_ms(void)
{
    int ms = -1;
    if (armed) {
        int d = SHOW_MS - since_ms(&arm_at);
        if (d < 0)
            d = 0;
        ms = d;
    }
    if (scroll_dir) {
        int d = SCROLL_MS - since_ms(&scroll_at);
        if (d < 0)
            d = 0;
        if (ms < 0 || d < ms)
            ms = d;
    }
    return ms;
}

void programs_tick(void)
{
    if (armed && since_ms(&arm_at) >= SHOW_MS) {
        if (arm_panel < 0)
            open_root();
        else if (arm_panel < npanels)
            open_child(arm_panel, arm_row);
        else
            armed = 0;
    }
    if (scroll_dir && scroll_panel >= 0 && scroll_panel < npanels && since_ms(&scroll_at) >= SCROLL_MS) {
        scroll_by(&panels[scroll_panel], scroll_dir);
        gettimeofday(&scroll_at, NULL);
    }
}

static int activate(struct panel *p, int row, char *cmd, int cmdlen)
{
    int child = child_at(p->folder, row);
    if (child < 0)
        return 0;
    if (nodes[child].is_dir)
        return 0;
    snprintf(cmd, (size_t)cmdlen, "%s", nodes[child].exec);
    return cmd[0] ? 1 : 0;
}

int programs_event(XEvent *ev, char *cmd, int cmdlen)
{
    int pi = panel_of(ev->xany.window);
    struct panel *p;
    if (pi < 0)
        return 0;
    p = &panels[pi];
    if (ev->type == MotionNotify) {
        int row = row_at(p, ev->xmotion.y);
        if (row == -2 || row == -3) {
            int dir = row == -2 ? -1 : 1;
            if (scroll_dir != dir || scroll_panel != pi) {
                scroll_dir = dir;
                scroll_panel = pi;
                scroll_by(p, dir);
                gettimeofday(&scroll_at, NULL);
            }
            if (p->hover != -1) {
                p->hover = -1;
                paint(p);
            }
            return 0;
        }
        if (scroll_panel == pi)
            scroll_dir = 0;
        if (row >= 0)
            rest(p, pi, row);
        return 0;
    }
    if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
        int row = row_at(p, ev->xbutton.y);
        if (row == -2 || row == -3) {
            scroll_by(p, row == -2 ? -1 : 1);
            return 0;
        }
        if (row >= 0) {
            int child = child_at(p->folder, row);
            if (child >= 0 && nodes[child].is_dir)
                open_child(pi, row);
        }
        return 0;
    }
    if (ev->type == ButtonRelease && ev->xbutton.button == Button1) {
        int row = row_at(p, ev->xbutton.y);
        if (row >= 0)
            return activate(p, row, cmd, cmdlen);
    }
    return 0;
}

int programs_key(unsigned long keysym, char *cmd, int cmdlen)
{
    struct panel *p;
    if (npanels <= 0)
        return 0;
    p = &panels[npanels - 1];
    if (keysym == XK_Escape)
        return -1;
    if (keysym == XK_Left) {
        close_from(npanels - 1);
        return 0;
    }
    if (keysym == XK_Up || keysym == XK_Down) {
        int n = p->n;
        int next;
        if (n <= 0)
            return 0;
        if (p->hover < 0)
            next = keysym == XK_Down ? 0 : n - 1;
        else if (keysym == XK_Down)
            next = (p->hover + 1) % n;
        else
            next = p->hover == 0 ? n - 1 : p->hover - 1;
        p->hover = next;
        reveal(p);
        paint(p);
        /* Right opens a folder. Moving up and down only moves the row, so
           sweeping the list with the keys does not open every folder. */
        armed = 0;
        return 0;
    }
    if (keysym == XK_Right || keysym == XK_Return || keysym == XK_KP_Enter) {
        int child;
        if (p->hover < 0 && p->n > 0) {
            p->hover = 0;
            reveal(p);
            paint(p);
        }
        child = child_at(p->folder, p->hover);
        if (child < 0)
            return 0;
        if (nodes[child].is_dir) {
            open_child(npanels - 1, p->hover);
            if (npanels > 0)
                panels[npanels - 1].hover = nodes[panels[npanels - 1].folder].n > 0 ? 0 : -1;
            if (npanels > 0 && panels[npanels - 1].hover == 0)
                paint(&panels[npanels - 1]);
            return 0;
        }
        if (keysym == XK_Right)
            return 0;
        return activate(p, p->hover, cmd, cmdlen);
    }
    return 0;
}
