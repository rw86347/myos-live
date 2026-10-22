/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-wifi — the Wi-Fi icon in the taskbar's tray.
 *
 * The icon shows the link: dark bars for the signal when connected (1 to
 * 4), grey bars with a yellow star when not but networks are around, grey
 * bars and a red cross when there is no Wi-Fi to talk to. Click it and a
 * menu drops up from the tray, the same bubble as the battery card and the
 * same arrangement as the Mac's Wi-Fi menu: a switch, Personal Hotspot when
 * a phone is nearby, the known networks, Other Networks, and Wi-Fi Settings.
 * Click a known network to join it. One that needs a password asks for it
 * ("Enter the network security key") and says so if it was wrong.
 *
 * wpa_supplicant does the work (scans, joining, the WPA2 handshake, keys);
 * this program only asks it, through its control socket: Unix datagrams to
 * /var/run/wpa_supplicant/wlan0, one command per datagram and one answer
 * back ("STATUS", "SCAN_RESULTS", "ADD_NETWORK", ...) — what wpa_cli
 * speaks, from a small client of our own. A second socket is "attached"
 * and hears wpa_supplicant's events (CTRL-EVENT-CONNECTED, -DISCONNECTED,
 * -SCAN-RESULTS), so the icon changes the moment the link does instead of
 * at the next look.
 *
 * GTK3 (already on the disk), GtkStatusIcon for the tray: IceWM's tray is
 * the XEmbed kind that it speaks. Sizes are points at 100%, times K, the
 * desktop's scale (myos-xsettings), as in the other MyOS programs.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "scale.h"
#include "menu_bubble.h"

#define CTRL_DIR "/var/run/wpa_supplicant"
#define REPLY_MAX 8192                  /* wpa_supplicant answers in up to 4 KiB */
#define NETS_MAX  64

static double K = 1.0;                  /* points at 100% -> pixels */
#define S(x) ((int)((x) * K + 0.5))

/* --- wpa_supplicant's control socket ------------------------------------------------ */

/* One socket: our own name in /tmp (wpa_supplicant answers to it), connected
   to the interface's socket. */
struct ctrl {
    int fd;
    char local[108];
};

static struct ctrl cmd_sock = { -1, "" }, ev_sock = { -1, "" };
static char iface[64];                  /* "wlan0" */
static guint ev_watch;

/* The interface wpa_supplicant serves: the first socket in its directory. */
static int find_iface(void)
{
    DIR *d = opendir(CTRL_DIR);
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d)))
        if (e->d_name[0] != '.' && strncmp(e->d_name, "p2p-dev-", 8) && strlen(e->d_name) < sizeof iface) {
            strcpy(iface, e->d_name);
            found = 0;
        }
    closedir(d);
    return found;
}

static int ctrl_open(struct ctrl *c, const char *tag)
{
    c->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0)
        return -1;
    struct sockaddr_un me = { .sun_family = AF_UNIX }, it = { .sun_family = AF_UNIX };
    snprintf(c->local, sizeof c->local, "/tmp/myos-wifi-%d-%s", (int)getpid(), tag);
    unlink(c->local);
    strcpy(me.sun_path, c->local);
    snprintf(it.sun_path, sizeof it.sun_path, CTRL_DIR "/%s", iface);
    if (bind(c->fd, (struct sockaddr *)&me, sizeof me) < 0 ||
        connect(c->fd, (struct sockaddr *)&it, sizeof it) < 0) {
        close(c->fd);
        unlink(c->local);
        c->fd = -1;
        return -1;
    }
    return 0;
}

static void ctrl_close(struct ctrl *c)
{
    if (c->fd < 0)
        return;
    close(c->fd);
    unlink(c->local);
    c->fd = -1;
}

/* One command and its answer, NUL-terminated. wpa_supplicant answers at
   once; 2 s without an answer means it's gone. Only the attached socket
   gets events, so the first datagram back is the answer. */
static int ctrl_cmd(const char *cmd, char *out, size_t size)
{
    struct ctrl *c = &cmd_sock;
    if (c->fd < 0 || send(c->fd, cmd, strlen(cmd), 0) < 0)
        return -1;
    struct pollfd p = { c->fd, POLLIN, 0 };
    for (;;) {
        if (poll(&p, 1, 2000) <= 0)
            return -1;
        ssize_t n = recv(c->fd, out, size - 1, 0);
        if (n < 0)
            return -1;
        out[n] = '\0';
        if (out[0] != '<')
            return (int)n;
    }
}

/* A command whose answer is "OK". */
static int ctrl_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int ctrl_ok(const char *fmt, ...)
{
    char cmd[512], r[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return ctrl_cmd(cmd, r, sizeof r) > 0 && !strncmp(r, "OK", 2) ? 0 : -1;
}

/* "key=value" lines: the value of key, or "" (copied into out). */
static const char *field(const char *text, const char *key, char *out, size_t size)
{
    size_t kl = strlen(key);
    out[0] = '\0';
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            size_t n = strcspn(l + kl + 1, "\n");
            if (n >= size)
                n = size - 1;
            memcpy(out, l + kl + 1, n);
            out[n] = '\0';
            break;
        }
    return out;
}

/* wpa_supplicant prints a name with \xNN, \\ and \" for the bytes that
   aren't plain text (printf_encode): back to the bytes. */
static size_t decode_ssid(const char *in, size_t inlen, unsigned char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < inlen && n < 32; i++) {
        if (in[i] == '\\' && i + 1 < inlen) {
            i++;
            if (in[i] == 'x' && i + 2 < inlen) {
                unsigned v;
                if (sscanf(in + i + 1, "%2x", &v) == 1)
                    out[n++] = (unsigned char)v;
                i += 2;
            } else {
                out[n++] = in[i] == 'n' ? '\n' : in[i] == 't' ? '\t' : in[i] == 'e' ? 27 : (unsigned char)in[i];
            }
        } else {
            out[n++] = (unsigned char)in[i];
        }
    }
    return n;
}

/* --- what we know ---------------------------------------------------------------------- */

enum sec { SEC_OPEN, SEC_PSK, SEC_SAE, SEC_EAP, SEC_WEP };

/* A network nearby: the strongest access point of each name. */
struct net {
    unsigned char ssid[32];
    size_t ssid_len;
    char shown[3 * 32 + 1];             /* the name to show (UTF-8) */
    int level;                          /* dBm */
    enum sec sec;
    int saved;                          /* wpa_supplicant already has a key for it */
};

static struct net nets[NETS_MAX];
static int n_nets;

static enum { ST_NONE, ST_IDLE, ST_CONNECTING, ST_CONNECTED } state;
static unsigned char cur_ssid[32];      /* connected (or connecting) to */
static size_t cur_ssid_len;
static char cur_shown[3 * 32 + 1];
static char cur_bssid[18];
static int cur_level = -100;
static enum sec cur_sec;

static void shown_name(const unsigned char *ssid, size_t len, char *out, size_t size)
{
    if (g_utf8_validate((const char *)ssid, (gssize)len, NULL)) {
        snprintf(out, size, "%.*s", (int)len, ssid);
        return;
    }
    size_t o = 0;                       /* not text: show the odd bytes as \xNN */
    for (size_t i = 0; i < len && o + 5 < size; i++)
        o += (size_t)snprintf(out + o, size - o, ssid[i] >= 32 && ssid[i] < 127 ? "%c" : "\\x%02x", ssid[i]);
}

static enum sec sec_of(const char *flags)
{
    if (strstr(flags, "-EAP"))
        return SEC_EAP;
    if (strstr(flags, "SAE") && !strstr(flags, "-PSK"))
        return SEC_SAE;
    if (strstr(flags, "WPA"))
        return SEC_PSK;
    if (strstr(flags, "WEP"))
        return SEC_WEP;
    return SEC_OPEN;
}

static void mark_saved(void);

/* SCAN_RESULTS: "bssid / frequency / signal level / flags / ssid", then
   one access point per line, tab-separated. */
static void read_scan(void)
{
    static char r[REPLY_MAX];
    n_nets = 0;
    if (ctrl_cmd("SCAN_RESULTS", r, sizeof r) <= 0)
        return;
    char *save, *line = strtok_r(r, "\n", &save);         /* (the header) */
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *f[5];
        int nf = 0;
        for (char *p = line; nf < 5; nf++) {
            f[nf] = p;
            p = nf < 4 ? strchr(p, '\t') : NULL;
            if (!p)
                break;
            *p++ = '\0';
        }
        if (nf < 4 || !f[4][0])
            continue;                   /* (a hidden network: no name to show) */
        unsigned char ssid[32];
        size_t len = decode_ssid(f[4], strlen(f[4]), ssid);
        int level = atoi(f[2]);
        int k = 0;
        while (k < n_nets && !(nets[k].ssid_len == len && !memcmp(nets[k].ssid, ssid, len)))
            k++;
        if (k == n_nets) {
            if (n_nets == NETS_MAX)
                continue;
            n_nets++;
            memcpy(nets[k].ssid, ssid, len);
            nets[k].ssid_len = len;
            shown_name(ssid, len, nets[k].shown, sizeof nets[k].shown);
            nets[k].level = level;
            nets[k].sec = sec_of(f[3]);
            nets[k].saved = 0;
        } else if (level > nets[k].level) {
            nets[k].level = level;
        }
        if (!strcmp(f[0], cur_bssid))
            cur_level = level;
    }
    /* Strongest first. */
    for (int i = 1; i < n_nets; i++)
        for (int j = i; j > 0 && nets[j].level > nets[j - 1].level; j--) {
            struct net t = nets[j];
            nets[j] = nets[j - 1];
            nets[j - 1] = t;
        }
    mark_saved();
}

/* STATUS: the state, and the network's name; the signal from SIGNAL_POLL. */
static void read_status(void)
{
    static char r[REPLY_MAX];
    char v[160];
    if (ctrl_cmd("STATUS", r, sizeof r) <= 0) {
        state = ST_NONE;
        return;
    }
    field(r, "wpa_state", v, sizeof v);
    state = !strcmp(v, "COMPLETED") ? ST_CONNECTED
          : !strcmp(v, "AUTHENTICATING") || !strcmp(v, "ASSOCIATING") || !strcmp(v, "ASSOCIATED") ||
            !strcmp(v, "4WAY_HANDSHAKE") || !strcmp(v, "GROUP_HANDSHAKE") ? ST_CONNECTING : ST_IDLE;
    field(r, "bssid", cur_bssid, sizeof cur_bssid);
    field(r, "ssid", v, sizeof v);
    cur_ssid_len = decode_ssid(v, strlen(v), cur_ssid);
    shown_name(cur_ssid, cur_ssid_len, cur_shown, sizeof cur_shown);
    field(r, "key_mgmt", v, sizeof v);
    cur_sec = strstr(v, "EAP") ? SEC_EAP : strstr(v, "SAE") ? SEC_SAE : strstr(v, "PSK") ? SEC_PSK
            : SEC_OPEN;
    if (state != ST_CONNECTED)
        return;
    if (ctrl_cmd("SIGNAL_POLL", r, sizeof r) > 0 && field(r, "RSSI", v, sizeof v)[0]) {
        cur_level = atoi(v);
        return;
    }
    for (int i = 0; i < n_nets; i++)       /* (no poll: what the last scan heard) */
        if (nets[i].ssid_len == cur_ssid_len && !memcmp(nets[i].ssid, cur_ssid, cur_ssid_len))
            cur_level = nets[i].level;
}

/* Windows' bars: 4 at -55 dBm or better, then -67, -75, and 1 below that. */
static int bars_for(int dbm)
{
    return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1;
}

/* The saved network of this name: its id, or -1. */
static int known_id(const unsigned char *ssid, size_t len)
{
    static char r[REPLY_MAX];
    if (ctrl_cmd("LIST_NETWORKS", r, sizeof r) <= 0)
        return -1;
    char *save, *line = strtok_r(r, "\n", &save);
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        char *end = strchr(tab + 1, '\t');
        unsigned char s[32];
        size_t n = decode_ssid(tab + 1, end ? (size_t)(end - tab - 1) : strlen(tab + 1), s);
        if (n == len && !memcmp(s, ssid, len))
            return atoi(line);
    }
    return -1;
}

/* Which of the networks nearby are already saved. One LIST_NETWORKS. */
static void mark_saved(void)
{
    static char r[REPLY_MAX];
    for (int i = 0; i < n_nets; i++)
        nets[i].saved = 0;
    if (ctrl_cmd("LIST_NETWORKS", r, sizeof r) <= 0)
        return;
    char *save, *line = strtok_r(r, "\n", &save);
    while ((line = strtok_r(NULL, "\n", &save))) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        char *end = strchr(tab + 1, '\t');
        unsigned char s[32];
        size_t n = decode_ssid(tab + 1, end ? (size_t)(end - tab - 1) : strlen(tab + 1), s);
        for (int i = 0; i < n_nets; i++)
            if (nets[i].ssid_len == n && !memcmp(nets[i].ssid, s, n))
                nets[i].saved = 1;
    }
}

/* A phone sharing its connection. The Mac lists those on their own. */
static int is_hotspot(const struct net *n)
{
    return strstr(n->shown, "iPhone") || strstr(n->shown, "iPad") || strstr(n->shown, "Hotspot");
}

/* --- drawing ----------------------------------------------------------------------------- */

/* Four rising bars in a size x size square, `lit` of them solid. On the
   taskbar (dark) they are white like the theme's other tray icons; in the
   list (white) they are dark. */
static void draw_bars(cairo_t *cr, double x, double y, double size, int lit, int on_dark)
{
    double gap = size / 16, w = (size - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) {
        double h = size * (i + 1) / 4;
        cairo_rectangle(cr, x + i * (w + gap), y + size - h, w, h);
        double c = on_dark ? 1.0 : 0.12;
        cairo_set_source_rgba(cr, c, c, c, i < lit ? 1.0 : on_dark ? 0.35 : 0.3);
        cairo_fill(cr);
    }
}

static GtkStatusIcon *icon;
static int icon_size = 16;
static int frame;                       /* (the "connecting" animation) */
static guint anim_id;                   /* its 300 ms timer, only while connecting */
static gboolean anim(gpointer data);

/* The tray picture for the state now. */
static void update_icon(void)
{
    int s = icon_size;
    if (state == ST_CONNECTING && !anim_id)
        anim_id = g_timeout_add(300, anim, NULL);   /* it stops itself (anim) */
    /* Only when the picture or its tip would change: every new picture goes
       to the X server and makes the panel repaint its tray, and the 5 s
       refresh mostly finds everything as it was. */
    static int last_size = -1, last_state = -1, last_look = -1, last_sec = -1;
    static char last_shown[sizeof cur_shown];
    int look = state == ST_CONNECTED ? bars_for(cur_level) : state == ST_CONNECTING ? frame % 5
             : state == ST_IDLE ? n_nets > 0 : 0;
    if (s == last_size && (int)state == last_state && look == last_look &&
        (int)cur_sec == last_sec && !strcmp(cur_shown, last_shown))
        return;
    last_size = s, last_state = (int)state, last_look = look, last_sec = (int)cur_sec;
    memcpy(last_shown, cur_shown, sizeof last_shown);
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s, s);
    cairo_t *cr = cairo_create(sf);
    double m = s / 8.0;
    if (state == ST_CONNECTED)
        draw_bars(cr, m, m, s - 2 * m, bars_for(cur_level), 1);
    else if (state == ST_CONNECTING)
        draw_bars(cr, m, m, s - 2 * m, frame % 5, 1);
    else
        draw_bars(cr, m, m, s - 2 * m, 0, 1);
    if (state == ST_NONE) {             /* a red cross: no Wi-Fi */
        double r = s * 0.22, cx = s - r - 0.5, cy = s - r - 0.5;
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 0.80, 0.12, 0.12);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, r / 3);
        cairo_move_to(cr, cx - r / 2, cy - r / 2);
        cairo_line_to(cr, cx + r / 2, cy + r / 2);
        cairo_move_to(cr, cx + r / 2, cy - r / 2);
        cairo_line_to(cr, cx - r / 2, cy + r / 2);
        cairo_stroke(cr);
    } else if (state == ST_IDLE && n_nets) {    /* a yellow star: networks around */
        double r = s * 0.2, cx = s - r - 0.5, cy = r + 0.5;
        for (int i = 0; i < 10; i++) {
            double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45 : r;
            if (i == 0)
                cairo_move_to(cr, cx + rr * cos(a), cy + rr * sin(a));
            else
                cairo_line_to(cr, cx + rr * cos(a), cy + rr * sin(a));
        }
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.98, 0.78, 0.10);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.60, 0.42, 0.0);
        cairo_set_line_width(cr, 0.7);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(sf, 0, 0, s, s);
    cairo_surface_destroy(sf);
    gtk_status_icon_set_from_pixbuf(icon, pb);
    g_object_unref(pb);

    char *tip;
    if (state == ST_NONE)
        tip = g_strdup("Wi-Fi\nNot available");
    else if (state == ST_CONNECTED)
        tip = g_strdup_printf("%s\nConnected%s", cur_shown, cur_sec == SEC_OPEN ? "" : ", secured");
    else if (state == ST_CONNECTING)
        tip = g_strdup_printf("%s\nConnecting…", cur_shown);
    else
        tip = g_strdup(n_nets ? "Not connected\nConnections are available" : "Not connected");
    gtk_status_icon_set_tooltip_text(icon, tip);
    g_free(tip);
}

/* --- the list of networks --------------------------------------------------------------- */

static GtkWidget *popup, *card, *body, *scroller, *radio_sw, *note;
static int radio_on = 1, radio_busy, others_open;
static int anchor_x, anchor_y;
static unsigned char sel_ssid[32];      /* the network clicked on */
static size_t sel_len;
static int have_sel;
static int asking;                      /* the password field is showing: don't rebuild */
static GtkWidget *pw_entry, *auto_check, *go_button;
static int pending_id = -1;             /* a network we just added (removed if its key is wrong) */
static char error_text[160];

static void rebuild(void);

static int is_current(const struct net *n)
{
    return state >= ST_CONNECTING && n->ssid_len == cur_ssid_len && !memcmp(n->ssid, cur_ssid, cur_ssid_len);
}

/* The Mac's fan: a dot and up to three arcs. Blue is the network we're on. */
static gboolean draw_fan(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    int lit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "lit"));
    int blue = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "blue"));
    double s = gtk_widget_get_allocated_width(w);
    double cx = s * 0.5, cy = s * 0.82;
    cairo_set_source_rgb(cr, blue ? 0.0 : 0.45, blue ? 0.48 : 0.45, blue ? 1.0 : 0.48);
    cairo_arc(cr, cx, cy, s * 0.07, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_set_line_width(cr, s * 0.09);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 3; i++) {
        cairo_set_source_rgba(cr, blue ? 0.0 : 0.45, blue ? 0.48 : 0.45, blue ? 1.0 : 0.48,
                              lit >= i + 2 ? 1.0 : 0.28);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, s * (0.24 + i * 0.2), G_PI * 1.18, G_PI * 1.82);
        cairo_stroke(cr);
    }
    return TRUE;
}

/* A small grey padlock, as on the Mac's known networks. */
static gboolean draw_menu_lock(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double s = gtk_widget_get_allocated_width(w);
    cairo_set_source_rgb(cr, 0.56, 0.56, 0.58);
    cairo_set_line_width(cr, s * 0.1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_sub_path(cr);
    cairo_arc(cr, s * 0.5, s * 0.4, s * 0.2, G_PI, 2 * G_PI);
    cairo_stroke(cr);
    cairo_rectangle(cr, s * 0.2, s * 0.42, s * 0.6, s * 0.46);
    cairo_fill(cr);
    return TRUE;
}

/* A phone, for a personal hotspot. */
static gboolean draw_phone(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    (void)unused;
    double s = gtk_widget_get_allocated_height(w);
    double x = s * 0.28, y = s * 0.06, rw = s * 0.44, rh = s * 0.88, r = s * 0.1;
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.28);
    cairo_set_line_width(cr, 1.3);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + rw - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + rw - r, y + rh - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + rh - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_stroke(cr);
    return TRUE;
}

/* SET_NETWORK's text for a name: hex, which needs no quoting rules. */
static void ssid_hex(const unsigned char *ssid, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        sprintf(out + 2 * i, "%02x", ssid[i]);
    out[2 * len] = '\0';
}

/* Join a network: the saved one of that name, or a new one with this
   password (NULL: none needed). "Connect automatically" saves it. */
static void join(const struct net *n, const char *psk)
{
    int id = known_id(n->ssid, n->ssid_len);
    error_text[0] = '\0';
    if (id < 0 || psk) {
        char r[32], hex[65];
        if (id >= 0)
            ctrl_ok("REMOVE_NETWORK %d", id);  /* a new password replaces the old */
        if (ctrl_cmd("ADD_NETWORK", r, sizeof r) <= 0 || (id = atoi(r), r[0] < '0' || r[0] > '9'))
            return;
        ssid_hex(n->ssid, n->ssid_len, hex);
        ctrl_ok("SET_NETWORK %d ssid %s", id, hex);
        if (n->sec == SEC_OPEN) {
            ctrl_ok("SET_NETWORK %d key_mgmt NONE", id);
        } else {
            /* 64 hex digits are the key itself; else a passphrase, quoted
               (wpa_supplicant takes everything up to the last quote). */
            int raw = strlen(psk) == 64 && strspn(psk, "0123456789abcdefABCDEF") == 64;
            ctrl_ok(raw ? "SET_NETWORK %d psk %s" : "SET_NETWORK %d psk \"%s\"", id, psk);
            if (n->sec == SEC_SAE) {
                ctrl_ok("SET_NETWORK %d key_mgmt SAE", id);
                ctrl_ok("SET_NETWORK %d ieee80211w 2", id);
            } else {
                ctrl_ok("SET_NETWORK %d key_mgmt WPA-PSK", id);
            }
        }
        pending_id = id;
    }
    ctrl_ok("SELECT_NETWORK %d", id);
    if (!auto_check || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_check)))
        ctrl_ok("SAVE_CONFIG");
    memcpy(cur_ssid, n->ssid, n->ssid_len);
    cur_ssid_len = n->ssid_len;
    shown_name(cur_ssid, cur_ssid_len, cur_shown, sizeof cur_shown);
    cur_sec = n->sec;
    state = ST_CONNECTING;
}

static const struct net *selected_net(void)
{
    for (int i = 0; i < n_nets; i++)
        if (have_sel && nets[i].ssid_len == sel_len && !memcmp(nets[i].ssid, sel_ssid, sel_len))
            return &nets[i];
    return NULL;
}

static void on_connect(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    const struct net *n = selected_net();
    if (!n)
        return;
    if (n->sec != SEC_OPEN && known_id(n->ssid, n->ssid_len) < 0 && !asking) {
        asking = 1;                     /* first the password */
        rebuild();
        return;
    }
    const char *psk = NULL;
    if (asking) {
        psk = gtk_entry_get_text(GTK_ENTRY(pw_entry));
        size_t len = strlen(psk);
        if (len < 8 || len > 64) {
            snprintf(error_text, sizeof error_text, "The network security key must be 8 to 63 characters.");
            gtk_label_set_text(GTK_LABEL(note), error_text);
            return;
        }
    }
    join(n, psk);
    asking = 0;
    update_icon();
    rebuild();
}

static void on_cancel(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    asking = 0;
    error_text[0] = '\0';
    rebuild();
}

static void on_disconnect(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    ctrl_ok("DISCONNECT");
    read_status();
    update_icon();
    rebuild();
}

/* Click a network: a saved one joins at once. A new locked one asks for
   the key. The one we're already on is left alone (the switch disconnects). */
static gboolean on_net_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    if (e->button != 1)
        return FALSE;
    int i = GPOINTER_TO_INT(data);
    if (i < 0 || i >= n_nets)
        return TRUE;
    struct net *n = &nets[i];
    if (asking && have_sel && n->ssid_len == sel_len && !memcmp(n->ssid, sel_ssid, sel_len))
        return FALSE;                   /* the password field is in this row */
    if (is_current(n))
        return TRUE;
    memcpy(sel_ssid, n->ssid, n->ssid_len);
    sel_len = n->ssid_len;
    have_sel = 1;
    error_text[0] = '\0';
    if (n->sec == SEC_EAP || n->sec == SEC_WEP) {
        asking = 0;
        rebuild();
        return TRUE;
    }
    if (n->sec != SEC_OPEN && !n->saved) {
        asking = 1;
        rebuild();
        return TRUE;
    }
    asking = 0;
    join(n, NULL);
    update_icon();
    rebuild();
    return TRUE;
}

static gboolean on_others(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w, (void)data;
    if (e->button != 1)
        return FALSE;
    others_open = !others_open;
    rebuild();
    return TRUE;
}

static GtkWidget *label(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (cls)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

/* The eye in the password field: show or hide what was typed. */
static void on_reveal(GtkEntry *e, GtkEntryIconPosition pos, GdkEvent *ev, gpointer data)
{
    (void)pos;
    (void)ev;
    (void)data;
    gtk_entry_set_visibility(e, !gtk_entry_get_visibility(e));
}

static GtkWidget *fan_icon(const struct net *n)
{
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, S(18), S(18));
    gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(da), "lit", GINT_TO_POINTER(bars_for(n->level)));
    g_object_set_data(G_OBJECT(da), "blue", GINT_TO_POINTER(is_current(n) ? 1 : 0));
    g_signal_connect(da, "draw", G_CALLBACK(draw_fan), NULL);
    return da;
}

static GtkWidget *click_row(GtkWidget *inner, GCallback press, gpointer data)
{
    GtkWidget *row = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "menu-row");
    gtk_widget_set_margin_start(row, S(6));
    gtk_widget_set_margin_end(row, S(6));
    gtk_container_add(GTK_CONTAINER(row), inner);
    if (press)
        g_signal_connect(row, "button-press-event", press, data);
    return row;
}

/* One network: the fan, the name, a padlock if it wants a key. The one
   that was clicked and needs a password opens a field under the name. */
static GtkWidget *net_row(int i, int phone)
{
    struct net *n = &nets[i];
    int sel = have_sel && n->ssid_len == sel_len && !memcmp(n->ssid, sel_ssid, sel_len);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(2));
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    gtk_container_set_border_width(GTK_CONTAINER(h), S(3));
    if (phone) {
        GtkWidget *p = gtk_drawing_area_new();
        gtk_widget_set_size_request(p, S(18), S(18));
        gtk_widget_set_valign(p, GTK_ALIGN_CENTER);
        g_signal_connect(p, "draw", G_CALLBACK(draw_phone), NULL);
        gtk_box_pack_start(GTK_BOX(h), p, FALSE, FALSE, S(4));
        gtk_box_pack_start(GTK_BOX(h), label(n->shown, "name"), TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(h), fan_icon(n), FALSE, FALSE, S(4));
    } else {
        gtk_box_pack_start(GTK_BOX(h), fan_icon(n), FALSE, FALSE, S(4));
        GtkWidget *name = label(n->shown, "name");
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_pack_start(GTK_BOX(h), name, TRUE, TRUE, 0);
        if (n->sec != SEC_OPEN) {
            GtkWidget *lock = gtk_drawing_area_new();
            gtk_widget_set_size_request(lock, S(14), S(14));
            gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
            g_signal_connect(lock, "draw", G_CALLBACK(draw_menu_lock), NULL);
            gtk_box_pack_end(GTK_BOX(h), lock, FALSE, FALSE, S(6));
        }
    }
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    if (sel && (n->sec == SEC_EAP || n->sec == SEC_WEP))
        gtk_box_pack_start(GTK_BOX(v),
                           label(n->sec == SEC_EAP ? "Enterprise networks aren't supported yet."
                                                   : "WEP networks aren't supported.", "dim"),
                           FALSE, FALSE, 0);
    else if (sel && asking) {
        GtkWidget *ask = label("Enter the network security key", "dim");
        gtk_widget_set_margin_start(ask, S(8));
        gtk_box_pack_start(GTK_BOX(v), ask, FALSE, FALSE, 0);
        pw_entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(pw_entry), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(pw_entry), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(pw_entry), GTK_ENTRY_ICON_SECONDARY, "view-reveal-symbolic");
        gtk_widget_set_margin_start(pw_entry, S(8));
        gtk_widget_set_margin_end(pw_entry, S(8));
        g_signal_connect(pw_entry, "icon-press", G_CALLBACK(on_reveal), NULL);
        g_signal_connect_swapped(pw_entry, "activate", G_CALLBACK(on_connect), NULL);
        gtk_box_pack_start(GTK_BOX(v), pw_entry, FALSE, FALSE, 0);
        if (error_text[0]) {
            note = label(error_text, "error");
            gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
            gtk_widget_set_margin_start(note, S(8));
            gtk_box_pack_start(GTK_BOX(v), note, FALSE, FALSE, 0);
        }
        GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_widget_set_margin_end(bb, S(4));
        gtk_widget_set_margin_bottom(bb, S(4));
        GtkWidget *go = gtk_button_new_with_label("Join");
        go_button = go;
        g_signal_connect(go, "clicked", G_CALLBACK(on_connect), NULL);
        GtkWidget *c = gtk_button_new_with_label("Cancel");
        g_signal_connect(c, "clicked", G_CALLBACK(on_cancel), NULL);
        gtk_box_pack_end(GTK_BOX(bb), go, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(bb), c, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), bb, FALSE, FALSE, 0);
    }
    return click_row(v, G_CALLBACK(on_net_press), GINT_TO_POINTER(i));
}

static void add_section(const char *title)
{
    GtkWidget *l = label(title, "section");
    gtk_widget_set_margin_start(l, S(14));
    gtk_widget_set_margin_top(l, S(6));
    gtk_widget_set_margin_bottom(l, S(1));
    gtk_box_pack_start(GTK_BOX(body), l, FALSE, FALSE, 0);
}

static void add_nets(int (*want)(const struct net *), int phone)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n_nets; i++)
            if (want(&nets[i]) && is_current(&nets[i]) == (pass == 0))
                gtk_box_pack_start(GTK_BOX(body), net_row(i, phone), FALSE, FALSE, 0);
}

static int want_hotspot(const struct net *n) { return is_hotspot(n); }
static int want_known(const struct net *n) { return !is_hotspot(n) && (n->saved || is_current(n)); }
static int want_other(const struct net *n) { return !is_hotspot(n) && !n->saved && !is_current(n); }

static void sync_switch(void)
{
    if (!radio_sw)
        return;
    radio_busy++;
    int available = state != ST_NONE;
    gtk_widget_set_sensitive(radio_sw, available);
    gtk_switch_set_active(GTK_SWITCH(radio_sw), available && radio_on);
    radio_busy--;
}

static void rebuild(void)
{
    if (!body)
        return;
    GList *old = gtk_container_get_children(GTK_CONTAINER(body));
    for (GList *l = old; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
    pw_entry = auto_check = note = go_button = NULL;
    sync_switch();

    int available = state != ST_NONE;
    if (!available) {
        GtkWidget *l = label("Wi-Fi isn't available.", "dim");
        gtk_widget_set_margin_start(l, S(14));
        gtk_widget_set_margin_top(l, S(4));
        gtk_widget_set_margin_bottom(l, S(6));
        gtk_box_pack_start(GTK_BOX(body), l, FALSE, FALSE, 0);
    } else if (radio_on) {
        int hot = 0, known = 0, other = 0;
        for (int i = 0; i < n_nets; i++) {
            if (want_hotspot(&nets[i])) hot++;
            else if (want_known(&nets[i])) known++;
            else other++;
        }
        if (hot) {
            add_section("Personal Hotspot");
            add_nets(want_hotspot, 1);
        }
        if (known || (!hot && !other)) {
            add_section("Known Networks");
            if (known)
                add_nets(want_known, 0);
            else {
                GtkWidget *l = label(n_nets ? "No known networks nearby." : "Looking for networks\u2026", "dim");
                gtk_widget_set_margin_start(l, S(14));
                gtk_widget_set_margin_bottom(l, S(4));
                gtk_box_pack_start(GTK_BOX(body), l, FALSE, FALSE, 0);
            }
        }
        if (other) {
            GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_container_set_border_width(GTK_CONTAINER(h), S(3));
            GtkWidget *name = label("Other Networks", "name");
            gtk_widget_set_hexpand(name, TRUE);
            gtk_widget_set_margin_start(name, S(8));
            gtk_box_pack_start(GTK_BOX(h), name, TRUE, TRUE, 0);
            GtkWidget *chev = label(others_open ? "\u2039" : "\u203a", "name");
            gtk_widget_set_margin_end(chev, S(8));
            gtk_box_pack_end(GTK_BOX(h), chev, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(body), click_row(h, G_CALLBACK(on_others), NULL), FALSE, FALSE, 0);
            if (others_open)
                add_nets(want_other, 0);
        }
    }
    gtk_widget_show_all(body);
    if (pw_entry)
        gtk_widget_grab_focus(pw_entry);
    else if (go_button)
        gtk_widget_grab_focus(go_button);
    if (popup && card && gtk_widget_get_visible(popup))
        menu_place(popup, card, anchor_x, anchor_y);
}

/* --- talking to wpa_supplicant: connecting, events, the clock ---------------------------- */

static void disconnect_ctrl(void)
{
    if (ev_watch)
        g_source_remove(ev_watch);
    ev_watch = 0;
    ctrl_close(&cmd_sock);
    ctrl_close(&ev_sock);
    state = ST_NONE;
    n_nets = 0;
}

static gboolean on_event(gint fd, GIOCondition cond, gpointer data);

/* (Re)connect to wpa_supplicant: it may start after us, or restart. */
static int connect_ctrl(void)
{
    char r[32];
    if (cmd_sock.fd >= 0)
        return 0;
    if (find_iface() < 0 || ctrl_open(&cmd_sock, "cmd") < 0)
        return -1;
    if (ctrl_open(&ev_sock, "ev") < 0 || send(ev_sock.fd, "ATTACH", 6, 0) < 0) {
        disconnect_ctrl();
        return -1;
    }
    struct pollfd p = { ev_sock.fd, POLLIN, 0 };
    ssize_t n = poll(&p, 1, 2000) > 0 ? recv(ev_sock.fd, r, sizeof r - 1, 0) : -1;
    if (n < 2 || strncmp(r, "OK", 2)) {
        disconnect_ctrl();
        return -1;
    }
    ev_watch = g_unix_fd_add(ev_sock.fd, G_IO_IN, on_event, NULL);
    read_status();
    read_scan();
    return 0;
}

static void refresh(void)
{
    if (connect_ctrl() < 0) {
        state = ST_NONE;
    } else {
        read_status();
        if (state == ST_NONE)
            disconnect_ctrl();      /* it went away */
    }
    update_icon();
}

/* wpa_supplicant's events: "<3>CTRL-EVENT-CONNECTED - Connection to ...". */
static gboolean on_event(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond;
    (void)data;
    char m[1024];
    ssize_t n;
    int scan = 0, status = 0;
    while ((n = recv(fd, m, sizeof m - 1, MSG_DONTWAIT)) > 0) {
        m[n] = '\0';
        const char *e = m[0] == '<' && strchr(m, '>') ? strchr(m, '>') + 1 : m;
        if (!strncmp(e, "CTRL-EVENT-SCAN-RESULTS", 23))
            scan = 1;
        else if (!strncmp(e, "CTRL-EVENT-TERMINATING", 22)) {
            ev_watch = 0;               /* (removed by returning FALSE) */
            ctrl_close(&cmd_sock);
            ctrl_close(&ev_sock);
            state = ST_NONE;
            n_nets = 0;
            update_icon();
            rebuild();
            return G_SOURCE_REMOVE;
        } else if (!strncmp(e, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) && strstr(e, "reason=WRONG_KEY")) {
            /* The key was wrong: forget the network we just added, and say so. */
            if (pending_id >= 0) {
                ctrl_ok("REMOVE_NETWORK %d", pending_id);
                ctrl_ok("SAVE_CONFIG");
                pending_id = -1;
            }
            snprintf(error_text, sizeof error_text, "The network security key isn't correct. Please try again.");
            memcpy(sel_ssid, cur_ssid, cur_ssid_len);
            sel_len = cur_ssid_len;
            have_sel = 1;
            asking = 1;
            status = 1;
        } else if (!strncmp(e, "CTRL-EVENT-CONNECTED", 20)) {
            pending_id = -1;
            status = 1;
        } else if (!strncmp(e, "CTRL-EVENT-", 11) || !strncmp(e, "Trying to associate", 19)) {
            status = 1;
        }
    }
    if (status)
        read_status();
    if (scan)
        read_scan();
    if (scan || status) {
        update_icon();
        if (!asking || error_text[0])
            rebuild();
    }
    return G_SOURCE_CONTINUE;
}

/* Every 5 s: the signal for the icon (and wpa_supplicant, if it was gone).
   Every 300 ms while connecting: the animation. */
static gboolean tick(gpointer data)
{
    (void)data;
    refresh();
    return G_SOURCE_CONTINUE;
}

static gboolean anim(gpointer data)
{
    (void)data;
    if (state != ST_CONNECTING) {
        anim_id = 0;                    /* until the next connection (update_icon) */
        return G_SOURCE_REMOVE;
    }
    frame++;
    update_icon();
    return G_SOURCE_CONTINUE;
}

/* --- the window ------------------------------------------------------------------------------ */

static Atom a_scale, a_scale_auto;

static long root_cardinal(Atom a)
{
    Display *d = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    long v = 0;
    if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after,
                           &p) == Success && p && n == 1 && fmt == 32)
        v = *(long *)p;
    if (p)
        XFree(p);
    return v;
}

/* The desktop's scale now (it can change while we run). */
static void update_k(void)
{
    long s = root_cardinal(a_scale);
    if (!s)
        s = root_cardinal(a_scale_auto);
    if (!s)
        s = auto_scale(gdk_screen_get_width(gdk_screen_get_default()));
    K = clamp_scale((int)s) / 100.0 / (s >= 200 ? 2 : 1);
}

static GtkCssProvider *css;

static void restyle(void)
{
    char *c = g_strdup_printf(
        "window { background-color: transparent; }"
        ".title { font-size: %dpx; font-weight: bold; color: #1d1d1f; }"
        ".section { font-size: %dpx; color: #8e8e93; }"
        ".name { font-size: %dpx; color: #1d1d1f; }"
        ".dim { font-size: %dpx; color: #8e8e93; }"
        ".error { font-size: %dpx; color: #c42b1c; }"
        ".menu-row { background-color: transparent; border-radius: %dpx; }"
        ".menu-row:hover { background-color: rgba(0,0,0,0.08); }"
        "separator { background-color: rgba(0,0,0,0.12); min-height: 1px; margin: %dpx %dpx; }"
        "scrolledwindow, viewport { background-color: #e8e8ed; }"
        "button { padding: %dpx %dpx; border-radius: %dpx; }"
        "switch { min-width: %dpx; min-height: %dpx; border-radius: %dpx;"
        "  background-color: #e5e5ea; border: none; box-shadow: none; }"
        "switch:checked { background-color: #007aff; }"
        "switch slider { min-width: %dpx; min-height: %dpx; border-radius: %dpx;"
        "  background-color: white; border: none; }",
        S(13), S(11), S(13), S(12), S(12), S(6), S(6), S(10), S(2), S(10), S(6),
        S(38), S(22), S(11), S(18), S(18), S(9));
    gtk_css_provider_load_from_data(css, c, -1, NULL);
    g_free(c);
}

static void popup_hide(void)
{
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    gdk_seat_ungrab(seat);
    gtk_widget_hide(popup);
    asking = 0;
    error_text[0] = '\0';
}

static gboolean on_focus_out(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w, (void)e, (void)data;
    popup_hide();
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    (void)w, (void)data;
    if (e->keyval == GDK_KEY_Escape) {
        popup_hide();
        return TRUE;
    }
    return FALSE;
}

/* A click outside the bubble ends it (we hold the pointer while it is open). */
static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)data;
    int ww = gtk_widget_get_allocated_width(w), wh = gtk_widget_get_allocated_height(w);
    if (e->x < 0 || e->y < 0 || e->x >= ww || e->y >= wh) {
        popup_hide();
        return TRUE;
    }
    return FALSE;
}

static gboolean on_radio(GtkSwitch *sw, gboolean state, gpointer data)
{
    (void)sw, (void)data;
    if (radio_busy)
        return FALSE;
    radio_on = state;
    if (!state)
        on_disconnect(NULL, NULL);      /* also rebuilds */
    else {
        if (cmd_sock.fd >= 0)
            ctrl_ok("SCAN");
        rebuild();
    }
    return FALSE;
}

static gboolean on_wifi_settings(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w, (void)data;
    if (e->button != 1)
        return FALSE;
    g_spawn_command_line_async("myos-settings wifi", NULL);
    popup_hide();
    return TRUE;
}

static gboolean grab_it(gpointer unused)
{
    (void)unused;
    GdkWindow *gw = gtk_widget_get_window(popup);
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    if (gw && gtk_widget_get_visible(popup))
        gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void make_popup(void)
{
    popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(popup), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(popup), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(popup), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(popup), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(popup), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(popup), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_title(GTK_WINDOW(popup), "Wi-Fi");
    menu_bubble_prepare(popup);

    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(card, S(8));
    gtk_widget_set_margin_bottom(card, MENU_CARET);
    gtk_widget_set_size_request(card, S(292), -1);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    gtk_widget_set_margin_start(head, S(14));
    gtk_widget_set_margin_end(head, S(12));
    gtk_widget_set_margin_bottom(head, S(2));
    gtk_box_pack_start(GTK_BOX(head), label("Wi-Fi", "title"), TRUE, TRUE, 0);
    radio_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(radio_sw), TRUE);
    gtk_widget_set_valign(radio_sw, GTK_ALIGN_CENTER);
    g_signal_connect(radio_sw, "state-set", G_CALLBACK(on_radio), NULL);
    gtk_box_pack_end(GTK_BOX(head), radio_sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), S(360));
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller), GTK_SHADOW_NONE);
    body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scroller), body);
    gtk_box_pack_start(GTK_BOX(card), scroller, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    GtkWidget *settings = label("Wi-Fi Settings\u2026", "name");
    gtk_widget_set_margin_start(settings, S(8));
    gtk_widget_set_margin_top(settings, S(4));
    gtk_widget_set_margin_bottom(settings, S(4));
    GtkWidget *settings_row = click_row(settings, G_CALLBACK(on_wifi_settings), NULL);
    gtk_widget_set_margin_bottom(settings_row, S(4));
    gtk_box_pack_start(GTK_BOX(card), settings_row, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(popup), card);
    g_signal_connect(popup, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(popup, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(popup, "button-press-event", G_CALLBACK(on_button), NULL);
    g_signal_connect(popup, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
}

/* The point of the bubble sits on the Wi-Fi icon. */
static void place_anchor(void)
{
    GdkRectangle area;
    GdkScreen *scr;
    GtkOrientation o;
    if (icon && gtk_status_icon_get_geometry(icon, &scr, &area, &o)) {
        anchor_x = area.x + area.width / 2;
        anchor_y = area.y;
        return;
    }
    GdkRectangle wa = { 0, 0, 1024, 768 };
    GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (!mon)
        mon = gdk_display_get_monitor(gdk_display_get_default(), 0);
    if (mon)
        gdk_monitor_get_workarea(mon, &wa);
    anchor_x = wa.x + wa.width - S(28);
    anchor_y = wa.y + wa.height;
}

/* Open the menu above the tray, and scan, so it fills in with what is around. */
static void show_popup(void)
{
    update_k();
    restyle();
    gtk_widget_set_margin_top(card, S(8));
    gtk_widget_set_size_request(card, S(292), -1);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), S(360));
    have_sel = asking = 0;
    others_open = 0;
    error_text[0] = '\0';
    refresh();
    if (cmd_sock.fd >= 0 && radio_on) {
        read_scan();
        ctrl_ok("SCAN");
    }
    place_anchor();
    gtk_widget_show_all(popup);
    rebuild();
    gtk_window_present(GTK_WINDOW(popup));
    g_timeout_add(50, grab_it, NULL);
}

static void on_activate(GtkStatusIcon *si, gpointer data)
{
    (void)si;
    (void)data;
    if (gtk_widget_get_visible(popup))
        popup_hide();
    else
        show_popup();
}

static void on_popup_menu(GtkStatusIcon *si, guint button, guint time, gpointer data)
{
    (void)button;
    (void)time;
    on_activate(si, data);
}

static gboolean on_size(GtkStatusIcon *si, gint size, gpointer data)
{
    (void)si;
    (void)data;
    icon_size = size > 8 ? size : 16;
    update_icon();
    return TRUE;
}

/* GTK's own handler ends the program on any X error. The tray takes our
   icon's window in as its child (XEmbed), and when the tray goes away
   (myos-menu starts itself over for a new scale) what GTK still holds on
   that window is gone; the icon docks again in the new tray. So an X
   error is noted, never fatal. */
static int x_error(Display *d, XErrorEvent *e)
{
    char msg[80];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    fprintf(stderr, "myos-wifi: X error: %s (request %d), ignored\n", msg, e->request_code);
    return 0;
}

static gboolean on_quit(gpointer data)
{
    (void)data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    Display *d = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XSetErrorHandler(x_error);
    a_scale = XInternAtom(d, "_MYOS_SCALE", False);
    a_scale_auto = XInternAtom(d, "_MYOS_SCALE_AUTO", False);
    update_k();
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    restyle();

    icon = gtk_status_icon_new();
    gtk_status_icon_set_title(icon, "Wi-Fi");
    g_signal_connect(icon, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(icon, "popup-menu", G_CALLBACK(on_popup_menu), NULL);
    g_signal_connect(icon, "size-changed", G_CALLBACK(on_size), NULL);
    make_popup();
    refresh();
    g_timeout_add_seconds(5, tick, NULL);
    g_unix_signal_add(SIGTERM, on_quit, NULL);
    g_unix_signal_add(SIGINT, on_quit, NULL);
    if (argc > 1 && !strcmp(argv[1], "--open"))
        show_popup();                   /* (open the list at once: for screenshots) */
    gtk_main();
    disconnect_ctrl();
    return 0;
}
