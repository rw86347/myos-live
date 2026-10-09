/*
 * myos-install — the nine-page installer (docs/INSTALLER-PLAN.md).
 *
 * GTK on the live desktop, or `myos-install --text` on a machine without
 * graphics. Both write a plan and, only after Install, run the setuid
 * helper. Closing the window leaves the live system as it was.
 *
 * The look follows the Golden Hour mockups in
 * assets/themes/golden-hour/make-installer.py: sand sidebar, teal labels,
 * the same step names, nothing written before the summary page.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cairo.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "disks.h"
#include "plan.h"
#include "scale.h"
#include "worldmap.h"

/* --- tables (the mockup's lists) ------------------------------------------ */

struct lang_opt {
    const char *id, *native, *english, *keyboard, *kb_name, *tz;
    int cjk;
};

static const struct lang_opt LANGS[] = {
    { "en_US.UTF-8", "English (United States)", "English", "us", "English (US)", "America/New_York", 0 },
    { "en_GB.UTF-8", "English (United Kingdom)", "English", "gb", "English (UK)", "Europe/London", 0 },
    { "zh_CN.UTF-8", "中文 (简体)", "Chinese, Simplified", "cn", "Chinese", "Asia/Shanghai", 1 },
    { "zh_TW.UTF-8", "中文 (繁體)", "Chinese, Traditional", "cn", "Chinese", "Asia/Taipei", 1 },
    { "de_DE.UTF-8", "Deutsch", "German", "de", "German", "Europe/Berlin", 0 },
    { "es_ES.UTF-8", "Español", "Spanish", "es", "Spanish", "Europe/Madrid", 0 },
    { "fr_FR.UTF-8", "Français", "French", "fr", "French", "Europe/Paris", 0 },
    { "ja_JP.UTF-8", "日本語", "Japanese", "us", "English (US)", "Asia/Tokyo", 1 },
    { "pt_BR.UTF-8", "Português (Brasil)", "Portuguese", "us", "English (US)", "America/Sao_Paulo", 0 },
};

struct kb_opt { const char *id, *name; };

static const struct kb_opt KBS[] = {
    { "us", "English (US)" },
    { "us(intl)", "English (US, international with dead keys)" },
    { "gb", "English (UK)" },
    { "cn", "Chinese" },
    { "de", "German" },
    { "fr", "French" },
    { "es", "Spanish" },
    { "dvorak", "Dvorak" },
};

/* The time zones are tzdata's own list, read at start-up from
   /usr/share/zoneinfo/zone.tab: one line per zone, giving its country, the
   latitude and longitude of the city it is named after, and its name. Every
   inhabited country has at least one line (countries that share a zone get
   one each, e.g. Europe/Vatican). iso3166.tab names the countries. Sorted by
   region, then city, for the lists; UTC, which is no place and has no dot on
   the map (NOWHERE), comes last. */
#define ZONEINFO "/usr/share/zoneinfo"
#define NOWHERE  999

struct tz_opt {
    const char *id;             /* Asia/Shanghai: what /etc/localtime is copied from */
    const char *country;        /* China (NULL for UTC) */
    char region[16];            /* Asia */
    char city[40];              /* Shanghai: the name's last part, with spaces */
    double lat, lon;            /* degrees, north and east positive */
};

static struct tz_opt *tzs;
static int ntz;

static const char *STEPS[] = {
    "Language", "Keyboard", "Time zone", "Disk", "Partitions",
    "Your account", "Summary", "Install", "Done"
};

static const char *WORK[] = {
    "Making partitions",
    "Making file systems",
    "Copying the system",
    "Setting up your account and language",
    "Making the disk start MyOS",
    "Cleaning up"
};

static const char *WORK_KEY[] = {
    "partition", "filesystems", "copy", "configure", "boot", "finish"
};

/* --- time zones ----------------------------------------------------------- */

/* A whole small file, NUL-terminated; NULL if it can't be read. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    long n;
    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n + 1)))
        buf[fread(buf, 1, (size_t)n, f)] = 0;
    if (f)
        fclose(f);
    return buf;
}

/* Cut buf into lines in place: the next line, and *rest moves past it. */
static char *next_line(char **rest)
{
    char *line = *rest, *nl;
    if (!line || !*line)
        return NULL;
    nl = strchr(line, '\n');
    *rest = nl ? nl + 1 : line + strlen(line);
    if (nl)
        *nl = 0;
    return line;
}

/* One coordinate as zone.tab writes it (ISO 6709): a sign, then degrees
   (deg digits: 2 for a latitude, 3 for a longitude), minutes, and maybe
   seconds — +3114 is 31°14'N, -0740023 is 74°00'23"W. *s moves past it. */
static double iso6709(const char **s, int deg)
{
    const char *p = *s + 1;
    double sign = **s == '-' ? -1 : 1, v = 0, unit = 1;
    int n = 0;
    while (isdigit((unsigned char)p[n]))
        n++;
    for (int i = 0, w = deg; i < n; w = 2, unit *= 60) {
        int part = 0;
        for (int j = 0; j < w && i < n; j++, i++)
            part = part * 10 + (p[i] - '0');
        v += part / unit;
    }
    *s = p + n;
    return sign * v;
}

static int tz_cmp(const void *a, const void *b)
{
    const struct tz_opt *x = a, *y = b;
    int c = strcmp(x->region, y->region);
    return c ? c : strcmp(x->city, y->city);
}

/* Read zone.tab and iso3166.tab into tzs[]. Their buffers are kept: the
   names point into them. Without tzdata the list is just UTC. */
static void tz_load(void)
{
    char *countries = slurp(ZONEINFO "/iso3166.tab"), *zones = slurp(ZONEINFO "/zone.tab");
    const char *cname[26][26] = { { 0 } };     /* country names by their two letters */
    char *rest, *line;
    int cap = 1;                                /* UTC */

    for (rest = countries; (line = next_line(&rest)); )
        if (isupper((unsigned char)line[0]) && isupper((unsigned char)line[1]) && line[2] == '\t')
            cname[line[0] - 'A'][line[1] - 'A'] = line + 3;
    for (const char *s = zones; s && *s; s++)
        cap += *s == '\n';
    tzs = calloc((size_t)cap + 1, sizeof *tzs);
    if (!tzs) {
        fputs("myos-install: out of memory\n", stderr);
        exit(1);
    }
    for (rest = zones; (line = next_line(&rest)); ) {
        char *f[4] = { line };                  /* country, place, zone, comment */
        for (int k = 1; k < 4 && (f[k] = strchr(f[k - 1], '\t')); k++)
            *f[k]++ = 0;
        if (line[0] == '#' || !f[2] || (f[1][0] != '+' && f[1][0] != '-') ||
            !strchr(f[2], '/') || !plan_good_zone(f[2]) || ntz >= cap - 1)
            continue;
        struct tz_opt *t = &tzs[ntz++];
        const char *c = f[1];
        t->lat = iso6709(&c, 2);
        t->lon = iso6709(&c, 3);
        t->id = f[2];
        snprintf(t->region, sizeof t->region, "%.*s", (int)(strchr(f[2], '/') - f[2]), f[2]);
        snprintf(t->city, sizeof t->city, "%s", strrchr(f[2], '/') + 1);
        for (char *u = t->city; *u; u++)
            if (*u == '_')
                *u = ' ';                       /* Buenos_Aires */
        if (isupper((unsigned char)f[0][0]) && isupper((unsigned char)f[0][1]) && !f[0][2])
            t->country = cname[f[0][0] - 'A'][f[0][1] - 'A'];
    }
    qsort(tzs, (size_t)ntz, sizeof *tzs, tz_cmp);
    tzs[ntz++] = (struct tz_opt){ "UTC", NULL, "UTC", "UTC", NOWHERE, NOWHERE };
}

/* The zone called id; UTC (last) if tzdata has none of that name. */
static int tz_find(const char *id)
{
    for (int i = 0; i < ntz; i++)
        if (!strcmp(tzs[i].id, id))
            return i;
    return ntz - 1;
}

/* "Shanghai, China"; only "Singapore" where the city is the country. */
static void tz_label(const struct tz_opt *t, char *out, size_t n)
{
    if (t->country && strcmp(t->country, t->city))
        snprintf(out, n, "%s, %s", t->city, t->country);
    else
        snprintf(out, n, "%s", t->city);
}

/* tzdata calls the oceans' islands by the ocean's first word. */
static const char *region_label(const char *r)
{
    static const char *const oceans[][2] = {
        { "Arctic", "Arctic Ocean" }, { "Atlantic", "Atlantic Ocean" },
        { "Indian", "Indian Ocean" }, { "Pacific", "Pacific Ocean" },
    };
    for (size_t i = 0; i < sizeof oceans / sizeof *oceans; i++)
        if (!strcmp(r, oceans[i][0]))
            return oceans[i][1];
    return r;
}

/* --- compositor measure --------------------------------------------------- */

static double measure_redraw_ms(void)
{
    const int N = 60, W = 1280, H = 800;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(s);
    struct timespec a, b;

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++) {
        cairo_set_source_rgb(cr, (i & 1) ? 0.95 : 0.20, 0.85, 0.55);
        cairo_paint(cr);
        cairo_surface_flush(s);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    return ms / N;
}

static void pick_compositor(struct install_plan *p)
{
    p->compositor_ms = measure_redraw_ms();
    if (access("/dev/dri/renderD128", R_OK) == 0 && p->compositor_ms < 8.0)
        strcpy(p->compositor, "gl");
    else if (p->compositor_ms < 16.0)
        strcpy(p->compositor, "xrender");
    else
        strcpy(p->compositor, "none");
}

static const char *helper_path(void)
{
    if (access("/usr/libexec/myos-install-helper", X_OK) == 0)
        return "/usr/libexec/myos-install-helper";
    if (access("./myos-install-helper", X_OK) == 0)
        return "./myos-install-helper";
    return "/usr/libexec/myos-install-helper";
}

static void apply_layout(const char *kb)
{
    char layout[32], *var, *argv[6];
    pid_t pid;
    int st;

    snprintf(layout, sizeof layout, "%s", kb);
    var = strchr(layout, '(');
    argv[0] = "setxkbmap";
    argv[1] = layout;
    if (var) {
        *var++ = 0;
        char *end = strchr(var, ')');
        if (end)
            *end = 0;
        argv[2] = "-variant";
        argv[3] = var;
        argv[4] = NULL;
    } else {
        argv[2] = NULL;
    }
    pid = fork();
    if (pid == 0) {
        execvp("setxkbmap", argv);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &st, 0);
}

/* --- text mode ------------------------------------------------------------ */

static int read_line(const char *prompt, char *buf, size_t n, int secret)
{
    struct termios old, raw;
    int hide = 0;

    fputs(prompt, stdout);
    fflush(stdout);
    if (secret && isatty(0) && tcgetattr(0, &old) == 0) {
        raw = old;
        raw.c_lflag &= ~(unsigned)ECHO;
        hide = tcsetattr(0, TCSAFLUSH, &raw) == 0;
    }
    if (!fgets(buf, (int)n, stdin)) {
        if (hide)
            tcsetattr(0, TCSAFLUSH, &old);
        return -1;
    }
    if (hide) {
        tcsetattr(0, TCSAFLUSH, &old);
        fputc('\n', stdout);
    }
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int pick(const char *title, int n, int dflt)
{
    char buf[32];
    printf("%s [%d]: ", title, dflt + 1);
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin))
        return dflt;
    if (!buf[0] || buf[0] == '\n')
        return dflt;
    int v = atoi(buf);
    if (v < 1 || v > n)
        return dflt;
    return v - 1;
}

static int text_install(struct install_plan *p, const char *plan_path)
{
    char buf[256], err[256], pass2[PLAN_PASS];
    struct disk_info disks[DISKS_MAX];
    int ndisk, i, lang = 0, kb = 0, tz;
    int apply = 0;

    printf("Install MyOS  (nothing is written until the last question)\n\n");

    printf("Language:\n");
    for (i = 0; i < (int)(sizeof LANGS / sizeof *LANGS); i++)
        printf("  %d) %s  (%s)\n", i + 1, LANGS[i].native, LANGS[i].english);
    lang = pick("Number", (int)(sizeof LANGS / sizeof *LANGS), 0);
    strcpy(p->lang, LANGS[lang].id);
    strcpy(p->lang_name, LANGS[lang].native);
    p->cjk = LANGS[lang].cjk;
    strcpy(p->keyboard, LANGS[lang].keyboard);
    strcpy(p->keyboard_name, LANGS[lang].kb_name);
    strcpy(p->timezone, LANGS[lang].tz);

    if (read_line("Also install fonts and input for Chinese? [Y/n] ", buf, sizeof buf, 0) == 0)
        p->cjk = !(buf[0] == 'n' || buf[0] == 'N');

    printf("\nKeyboard:\n");
    for (i = 0; i < (int)(sizeof KBS / sizeof *KBS); i++) {
        printf("  %d) %s\n", i + 1, KBS[i].name);
        if (!strcmp(KBS[i].id, p->keyboard))
            kb = i;
    }
    kb = pick("Number", (int)(sizeof KBS / sizeof *KBS), kb);
    strcpy(p->keyboard, KBS[kb].id);
    strcpy(p->keyboard_name, KBS[kb].name);

    /* The time zone: a region, then a city in it (America alone has 144).
       tzs[] is sorted by region, so each region is one run of it. */
    printf("\nTime zone, region:\n");
    tz = tz_find(p->timezone);
    {
        int start[ntz], nreg = 0, r = 0, end;
        for (i = 0; i < ntz; i++)
            if (!i || strcmp(tzs[i].region, tzs[i - 1].region)) {
                if (!strcmp(tzs[i].region, tzs[tz].region))
                    r = nreg;
                start[nreg++] = i;
                printf("  %d) %s\n", nreg, region_label(tzs[i].region));
            }
        r = pick("Number", nreg, r);
        end = r + 1 < nreg ? start[r + 1] : ntz;
        printf("\nTime zone, city:\n");
        for (i = start[r]; i < end; i++) {
            tz_label(&tzs[i], buf, sizeof buf);
            printf("  %d) %s\n", i - start[r] + 1, buf);
        }
        tz = start[r] + pick("Number", end - start[r], tz >= start[r] && tz < end ? tz - start[r] : 0);
    }
    snprintf(p->timezone, sizeof p->timezone, "%s", tzs[tz].id);
    if (read_line("Set the time from the network? [Y/n] ", buf, sizeof buf, 0) == 0)
        p->ntp = !(buf[0] == 'n' || buf[0] == 'N');
    if (read_line("This PC also runs Windows (hardware clock in local time)? [y/N] ", buf, sizeof buf, 0) == 0)
        p->hwclock_local = buf[0] == 'y' || buf[0] == 'Y';

    ndisk = disks_scan(disks, DISKS_MAX);
    printf("\nDisks:\n");
    if (!ndisk)
        printf("  (none in /sys/block — the kernel has not listed any disks yet)\n");
    for (i = 0; i < ndisk; i++) {
        char sz[32];
        disks_format_bytes(disks[i].bytes, sz, sizeof sz);
        printf("  %d) %s  %s  %s%s\n", i + 1, disks[i].name, sz, disks[i].summary,
               disks[i].in_use ? "  [can't choose]" : "");
    }
    if (ndisk) {
        int d = 0;
        while (disks[d].in_use && d + 1 < ndisk)
            d++;
        if (disks[d].in_use) {
            fprintf(stderr, "MyOS is running from every disk listed: there is nowhere to install.\n");
            return 1;
        }
        for (;;) {
            d = pick("Number", ndisk, d);
            if (!disks[d].in_use)
                break;
            printf("MyOS is running from that disk, so it can't be erased. Pick another one.\n");
            for (d = 0; disks[d].in_use; d++)
                ;                       /* the default goes back to one that can be chosen */
        }
        strcpy(p->disk, disks[d].path);
        strcpy(p->disk_name, disks[d].name);
        p->disk_bytes = disks[d].bytes;
        p->free_bytes = disks[d].free_bytes;
        p->keep_esp = disks[d].has_windows;
    } else if (read_line("Device path (for a later run, e.g. /dev/sda): ", buf, sizeof buf, 0) == 0)
        snprintf(p->disk, sizeof p->disk, "%s", buf);

    printf("\nHow to use the disk:\n  1) free space (keep what is there)\n  2) erase the disk\n  3) custom sizes\n");
    i = pick("Number", 3, 0);
    p->mode = i == 1 ? MODE_ERASE : i == 2 ? MODE_CUSTOM : MODE_FREE;
    if (read_line("System size in GB [32]: ", buf, sizeof buf, 0) == 0 && atoi(buf) >= MIN_INSTALL_GB)
        p->root_gb = atoi(buf);
    if (read_line("Separate /home (myos-data)? [Y/n] ", buf, sizeof buf, 0) == 0)
        p->separate_home = !(buf[0] == 'n' || buf[0] == 'N');

    printf("\nYour account (administrator; root will be locked):\n");
    if (read_line("Your name: ", p->fullname, sizeof p->fullname, 0))
        return 1;
    plan_suggest_user(p->fullname, p->user, sizeof p->user);
    printf("User name [%s]: ", p->user);
    fflush(stdout);
    if (fgets(buf, sizeof buf, stdin) && buf[0] && buf[0] != '\n') {
        buf[strcspn(buf, "\n")] = 0;
        snprintf(p->user, sizeof p->user, "%s", buf);
    }
    for (;;) {
        if (read_line("Password: ", p->password, sizeof p->password, 1))
            return 1;
        if (read_line("Again: ", pass2, sizeof pass2, 1))
            return 1;
        if (!p->password[0]) {
            printf("An empty password is refused.\n");
            continue;
        }
        if (strcmp(p->password, pass2)) {
            printf("Those did not match.\n");
            continue;
        }
        break;
    }
    disks_suggest_hostname(p->hostname, sizeof p->hostname, p->user);
    printf("Computer name [%s]: ", p->hostname);
    fflush(stdout);
    if (fgets(buf, sizeof buf, stdin) && buf[0] && buf[0] != '\n') {
        buf[strcspn(buf, "\n")] = 0;
        snprintf(p->hostname, sizeof p->hostname, "%s", buf);
    }
    if (read_line("Ask for the password at login? [Y/n] ", buf, sizeof buf, 0) == 0)
        p->ask_password = !(buf[0] == 'n' || buf[0] == 'N');
    if (read_line("Also make a guest account? [y/N] ", buf, sizeof buf, 0) == 0)
        p->guest = buf[0] == 'y' || buf[0] == 'Y';

    if (disks_wifi_ssid(p->wifi_ssid, sizeof p->wifi_ssid) == 0) {
        printf("Copy the Wi-Fi network \"%s\"? [Y/n] ", p->wifi_ssid);
        fflush(stdout);
        if (fgets(buf, sizeof buf, stdin) && (buf[0] == 'n' || buf[0] == 'N'))
            p->copy_wifi = 0;
        else
            p->copy_wifi = 1;
    }

    pick_compositor(p);
    p->firmware = disks_firmware_uefi() ? FW_UEFI : FW_BIOS;

    if (plan_check(p, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    plan_describe(p, buf, sizeof buf);
    printf("\n--- summary (nothing written yet) ---\n%s\n", buf);
    printf("The plan will be %s\n", plan_path);
    if (read_line("Install now? This writes the disk. [y/N] ", buf, sizeof buf, 0))
        return 1;
    apply = buf[0] == 'y' || buf[0] == 'Y';
    if (plan_write(plan_path, p)) {
        fprintf(stderr, "Can't write %s\n", plan_path);
        return 1;
    }
    printf("Wrote %s\n", plan_path);
    if (!apply) {
        printf("Stopped before writing. Run: %s --apply %s\n", helper_path(), plan_path);
        return 0;
    }
    {
        pid_t pid = fork();
        int st;
        if (pid == 0) {
            execl(helper_path(), "myos-install-helper", "--apply", plan_path, (char *)NULL);
            perror("myos-install-helper");
            _exit(127);
        }
        waitpid(pid, &st, 0);
        return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
    }
}

/* --- GTK ------------------------------------------------------------------ */

static int chosen, auto_pct, screen_w, screen_h;
static double k = 1.0;
#define S(x) ((int)((x) * k + 0.5))

static struct {
    GtkWidget *window, *stack, *back, *next, *extra;
    GtkWidget *step_lab[9];
    GtkWidget *lang_list, *cjk, *kb_list, *try_kb, *detected;
    GtkWidget *map, *tz_search, *region, *city, *now, *ntp, *winclk;
    GtkWidget *disk_list, *disk_bar, *mode_free, *mode_erase, *mode_custom, *free_note;
    GtkWidget *before_bar, *after_bar, *part_list, *root_spin, *home, *encrypt;
    GtkWidget *full, *user, *pass, *pass2, *host, *strength, *ask_pw, *guest;
    GtkWidget *summary, *wifi, *warn;
    GtkWidget *work_lab[6], *work_mark[6], *prog, *file_lab;
    GtkWidget *done_body, *save_log;
    struct install_plan plan;
    struct disk_info disks[DISKS_MAX];
    int ndisk, disk_sel, lang_sel, kb_sel, tz_sel, page;
    int user_set_user, user_set_host;
    char plan_path[256];
    GPid helper_pid;
    guint helper_watch;
} ui;

static GtkWidget *label_new(const char *text, const char *cls, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    if (cls)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void apply_css(void)
{
    static GtkCssProvider *css;
    char *s = g_strdup_printf(
        "* { font-family: 'DejaVu Sans'; color: #1d2430; }\n"
        "window.install { background-color: #fbfbf9; }\n"
        ".sidebar { background-image: linear-gradient(#f9e6c1, #efd3a0); }\n"
        ".sidebar .step { padding: %dpx %dpx; color: #5b6573; }\n"
        ".sidebar .step.now { color: #1d2430; font-weight: bold; }\n"
        ".sidebar .step.done { color: #1f6d72; }\n"
        ".heading { font-weight: bold; font-size: 16pt; }\n"
        ".blurb { color: #5b6573; }\n"
        ".teal { color: #1f6d72; font-weight: bold; }\n"
        ".warn { background-color: #fde3d4; color: #a24f00; padding: %dpx; }\n"
        ".note { background-color: #fff4dc; padding: %dpx; }\n"
        ".foot { border-top: 1px solid #d8c9a8; padding: %dpx; }\n"
        "progress, trough { min-height: %dpx; }\n",
        S(4), S(10), S(8), S(8), S(10), S(18));
    if (!css)
        css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, s, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(s);
}

static long root_cardinal(Display *d, Atom a, long dflt)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long v = dflt;
    if (XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &data) == Success && data && n == 1 && fmt == 32)
        v = *(long *)data;
    if (data)
        XFree(data);
    return v;
}

static void read_scale(void)
{
    Display *d = gdk_x11_get_default_xdisplay();
    Atom a_scale = XInternAtom(d, "_MYOS_SCALE", False);
    Atom a_auto = XInternAtom(d, "_MYOS_SCALE_AUTO", False);
    screen_w = DisplayWidth(d, DefaultScreen(d));
    screen_h = DisplayHeight(d, DefaultScreen(d));
    auto_pct = root_cardinal(d, a_auto, auto_scale(screen_w));
    long v = root_cardinal(d, a_scale, -1);
    chosen = v < 0 ? scale_read_conf() : v ? clamp_scale((int)v) : 0;
    k = (chosen ? chosen : auto_pct) >= 200
            ? (chosen ? chosen : auto_pct) / 200.0
            : (chosen ? chosen : auto_pct) / 100.0;
}

static GtkListBoxRow *add_row(GtkWidget *box, const char *left, const char *right, int idx)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(12));
    GtkWidget *a = label_new(left, NULL, 0);
    gtk_box_pack_start(GTK_BOX(row), a, TRUE, TRUE, 0);
    if (right && right[0]) {
        GtkWidget *b = label_new(right, "blurb", 1);
        gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    GtkWidget *wrap = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(wrap), row);
    gtk_container_add(GTK_CONTAINER(box), wrap);
    g_object_set_data(G_OBJECT(wrap), "idx", GINT_TO_POINTER(idx));
    return GTK_LIST_BOX_ROW(wrap);
}

static void select_idx(GtkWidget *box, int idx)
{
    gtk_list_box_select_row(GTK_LIST_BOX(box), gtk_list_box_get_row_at_index(GTK_LIST_BOX(box), idx));
}

/* Disk bars: a cairo strip of partitions. parts[] is fraction, r,g,b, label. */
struct bar_part { double frac; double r, g, b; const char *label; };

static gboolean draw_bar(GtkWidget *w, cairo_t *cr, gpointer data)
{
    struct bar_part *parts = data;
    GtkAllocation a;
    double x;

    gtk_widget_get_allocation(w, &a);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, 0, 0, a.width, a.height);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.85, 0.80, 0.70);
    cairo_rectangle(cr, 0.5, 0.5, a.width - 1, a.height - 1);
    cairo_stroke(cr);
    x = 1;
    for (int i = 0; parts && parts[i].frac > 0; i++) {
        double pw = (a.width - 2) * parts[i].frac;
        cairo_set_source_rgb(cr, parts[i].r, parts[i].g, parts[i].b);
        cairo_rectangle(cr, x, 1, pw, a.height - 2);
        cairo_fill(cr);
        if (pw > 50 && parts[i].label) {
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, x + 6, a.height / 2 + 4);
            cairo_show_text(cr, parts[i].label);
        }
        x += pw;
    }
    return FALSE;
}

static struct bar_part before_parts[8], after_parts[8];

static void fill_disk_parts(const struct disk_info *d, int after)
{
    struct bar_part *dst = after ? after_parts : before_parts;
    memset(dst, 0, sizeof before_parts);
    if (!d || !d->bytes)
        return;
    int n = 0;
    for (int i = 0; i < d->npart && n < 6; i++) {
        dst[n].frac = (double)d->parts[i].bytes / (double)d->bytes;
        if (d->parts[i].is_esp) {
            dst[n].r = 0.49; dst[n].g = 0.61; dst[n].b = 0.72;
        } else {
            dst[n].r = 0.12; dst[n].g = 0.44; dst[n].b = 0.82;
        }
        dst[n].label = d->parts[i].label[0] ? d->parts[i].label : d->parts[i].name;
        n++;
    }
    if (d->free_bytes) {
        dst[n].frac = (double)d->free_bytes / (double)d->bytes;
        if (after) {
            double root = (ui.plan.root_gb * 1e9) / (double)d->bytes;
            if (root > dst[n].frac)
                root = dst[n].frac;
            dst[n].r = 0.12; dst[n].g = 0.50; dst[n].b = 0.53;
            dst[n].label = "root";
            dst[n].frac = root;
            n++;
            if (ui.plan.separate_home && d->free_bytes > (unsigned long long)ui.plan.root_gb * 1000000000ULL) {
                dst[n].frac = (double)d->free_bytes / (double)d->bytes - root;
                dst[n].r = 0.17; dst[n].g = 0.58; dst[n].b = 0.19;
                dst[n].label = "myos-data";
            }
        } else {
            dst[n].r = 0.79; dst[n].g = 0.76; dst[n].b = 0.70;
            dst[n].label = "Free";
        }
    }
}

static const struct disk_info *cur_disk(void)
{
    if (ui.disk_sel < 0 || ui.disk_sel >= ui.ndisk)
        return NULL;
    return &ui.disks[ui.disk_sel];
}

static void refresh_side(void)
{
    for (int i = 0; i < 9; i++) {
        char t[64];
        GtkStyleContext *c = gtk_widget_get_style_context(ui.step_lab[i]);
        gtk_style_context_remove_class(c, "now");
        gtk_style_context_remove_class(c, "done");
        if (i < ui.page) {
            snprintf(t, sizeof t, "✓  %s", STEPS[i]);
            gtk_style_context_add_class(c, "done");
        } else if (i == ui.page) {
            snprintf(t, sizeof t, "%d  %s", i + 1, STEPS[i]);
            gtk_style_context_add_class(c, "now");
        } else {
            snprintf(t, sizeof t, "%d  %s", i + 1, STEPS[i]);
        }
        gtk_label_set_text(GTK_LABEL(ui.step_lab[i]), t);
    }
}

static void gather(void)
{
    struct install_plan *p = &ui.plan;
    const struct disk_info *d = cur_disk();
    const struct lang_opt *L = &LANGS[ui.lang_sel];
    const struct kb_opt *K = &KBS[ui.kb_sel];
    const char *full, *user, *host;

    strcpy(p->lang, L->id);
    strcpy(p->lang_name, L->native);
    p->cjk = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.cjk));
    strcpy(p->keyboard, K->id);
    strcpy(p->keyboard_name, K->name);
    snprintf(p->timezone, sizeof p->timezone, "%s", tzs[ui.tz_sel].id);
    p->ntp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.ntp));
    p->hwclock_local = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.winclk));
    if (d) {
        strcpy(p->disk, d->path);
        strcpy(p->disk_name, d->name);
        p->disk_bytes = d->bytes;
        p->free_bytes = d->free_bytes;
        p->keep_esp = d->has_windows || d->npart > 0;
    }
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.mode_erase)))
        p->mode = MODE_ERASE;
    else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.mode_custom)))
        p->mode = MODE_CUSTOM;
    else
        p->mode = MODE_FREE;
    p->root_gb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.root_spin));
    p->separate_home = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.home));
    p->encrypt = 0;
    full = gtk_entry_get_text(GTK_ENTRY(ui.full));
    user = gtk_entry_get_text(GTK_ENTRY(ui.user));
    host = gtk_entry_get_text(GTK_ENTRY(ui.host));
    snprintf(p->fullname, sizeof p->fullname, "%s", full);
    snprintf(p->user, sizeof p->user, "%s", user);
    snprintf(p->hostname, sizeof p->hostname, "%s", host);
    snprintf(p->password, sizeof p->password, "%s", gtk_entry_get_text(GTK_ENTRY(ui.pass)));
    p->ask_password = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.ask_pw));
    p->guest = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.guest));
    p->copy_wifi = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.wifi));
    p->firmware = disks_firmware_uefi() ? FW_UEFI : FW_BIOS;
}

static void refresh_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    char buf[96], oldtz[128];
    const char *prev = getenv("TZ");

    snprintf(oldtz, sizeof oldtz, "%s", prev ? prev : "");
    setenv("TZ", tzs[ui.tz_sel].id, 1);
    tzset();
    localtime_r(&now, &tm);
    strftime(buf, sizeof buf, "Now: %I:%M %p, %A %e %B %Y", &tm);
    gtk_label_set_text(GTK_LABEL(ui.now), buf);
    if (oldtz[0])
        setenv("TZ", oldtz, 1);
    else
        unsetenv("TZ");
    tzset();
}

static void refresh_summary(void)
{
    char desc[1024];
    gather();
    plan_describe(&ui.plan, desc, sizeof desc);
    gtk_label_set_text(GTK_LABEL(ui.summary), desc);
    if (ui.plan.disk_name[0]) {
        char w[256];
        snprintf(w, sizeof w, "Clicking Install writes to %s. You can't undo it.\nBack up anything you care about first.",
                 ui.plan.disk_name);
        gtk_label_set_text(GTK_LABEL(ui.warn), w);
    }
    if (ui.plan.wifi_ssid[0]) {
        char t[160];
        snprintf(t, sizeof t, "Copy the Wi-Fi network I'm using now (%s)", ui.plan.wifi_ssid);
        gtk_button_set_label(GTK_BUTTON(ui.wifi), t);
        gtk_widget_set_sensitive(ui.wifi, TRUE);
    } else {
        gtk_button_set_label(GTK_BUTTON(ui.wifi), "Copy the Wi-Fi network I'm using now (none found)");
        gtk_widget_set_sensitive(ui.wifi, FALSE);
    }
}

static void refresh_parts(void)
{
    const struct disk_info *d = cur_disk();
    GList *children, *c;

    fill_disk_parts(d, 0);
    fill_disk_parts(d, 1);
    gtk_widget_queue_draw(ui.before_bar);
    gtk_widget_queue_draw(ui.after_bar);
    gtk_widget_queue_draw(ui.disk_bar);

    children = gtk_container_get_children(GTK_CONTAINER(ui.part_list));
    for (c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);
    if (!d)
        add_row(ui.part_list, "No disk selected", "", 0);
    else {
        if (ui.plan.firmware == FW_UEFI)
            add_row(ui.part_list, "EFI system partition",
                    ui.plan.keep_esp ? "kept; MyOS adds EFI/MYOS" : "new, 260 MB", 0);
        add_row(ui.part_list, "myos-root  (ext2, /)", "the system", 1);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.home)))
            add_row(ui.part_list, "myos-data  (ext2, /home)", "your files", 2);
    }
}

static void set_page(int page);
static gboolean page_ok(int page, char *err, size_t n);
static void show_tz(int i);

static void lang_picked(GtkListBox *box, GtkListBoxRow *row, gpointer unused)
{
    (void)box, (void)unused;
    if (!row)
        return;
    ui.lang_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.cjk), LANGS[ui.lang_sel].cjk);
    for (int i = 0; i < (int)(sizeof KBS / sizeof *KBS); i++)
        if (!strcmp(KBS[i].id, LANGS[ui.lang_sel].keyboard)) {
            ui.kb_sel = i;
            select_idx(ui.kb_list, i);
            break;
        }
    show_tz(tz_find(LANGS[ui.lang_sel].tz));
}

static void kb_picked(GtkListBox *box, GtkListBoxRow *row, gpointer unused)
{
    (void)box, (void)unused;
    if (!row)
        return;
    ui.kb_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx"));
    apply_layout(KBS[ui.kb_sel].id);
}

static void disk_picked(GtkListBox *box, GtkListBoxRow *row, gpointer unused)
{
    (void)box, (void)unused;
    if (!row)
        return;
    ui.disk_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx"));
    gather();
    refresh_parts();
}

static void full_changed(GtkEditable *e, gpointer unused)
{
    char sug[PLAN_USER], host[PLAN_NAME];
    (void)e, (void)unused;
    if (ui.user_set_user)
        return;
    plan_suggest_user(gtk_entry_get_text(GTK_ENTRY(ui.full)), sug, sizeof sug);
    gtk_entry_set_text(GTK_ENTRY(ui.user), sug);
    ui.user_set_user = 0;
    if (!ui.user_set_host) {
        disks_suggest_hostname(host, sizeof host, sug);
        gtk_entry_set_text(GTK_ENTRY(ui.host), host);
        ui.user_set_host = 0;
    }
}

static void user_changed(GtkEditable *e, gpointer unused)
{
    (void)e, (void)unused;
    if (gtk_widget_has_focus(ui.user))
        ui.user_set_user = 1;
    if (!ui.user_set_host) {
        char host[PLAN_NAME];
        disks_suggest_hostname(host, sizeof host, gtk_entry_get_text(GTK_ENTRY(ui.user)));
        gtk_entry_set_text(GTK_ENTRY(ui.host), host);
        ui.user_set_host = 0;
    }
}

static void host_changed(GtkEditable *e, gpointer unused)
{
    (void)e, (void)unused;
    if (gtk_widget_has_focus(ui.host))
        ui.user_set_host = 1;
}

static const char *score_word(int s)
{
    return s <= 0 ? "Empty: a password is required" :
           s == 1 ? "Too short" :
           s == 2 ? "Fair: add a word or two" :
           s == 3 ? "Good" : "Strong";
}

static void pass_changed(GtkEditable *e, gpointer unused)
{
    const char *a = gtk_entry_get_text(GTK_ENTRY(ui.pass));
    const char *b = gtk_entry_get_text(GTK_ENTRY(ui.pass2));
    char t[96];
    (void)e, (void)unused;
    snprintf(t, sizeof t, "%s%s", score_word(plan_password_score(a)),
             b[0] && strcmp(a, b) ? "  —  the two passwords don't match" : "");
    gtk_label_set_text(GTK_LABEL(ui.strength), t);
}

/* The time zone map is equirectangular: longitude goes straight to x and
   latitude to y, so a city's dot is one multiply away. It shows the whole
   way round the Earth, WORLD_LAT_TOP to WORLD_LAT_BOTTOM, as large as fits
   in the drawing area without stretching, centred; the sea fills the rest. */
struct map_fit { double x0, y0, ppd; };        /* ppd: pixels per degree */

static struct map_fit map_fit(GtkWidget *w)
{
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    double span = WORLD_LAT_TOP - WORLD_LAT_BOTTOM;
    double ppd = fmin(a.width / 360.0, a.height / span);
    return (struct map_fit){ (a.width - 360 * ppd) / 2, (a.height - span * ppd) / 2, ppd };
}

static double map_x(const struct map_fit *m, double lon) { return m->x0 + (lon + 180) * m->ppd; }
static double map_y(const struct map_fit *m, double lat) { return m->y0 + (WORLD_LAT_TOP - lat) * m->ppd; }

static gboolean map_draw(GtkWidget *w, cairo_t *cr, gpointer unused)
{
    struct map_fit m = map_fit(w);
    int width = gtk_widget_get_allocated_width(w);
    (void)unused;
    cairo_set_source_rgb(cr, 0.81, 0.91, 0.94);         /* the sea */
    cairo_paint(cr);

    /* The land (worldmap.h): every coastline in one path, filled even-odd
       so a lake inside a continent stays water. */
    const short (*p)[2] = WORLD_PTS;
    for (size_t r = 0; r < sizeof WORLD_RING / sizeof *WORLD_RING; r++) {
        cairo_move_to(cr, map_x(&m, p[0][0] / 10.0), map_y(&m, p[0][1] / 10.0));
        for (int i = 1; i < WORLD_RING[r]; i++)
            cairo_line_to(cr, map_x(&m, p[i][0] / 10.0), map_y(&m, p[i][1] / 10.0));
        cairo_close_path(cr);
        p += WORLD_RING[r];
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_source_rgb(cr, 0.61, 0.78, 0.63);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.42, 0.62, 0.46);         /* a coastline, so small islands show */
    cairo_set_line_width(cr, 0.7 * k);
    cairo_stroke(cr);

    /* Every zone's city: a small dot. All in one path and one fill (some
       400 of them; one fill each would be slow on the old PCs), winding so
       dots that overlap stay solid. */
    for (int i = 0; i < ntz; i++)
        if (i != ui.tz_sel && tzs[i].lat != NOWHERE) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, map_x(&m, tzs[i].lon), map_y(&m, tzs[i].lat), 1.8 * k, 0, 2 * M_PI);
        }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_set_source_rgba(cr, 0.18, 0.22, 0.28, 0.8);
    cairo_fill(cr);

    /* The chosen one on top: bigger, ringed, and named — right of the dot,
       or left of it near the edge. */
    const struct tz_opt *t = &tzs[ui.tz_sel];
    if (t->lat != NOWHERE) {
        double x = map_x(&m, t->lon), y = map_y(&m, t->lat), tx;
        cairo_text_extents_t te;
        cairo_arc(cr, x, y, 5.5 * k, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 0.94, 0.54, 0.07);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1.5 * k);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12 * k);
        cairo_text_extents(cr, t->city, &te);
        tx = x + 9 * k;
        if (tx + te.x_advance > width - 2)
            tx = x - 9 * k - te.x_advance;
        cairo_set_source_rgb(cr, 0.11, 0.14, 0.19);
        cairo_move_to(cr, tx, y - te.y_bearing / 2);
        cairo_show_text(cr, t->city);
    }
    return FALSE;
}

/* The lists carry ids: a region's is its tzdata name ("Indian"), a city's
   is its index in tzs[] — so neither depends on the words shown. */
static void city_changed(GtkComboBox *c, gpointer unused)
{
    const char *id = gtk_combo_box_get_active_id(c);
    (void)unused;
    if (!id)
        return;                         /* the list is being refilled */
    ui.tz_sel = atoi(id);
    gtk_widget_queue_draw(ui.map);
    refresh_clock();
}

static void fill_regions(void)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.region));
    for (int i = 0; i < ntz; i++)
        if (!i || strcmp(tzs[i].region, tzs[i - 1].region))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.region), tzs[i].region,
                                      region_label(tzs[i].region));
}

/* Region chosen: City lists its zones. The chosen zone stays if it is in
   the region; otherwise the region's first city is chosen. */
static void region_changed(GtkComboBox *c, gpointer unused)
{
    const char *reg = gtk_combo_box_get_active_id(c);
    char id[16], label[128];
    (void)unused;
    if (!reg)
        return;
    if (strcmp(tzs[ui.tz_sel].region, reg))
        for (int i = 0; i < ntz; i++)
            if (!strcmp(tzs[i].region, reg)) {
                ui.tz_sel = i;
                break;
            }
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui.city));
    for (int i = 0; i < ntz; i++)
        if (!strcmp(tzs[i].region, reg)) {
            snprintf(id, sizeof id, "%d", i);
            tz_label(&tzs[i], label, sizeof label);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.city), id, label);
        }
    snprintf(id, sizeof id, "%d", ui.tz_sel);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.city), id);
    gtk_widget_queue_draw(ui.map);
    refresh_clock();
}

/* Choose time zone i everywhere at once: Region, City, the map and the
   clock line. (Picking a language calls this before Region is filled;
   then only the choice is kept, and start-up fills Region later.) */
static void show_tz(int i)
{
    ui.tz_sel = i;
    if (!ui.region ||
        !gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(ui.region)), NULL))
        return;
    const char *cur = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.region));
    if (!cur || strcmp(cur, tzs[i].region))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.region), tzs[i].region);  /* refills City */
    else
        region_changed(GTK_COMBO_BOX(ui.region), NULL);         /* same region: City only */
}

/* --- time zone search -------------------------------------------------------- */

/* The search box: part of a city, a country or a region — "taipei",
   "taiwan", "new york", "america chicago" — in any order, any case. As you
   type, the best match is chosen at once (the map, Region and City follow),
   so it works even where a pop-up list does not; the matches are also
   offered under the box, and Enter takes the best one. tzdata names one city
   per zone, so a smaller town is found by its country or a city nearby. */
enum { TZS_LABEL, TZS_INDEX, TZS_KEY, TZS_N };

/* Every word of `typed` is in `key` (both lower case). */
static int tz_words_in(const char *key, const char *typed)
{
    char word[48];
    const char *s = typed;
    for (;;) {
        while (*s == ' ' || *s == ',')
            s++;
        if (!*s)
            return 1;
        size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != ',') {
            if (n < sizeof word - 1)
                word[n] = (char)tolower((unsigned char)s[n]);
            n++;
        }
        word[n < sizeof word ? n : sizeof word - 1] = 0;
        s += n;
        if (!strstr(key, word))
            return 0;
    }
}

/* What the zone is searched by: city, country, region, tzdata's name. */
static void tz_key(const struct tz_opt *t, char *out, size_t n)
{
    snprintf(out, n, "%s %s %s %s", t->city, t->country ? t->country : "",
             region_label(t->region), t->id);
    for (char *c = out; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

/* How well zone t fits what was typed: -1 not at all; more is better (the
   city itself, then a city starting so, then a country starting so). */
static int tz_score(const struct tz_opt *t, const char *typed)
{
    char key[160], city[48], country[64], first[48];
    tz_key(t, key, sizeof key);
    if (!tz_words_in(key, typed))
        return -1;
    snprintf(city, sizeof city, "%s", t->city);
    snprintf(country, sizeof country, "%s", t->country ? t->country : "");
    for (char *c = city; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    for (char *c = country; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    while (*typed == ' ')
        typed++;
    size_t n = strcspn(typed, " ,");
    snprintf(first, sizeof first, "%.*s", (int)(n < sizeof first ? n : sizeof first - 1), typed);
    for (char *c = first; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    int score = 0;
    if (!g_ascii_strcasecmp(city, typed))
        score += 8;
    if (!strncmp(city, first, strlen(first)))
        score += 4;
    if (!strncmp(country, first, strlen(first)))
        score += 2;
    return score;
}

/* The best zone for the text, or -1. Ties go to the first in the list. */
static int tz_best(const char *typed)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < ntz; i++) {
        int sc = tz_score(&tzs[i], typed);
        if (sc > best_score)
            best = i, best_score = sc;
    }
    return best;
}

static gboolean tz_match(GtkEntryCompletion *c, const gchar *typed, GtkTreeIter *it,
                         gpointer unused)
{
    gchar *key = NULL;
    (void)unused;
    gtk_tree_model_get(gtk_entry_completion_get_model(c), it, TZS_KEY, &key, -1);
    gboolean ok = key && tz_words_in(key, typed);
    g_free(key);
    return ok;
}

static gboolean tz_picked(GtkEntryCompletion *c, GtkTreeModel *m, GtkTreeIter *it,
                          gpointer unused)
{
    gint idx = -1;
    (void)c, (void)unused;
    gtk_tree_model_get(m, it, TZS_INDEX, &idx, -1);
    if (idx >= 0 && idx < ntz)
        show_tz(idx);
    return FALSE;                       /* the box shows the label it picked */
}

/* Typing: choose the best match now; Enter does the same. */
static void tz_typed(GtkWidget *e, gpointer unused)
{
    const char *typed = gtk_entry_get_text(GTK_ENTRY(e));
    (void)unused;
    while (*typed == ' ')
        typed++;
    if (!*typed)
        return;
    int i = tz_best(typed);
    if (i >= 0)
        show_tz(i);
}

static void tz_search_fill(void)
{
    GtkListStore *store = gtk_list_store_new(TZS_N, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    char label[160], key[160];
    for (int i = 0; i < ntz; i++) {
        GtkTreeIter it;
        tz_label(&tzs[i], label, sizeof label);
        tz_key(&tzs[i], key, sizeof key);
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, TZS_LABEL, label, TZS_INDEX, i, TZS_KEY, key, -1);
    }
    GtkEntryCompletion *c = gtk_entry_completion_new();
    gtk_entry_completion_set_model(c, GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_entry_completion_set_text_column(c, TZS_LABEL);
    gtk_entry_completion_set_match_func(c, tz_match, NULL, NULL);
    gtk_entry_completion_set_minimum_key_length(c, 1);
    gtk_entry_completion_set_popup_set_width(c, TRUE);
    g_signal_connect(c, "match-selected", G_CALLBACK(tz_picked), NULL);
    gtk_entry_set_completion(GTK_ENTRY(ui.tz_search), c);
    g_object_unref(c);
}

/* A click on the map picks the nearest city's dot. */
static gboolean map_click(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
    struct map_fit m = map_fit(w);
    int best = -1;
    double best_d = 0;
    (void)unused;
    for (int i = 0; i < ntz; i++) {
        if (tzs[i].lat == NOWHERE)
            continue;
        double dx = ev->x - map_x(&m, tzs[i].lon), dy = ev->y - map_y(&m, tzs[i].lat);
        if (best < 0 || dx * dx + dy * dy < best_d)
            best = i, best_d = dx * dx + dy * dy;
    }
    if (best >= 0)
        show_tz(best);
    return TRUE;
}

static GtkWidget *page_box(const char *title, const char *sub, GtkWidget **body)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
    gtk_widget_set_margin_start(v, S(20));
    gtk_widget_set_margin_end(v, S(20));
    gtk_widget_set_margin_top(v, S(16));
    gtk_box_pack_start(GTK_BOX(v), label_new(title, "heading", 0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), label_new(sub, "blurb", 0), FALSE, FALSE, 0);
    *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
    gtk_box_pack_start(GTK_BOX(v), *body, TRUE, TRUE, 0);
    return v;
}

static GtkWidget *build_pages(void)
{
    GtkWidget *body, *page, *scroll;

    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_NONE);

    /* 0 language */
    page = page_box("Welcome", "Choose the language for the installer and for MyOS.", &body);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ui.lang_list = gtk_list_box_new();
    for (int i = 0; i < (int)(sizeof LANGS / sizeof *LANGS); i++)
        add_row(ui.lang_list, LANGS[i].native, LANGS[i].english, i);
    g_signal_connect(ui.lang_list, "row-selected", G_CALLBACK(lang_picked), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), ui.lang_list);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    ui.cjk = gtk_check_button_new_with_label("Also install fonts and input for Chinese (pinyin)");
    gtk_box_pack_start(GTK_BOX(body), ui.cjk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body),
                       label_new("Not ready to install? Close this window to keep trying MyOS from the stick.",
                                 "blurb", 0),
                       FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "0");

    /* 1 keyboard */
    page = page_box("Keyboard", "Pick the layout printed on your keys.", &body);
    ui.detected = label_new("Detected: the layout the live session is using.", "blurb", 0);
    gtk_box_pack_start(GTK_BOX(body), ui.detected, FALSE, FALSE, 0);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    ui.kb_list = gtk_list_box_new();
    for (int i = 0; i < (int)(sizeof KBS / sizeof *KBS); i++)
        add_row(ui.kb_list, KBS[i].name, "", i);
    g_signal_connect(ui.kb_list, "row-selected", G_CALLBACK(kb_picked), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), ui.kb_list);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_box_pack_start(GTK_BOX(row), label_new("Try it here:", NULL, 0), FALSE, FALSE, 0);
        ui.try_kb = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ui.try_kb), "Type a few keys, with Shift and AltGr.");
        gtk_box_pack_start(GTK_BOX(row), ui.try_kb, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    }
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "1");

    /* 2 time zone */
    page = page_box("Time zone", "Where you are, so the clock is right.", &body);
    ui.map = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui.map, -1, S(180));
    gtk_widget_add_events(ui.map, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(ui.map, "draw", G_CALLBACK(map_draw), NULL);
    g_signal_connect(ui.map, "button-press-event", G_CALLBACK(map_click), NULL);
    gtk_box_pack_start(GTK_BOX(body), ui.map, TRUE, TRUE, 0);
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), S(8));
        gtk_grid_set_column_spacing(GTK_GRID(grid), S(12));
        ui.tz_search = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ui.tz_search),
                                       "Type your city or country: Taipei, Taiwan, Chicago...");
        g_signal_connect(ui.tz_search, "search-changed", G_CALLBACK(tz_typed), NULL);
        g_signal_connect(ui.tz_search, "activate", G_CALLBACK(tz_typed), NULL);
        ui.region = gtk_combo_box_text_new();
        ui.city = gtk_combo_box_text_new();
        gtk_grid_attach(GTK_GRID(grid), label_new("Search:", NULL, 1), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.tz_search, 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("Region:", NULL, 1), 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.region, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("City:", NULL, 1), 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.city, 1, 2, 1, 1);
        gtk_widget_set_hexpand(ui.tz_search, TRUE);
        gtk_widget_set_hexpand(ui.region, TRUE);
        gtk_widget_set_hexpand(ui.city, TRUE);
        gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
        g_signal_connect(ui.region, "changed", G_CALLBACK(region_changed), NULL);
        g_signal_connect(ui.city, "changed", G_CALLBACK(city_changed), NULL);
    }
    ui.now = label_new("", NULL, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.now, FALSE, FALSE, 0);
    ui.ntp = gtk_check_button_new_with_label("Set the time from the network");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.ntp), TRUE);
    ui.winclk = gtk_check_button_new_with_label("This PC also runs Windows (keep the hardware clock in local time)");
    gtk_box_pack_start(GTK_BOX(body), ui.ntp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.winclk, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "2");

    /* 3 disk */
    page = page_box("Where to install", "MyOS needs at least 8 GB. Pick a disk, then how to use it.", &body);
    ui.disk_list = gtk_list_box_new();
    g_signal_connect(ui.disk_list, "row-selected", G_CALLBACK(disk_picked), NULL);
    gtk_box_pack_start(GTK_BOX(body), ui.disk_list, FALSE, FALSE, 0);
    ui.disk_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui.disk_bar, -1, S(28));
    g_signal_connect(ui.disk_bar, "draw", G_CALLBACK(draw_bar), before_parts);
    gtk_box_pack_start(GTK_BOX(body), ui.disk_bar, FALSE, FALSE, 0);
    ui.mode_free = gtk_radio_button_new_with_label(NULL, "Install MyOS in the free space");
    ui.mode_erase = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(ui.mode_free),
                                                               "Erase this disk and install MyOS");
    ui.mode_custom = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(ui.mode_free),
                                                                "Something else");
    gtk_box_pack_start(GTK_BOX(body), ui.mode_free, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.mode_erase, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.mode_custom, FALSE, FALSE, 0);
    ui.free_note = label_new("No free space? Shrink the Windows partition first, in Windows' "
                             "Disk Management. MyOS can't resize NTFS.",
                             "note", 0);
    gtk_box_pack_start(GTK_BOX(body), ui.free_note, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "3");

    /* 4 partitions */
    page = page_box("Partitions", "What MyOS will make. Nothing is written yet.", &body);
    gtk_box_pack_start(GTK_BOX(body), label_new("Before", "blurb", 0), FALSE, FALSE, 0);
    ui.before_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui.before_bar, -1, S(28));
    g_signal_connect(ui.before_bar, "draw", G_CALLBACK(draw_bar), before_parts);
    gtk_box_pack_start(GTK_BOX(body), ui.before_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), label_new("After", "blurb", 0), FALSE, FALSE, 0);
    ui.after_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui.after_bar, -1, S(28));
    g_signal_connect(ui.after_bar, "draw", G_CALLBACK(draw_bar), after_parts);
    gtk_box_pack_start(GTK_BOX(body), ui.after_bar, FALSE, FALSE, 0);
    ui.part_list = gtk_list_box_new();
    gtk_box_pack_start(GTK_BOX(body), ui.part_list, TRUE, TRUE, 0);
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        gtk_box_pack_start(GTK_BOX(row), label_new("System size:", NULL, 0), FALSE, FALSE, 0);
        ui.root_spin = gtk_spin_button_new_with_range(MIN_INSTALL_GB, 512, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.root_spin), DEFAULT_ROOT_GB);
        gtk_box_pack_start(GTK_BOX(row), ui.root_spin, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), label_new("GB", "blurb", 0), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    }
    ui.home = gtk_check_button_new_with_label("Keep my files on a separate partition (myos-data)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.home), TRUE);
    ui.encrypt = gtk_check_button_new_with_label("Encrypt the data partition (later: not available yet)");
    gtk_widget_set_sensitive(ui.encrypt, FALSE);
    gtk_box_pack_start(GTK_BOX(body), ui.home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.encrypt, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "4");

    /* 5 account */
    page = page_box("Your account", "You'll use it to log in. It can also change settings.", &body);
    {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), S(8));
        gtk_grid_set_column_spacing(GTK_GRID(grid), S(12));
        ui.full = gtk_entry_new();
        ui.user = gtk_entry_new();
        ui.pass = gtk_entry_new();
        ui.pass2 = gtk_entry_new();
        ui.host = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(ui.pass), FALSE);
        gtk_entry_set_visibility(GTK_ENTRY(ui.pass2), FALSE);
        gtk_grid_attach(GTK_GRID(grid), label_new("Your name:", NULL, 1), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.full, 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("User name:", NULL, 1), 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.user, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("Password:", NULL, 1), 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.pass, 1, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("Again:", NULL, 1), 0, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.pass2, 1, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), label_new("Computer name:", NULL, 1), 0, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ui.host, 1, 4, 1, 1);
        gtk_widget_set_hexpand(ui.full, TRUE);
        gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
        g_signal_connect(ui.full, "changed", G_CALLBACK(full_changed), NULL);
        g_signal_connect(ui.user, "changed", G_CALLBACK(user_changed), NULL);
        g_signal_connect(ui.host, "changed", G_CALLBACK(host_changed), NULL);
        g_signal_connect(ui.pass, "changed", G_CALLBACK(pass_changed), NULL);
        g_signal_connect(ui.pass2, "changed", G_CALLBACK(pass_changed), NULL);
    }
    ui.strength = label_new("Empty: a password is required", "blurb", 0);
    gtk_box_pack_start(GTK_BOX(body), ui.strength, FALSE, FALSE, 0);
    ui.ask_pw = gtk_check_button_new_with_label("Ask for my password when I log in");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.ask_pw), TRUE);
    ui.guest = gtk_check_button_new_with_label("Also make a guest account (no password, nothing kept)");
    gtk_box_pack_start(GTK_BOX(body), ui.ask_pw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.guest, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body),
                       label_new("The root account will be locked. Your account is the administrator (doas).",
                                 "blurb", 0),
                       FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "5");

    /* 6 summary */
    page = page_box("Ready to install", "Check this. Nothing has been written to the disk yet.", &body);
    ui.summary = label_new("", NULL, 0);
    gtk_label_set_selectable(GTK_LABEL(ui.summary), TRUE);
    gtk_box_pack_start(GTK_BOX(body), ui.summary, TRUE, TRUE, 0);
    ui.wifi = gtk_check_button_new_with_label("Copy the Wi-Fi network I'm using now");
    gtk_box_pack_start(GTK_BOX(body), ui.wifi, FALSE, FALSE, 0);
    ui.warn = label_new("Clicking Install writes the chosen disk.", "warn", 0);
    gtk_box_pack_start(GTK_BOX(body), ui.warn, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "6");

    /* 7 progress */
    page = page_box("Installing MyOS", "This takes a few minutes. Keep the stick plugged in. There is no Cancel.",
                    &body);
    for (int i = 0; i < 6; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
        ui.work_mark[i] = label_new("○", "blurb", 0);
        ui.work_lab[i] = label_new(WORK[i], NULL, 0);
        gtk_box_pack_start(GTK_BOX(row), ui.work_mark[i], FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), ui.work_lab[i], TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    }
    ui.prog = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(body), ui.prog, FALSE, FALSE, 0);
    ui.file_lab = label_new("", "blurb", 0);
    gtk_box_pack_start(GTK_BOX(body), ui.file_lab, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "7");

    /* 8 done */
    page = page_box("MyOS is installed", "Remove the USB stick, then restart.", &body);
    ui.done_body = label_new("When the computer starts, choose MyOS. If Windows is still there, "
                             "use the machine's boot menu (F11 on the MSI).",
                             NULL, 0);
    gtk_box_pack_start(GTK_BOX(body), ui.done_body, FALSE, FALSE, 0);
    ui.save_log = gtk_check_button_new_with_label("Save the install log to the stick (/myos-install.log)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.save_log), TRUE);
    gtk_box_pack_start(GTK_BOX(body), ui.save_log, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), page, "8");

    return ui.stack;
}

static gboolean page_ok(int page, char *err, size_t n)
{
    gather();
    if (page == 3) {
        const struct disk_info *d = cur_disk();
        if (d && d->in_use) {
            snprintf(err, n, "MyOS is running from that disk. Pick another one.");
            return FALSE;
        }
        if (!ui.plan.disk[0]) {
            snprintf(err, n, "No disk is listed yet. The kernel has not published any "
                             "(/dev/nvme0n1, /dev/sda). You can still finish the questions "
                             "and save a plan; Install will explain what is missing.");
            /* allow going on so a plan can be written for QEMU later */
        }
        if (ui.plan.mode == MODE_FREE && d && d->free_bytes &&
            d->free_bytes < (unsigned long long)MIN_INSTALL_GB * 1000000000ULL) {
            snprintf(err, n, "There is not enough free space. Shrink Windows first, or erase the disk.");
            return FALSE;
        }
    }
    if (page == 5) {
        if (plan_check(&ui.plan, err, n) &&
            (strstr(err, "name") || strstr(err, "password") || strstr(err, "computer") ||
             strstr(err, "user") || strstr(err, "full")))
            return FALSE;
        if (strcmp(gtk_entry_get_text(GTK_ENTRY(ui.pass)), gtk_entry_get_text(GTK_ENTRY(ui.pass2)))) {
            snprintf(err, n, "The two passwords don't match.");
            return FALSE;
        }
    }
    return TRUE;
}

static void mark_work(const char *key, int running)
{
    for (int i = 0; i < 6; i++) {
        if (!strcmp(WORK_KEY[i], key)) {
            gtk_label_set_text(GTK_LABEL(ui.work_mark[i]), running ? "…" : "✓");
            return;
        }
        gtk_label_set_text(GTK_LABEL(ui.work_mark[i]), "✓");
    }
}

static gboolean helper_out(GIOChannel *ch, GIOCondition cond, gpointer unused)
{
    char *line = NULL;
    gsize n = 0;
    (void)cond, (void)unused;
    while (g_io_channel_read_line(ch, &line, &n, NULL, NULL) == G_IO_STATUS_NORMAL && line) {
        if (n && line[n - 1] == '\n')
            line[n - 1] = 0;
        if (!strncmp(line, "step ", 5))
            mark_work(line + 5, 1);
        else if (!strncmp(line, "progress ", 9))
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.prog), atoi(line + 9) / 100.0);
        else if (!strncmp(line, "file ", 5))
            gtk_label_set_text(GTK_LABEL(ui.file_lab), line + 5);
        else if (!strncmp(line, "error:", 6))
            gtk_label_set_text(GTK_LABEL(ui.file_lab), line);
        else if (!strcmp(line, "ok"))
            gtk_label_set_text(GTK_LABEL(ui.file_lab), "Finished.");
        g_free(line);
        line = NULL;
    }
    return TRUE;
}

static void helper_done(GPid pid, gint status, gpointer unused)
{
    (void)unused;
    g_spawn_close_pid(pid);
    ui.helper_pid = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char note[512];
        snprintf(note, sizeof note,
                 "When the computer starts, choose MyOS.\n"
                 "Your account: %s. Your files go in /home/%s.\n"
                 "If Windows is still there, press F11 while the MSI logo shows "
                 "and pick Windows Boot Manager.",
                 ui.plan.user, ui.plan.user);
        gtk_label_set_text(GTK_LABEL(ui.done_body), note);
        set_page(8);
        return;
    }
    gtk_label_set_text(GTK_LABEL(ui.file_lab),
                       gtk_label_get_text(GTK_LABEL(ui.file_lab))[0]
                           ? gtk_label_get_text(GTK_LABEL(ui.file_lab))
                           : "The helper stopped. The disk was not fully installed; see /tmp/myos-install.log.");
    gtk_widget_set_sensitive(ui.back, TRUE);
    gtk_button_set_label(GTK_BUTTON(ui.next), "Close");
}

static void start_helper(void)
{
    char *argv[] = { (char *)helper_path(), "--apply", ui.plan_path, NULL };
    gint out = -1;
    GError *err = NULL;
    GPid pid;

    gather();
    if (plan_write(ui.plan_path, &ui.plan)) {
        gtk_label_set_text(GTK_LABEL(ui.file_lab), "Can't write the plan file.");
        return;
    }
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                  &pid, NULL, &out, NULL, &err)) {
        gtk_label_set_text(GTK_LABEL(ui.file_lab), err ? err->message : "Can't start the helper.");
        if (err)
            g_error_free(err);
        return;
    }
    ui.helper_pid = pid;
    {
        GIOChannel *ch = g_io_channel_unix_new(out);
        g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
        g_io_add_watch(ch, G_IO_IN | G_IO_HUP, helper_out, NULL);
        g_io_channel_unref(ch);
    }
    g_child_watch_add(pid, helper_done, NULL);
}

static void save_log_if_asked(void)
{
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.save_log)))
        return;
    if (access(PLAN_LOG_DEFAULT, R_OK) == 0) {
        char *a[] = { "cp", PLAN_LOG_DEFAULT, "/myos-install.log", NULL };
        g_spawn_async(NULL, a, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
}

static void set_page(int page)
{
    char name[8];
    ui.page = page;
    snprintf(name, sizeof name, "%d", page);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), name);
    refresh_side();
    gtk_widget_set_sensitive(ui.back, page > 0 && page < 7);
    gtk_widget_set_visible(ui.extra, page == 4 || page == 8);
    if (page == 2)
        gtk_widget_grab_focus(ui.tz_search);    /* type a city straight away */
    if (page == 4)
        gtk_button_set_label(GTK_BUTTON(ui.extra), "Edit partitions...");
    if (page == 8)
        gtk_button_set_label(GTK_BUTTON(ui.extra), "Keep trying MyOS");
    if (page == 6)
        gtk_button_set_label(GTK_BUTTON(ui.next), "Install");
    else if (page == 7)
        gtk_button_set_label(GTK_BUTTON(ui.next), "Please wait");
    else if (page == 8)
        gtk_button_set_label(GTK_BUTTON(ui.next), "Restart now");
    else
        gtk_button_set_label(GTK_BUTTON(ui.next), "Next");
    gtk_widget_set_sensitive(ui.next, page != 7);
    if (page == 2)
        refresh_clock();
    if (page == 4)
        refresh_parts();
    if (page == 6)
        refresh_summary();
}

static void next_clicked(GtkButton *b, gpointer unused)
{
    char err[256];
    (void)b, (void)unused;
    if (ui.page == 8) {
        save_log_if_asked();
        g_spawn_command_line_async("doas /sbin/reboot", NULL);
        return;
    }
    if (ui.page == 7)
        return;
    if (ui.page == 6) {
        char e[256];
        gather();
        if (plan_check(&ui.plan, e, sizeof e) && ui.plan.disk[0] && strncmp(ui.plan.disk, "/dev/", 5) == 0 &&
            !strstr(e, "Pick a disk") && strstr(e, "free space")) {
            GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                  GTK_BUTTONS_OK, "%s", e);
            gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
            return;
        }
        if (plan_write(ui.plan_path, &ui.plan)) {
            GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                  GTK_BUTTONS_OK, "Can't write the plan file.");
            gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
            return;
        }
        set_page(7);
        start_helper();
        return;
    }
    err[0] = 0;
    if (!page_ok(ui.page, err, sizeof err)) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                              GTK_BUTTONS_OK, "%s", err);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    set_page(ui.page + 1);
}

static void back_clicked(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    if (ui.page > 0 && ui.page < 7)
        set_page(ui.page - 1);
}

static void extra_clicked(GtkButton *b, gpointer unused)
{
    (void)b, (void)unused;
    if (ui.page == 8) {
        save_log_if_asked();
        gtk_window_close(GTK_WINDOW(ui.window));
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_custom), TRUE);
    gtk_widget_grab_focus(ui.root_spin);
}

static void fill_disks(void)
{
    ui.ndisk = disks_scan(ui.disks, DISKS_MAX);
    ui.disk_sel = -1;
    if (!ui.ndisk) {
        add_row(ui.disk_list, "No disks yet", "the kernel has not listed any", 0);
        gtk_widget_set_sensitive(ui.disk_list, FALSE);
        return;
    }
    for (int i = 0; i < ui.ndisk; i++) {
        char sz[32], right[96];
        disks_format_bytes(ui.disks[i].bytes, sz, sizeof sz);
        snprintf(right, sizeof right, "%s   %s", sz, ui.disks[i].summary);
        GtkListBoxRow *row = add_row(ui.disk_list, ui.disks[i].name, right, i);
        if (ui.disks[i].in_use) {       /* shown, so it is clear why it isn't offered */
            gtk_list_box_row_set_selectable(row, FALSE);
            gtk_list_box_row_set_activatable(row, FALSE);
            gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
            gtk_widget_set_opacity(GTK_WIDGET(row), 0.45);    /* visibly out, whatever the theme */
            gtk_widget_set_tooltip_text(GTK_WIDGET(row),
                                        "MyOS is running from this disk, so it can't be erased.");
        }
        if (ui.disk_sel < 0 && !ui.disks[i].in_use)
            ui.disk_sel = i;
    }
    if (ui.disk_sel >= 0)
        select_idx(ui.disk_list, ui.disk_sel);
}

static void load_defaults(void)
{
    char host[PLAN_NAME];

    plan_init(&ui.plan);
    pick_compositor(&ui.plan);
    ui.plan.firmware = disks_firmware_uefi() ? FW_UEFI : FW_BIOS;
    ui.lang_sel = 0;
    ui.kb_sel = 0;
    ui.tz_sel = tz_find("Asia/Shanghai");      /* as in the mockup */
    disks_wifi_ssid(ui.plan.wifi_ssid, sizeof ui.plan.wifi_ssid);
    disks_suggest_hostname(host, sizeof host, "myos");
    gtk_entry_set_text(GTK_ENTRY(ui.host), host);
    select_idx(ui.lang_list, 0);
    select_idx(ui.kb_list, 0);
    fill_regions();
    tz_search_fill();
    show_tz(tz_find("Asia/Shanghai"));
    fill_disks();
    refresh_clock();
}

static int gtk_install(int argc, char **argv, const char *plan_path)
{
    GtkWidget *outer, *side, *right, *foot, *pages;

    gtk_init(&argc, &argv);
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        fprintf(stderr, "myos-install needs X. Use --text on a machine without graphics.\n");
        return 1;
    }
    read_scale();
    apply_css();
    snprintf(ui.plan_path, sizeof ui.plan_path, "%s", plan_path);

    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window), "Install MyOS");
    gtk_window_set_wmclass(GTK_WINDOW(ui.window), "myos-install", "Install MyOS");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui.window), "install");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), S(800), S(600));
    g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(ui.window), outer);

    side = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
    gtk_style_context_add_class(gtk_widget_get_style_context(side), "sidebar");
    gtk_widget_set_size_request(side, S(190), -1);
    gtk_widget_set_margin_top(side, S(20));
    gtk_box_pack_start(GTK_BOX(outer), side, FALSE, FALSE, 0);
    for (int i = 0; i < 9; i++) {
        char t[64];
        snprintf(t, sizeof t, "%d  %s", i + 1, STEPS[i]);
        ui.step_lab[i] = label_new(t, "step", 0);
        gtk_box_pack_start(GTK_BOX(side), ui.step_lab[i], FALSE, FALSE, 0);
    }

    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pages = build_pages();
    gtk_box_pack_start(GTK_BOX(right), pages, TRUE, TRUE, 0);

    foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(8));
    gtk_style_context_add_class(gtk_widget_get_style_context(foot), "foot");
    ui.extra = gtk_button_new_with_label("Edit partitions...");
    ui.back = gtk_button_new_with_label("Back");
    ui.next = gtk_button_new_with_label("Next");
    gtk_box_pack_start(GTK_BOX(foot), ui.extra, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), ui.next, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), ui.back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), foot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), right, TRUE, TRUE, 0);

    g_signal_connect(ui.back, "clicked", G_CALLBACK(back_clicked), NULL);
    g_signal_connect(ui.next, "clicked", G_CALLBACK(next_clicked), NULL);
    g_signal_connect(ui.extra, "clicked", G_CALLBACK(extra_clicked), NULL);

    load_defaults();
    set_page(0);
    gtk_window_set_position(GTK_WINDOW(ui.window), GTK_WIN_POS_CENTER);
    gtk_widget_show_all(ui.window);
    gtk_widget_hide(ui.extra);
    /* MYOS_INSTALL_PAGE=N opens on page N (0-6): for checking one page on
       a test machine without clicking through the ones before it. (After
       show_all: a stack ignores a switch to a page not yet shown.) */
    const char *start = getenv("MYOS_INSTALL_PAGE");
    if (start && *start > '0' && *start <= '6' && !start[1])
        set_page(*start - '0');
    gtk_main();
    explicit_bzero(ui.plan.password, sizeof ui.plan.password);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: myos-install [--text] [--plan FILE]\n"
            "       myos-install --apply FILE\n");
}

int main(int argc, char **argv)
{
    int text = 0, apply = 0;
    const char *plan_path = PLAN_PATH_DEFAULT;
    struct install_plan p;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--text"))
            text = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "--plan") && i + 1 < argc)
            plan_path = argv[++i];
        else if (!strcmp(argv[i], "--apply") && i + 1 < argc) {
            apply = 1;
            plan_path = argv[++i];
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        }
    }

    if (apply) {
        execl(helper_path(), "myos-install-helper", "--apply", plan_path, (char *)NULL);
        perror("myos-install-helper");
        return 1;
    }
    plan_init(&p);
    tz_load();
    if (text || !getenv("DISPLAY"))
        return text_install(&p, plan_path);
    return gtk_install(argc, argv, plan_path);
}
