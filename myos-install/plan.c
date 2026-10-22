/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#include "plan.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void plan_init(struct install_plan *p)
{
    memset(p, 0, sizeof *p);
    strcpy(p->lang, "en_US.UTF-8");
    strcpy(p->lang_name, "English (United States)");
    strcpy(p->keyboard, "us");
    strcpy(p->keyboard_name, "English (US)");
    strcpy(p->timezone, "Asia/Shanghai");
    p->ntp = 1;
    p->cjk = 1;                         /* Chinese fonts: always, never asked (Rodger, 2026-10-09) */
    p->root_gb = DEFAULT_ROOT_GB;
    p->separate_home = 1;
    p->ask_password = 1;
    strcpy(p->compositor, "xrender");
    p->firmware = FW_UEFI;
    p->keep_esp = 0;                    /* only free-space installs keep one */
}

const char *plan_mode_name(int mode)
{
    return mode == MODE_ERASE ? "erase" : mode == MODE_CUSTOM ? "custom"
         : mode == MODE_UPGRADE ? "upgrade" : "free";
}

const char *plan_firmware_name(int firmware)
{
    return firmware == FW_BIOS ? "bios" : "uefi";
}

int plan_good_user(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PLAN_USER || !(islower((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (size_t i = 1; i < n; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

int plan_good_fullname(const char *s)
{
    if (!*s || strlen(s) > 64)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p == 127 || *p == ':' || *p == ',')
            return 0;
    return 1;
}

/* A tzdata zone name: America/Argentina/Buenos_Aires, Etc/GMT+5, UTC. The
   helper (root) copies /usr/share/zoneinfo/NAME, so the name must stay in
   that folder: letters, digits, '_', '-', '+' and inner single '/', and no
   '.' at all (no ".."). */
int plan_good_zone(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PLAN_NAME || s[0] == '/' || s[n - 1] == '/' || strstr(s, "//"))
        return 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '+' || *p == '/'))
            return 0;
    return 1;
}

/* A host name (RFC 1123): ASCII letters of either case, digits and -.
   Names are case-insensitive, and Linux keeps them as typed, so "MSI" is
   fine (it was refused once for being capitals, 2026-10-09). */
int plan_good_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isdigit(c) || c == '-'))
            return 0;
    }
    return s[0] != '-' && s[n - 1] != '-';
}

void plan_suggest_user(const char *full, char *out, size_t n)
{
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)full; *p && *p != ' ' && i + 1 < n; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (islower(c) || (i && (isdigit(c) || c == '-' || c == '_')))
            out[i++] = (char)c;
    }
    out[i] = 0;
    if (!plan_good_user(out))
        snprintf(out, n, "user");
}

int plan_password_score(const char *s)
{
    int lower = 0, upper = 0, digit = 0, other = 0;
    size_t n = strlen(s);
    if (!n)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (islower(*p))
            lower = 1;
        else if (isupper(*p))
            upper = 1;
        else if (isdigit(*p))
            digit = 1;
        else
            other = 1;
    }
    int classes = lower + upper + digit + other;
    if (n < 6)
        return 1;
    if (n < 8 || classes < 2)
        return 2;
    if (n < 10 || classes < 3)
        return 3;
    return 4;
}

static int mode_from(const char *s)
{
    if (!strcmp(s, "erase"))
        return MODE_ERASE;
    if (!strcmp(s, "custom"))
        return MODE_CUSTOM;
    if (!strcmp(s, "upgrade"))
        return MODE_UPGRADE;
    return MODE_FREE;
}

static void set_str(char *dst, size_t n, const char *v)
{
    snprintf(dst, n, "%s", v);
}

int plan_write(const char *path, const struct install_plan *p)
{
    char tmp[PLAN_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s+", path);
    /* The plan holds the new password, and /tmp is everyone's: the file is
       private from the moment it exists (not chmod-ed afterwards), and a
       name someone else made first -- perhaps a link to a file of theirs --
       is refused rather than followed or written through. */
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    fprintf(f, "# myos-install plan v%d\n", PLAN_VERSION);
    fprintf(f, "lang=%s\n", p->lang);
    fprintf(f, "lang_name=%s\n", p->lang_name);
    fprintf(f, "cjk=%d\n", p->cjk ? 1 : 0);
    fprintf(f, "keyboard=%s\n", p->keyboard);
    fprintf(f, "keyboard_name=%s\n", p->keyboard_name);
    fprintf(f, "timezone=%s\n", p->timezone);
    fprintf(f, "ntp=%d\n", p->ntp ? 1 : 0);
    fprintf(f, "hwclock=%s\n", p->hwclock_local ? "local" : "utc");
    fprintf(f, "disk=%s\n", p->disk);
    fprintf(f, "disk_name=%s\n", p->disk_name);
    fprintf(f, "disk_bytes=%llu\n", p->disk_bytes);
    fprintf(f, "free_bytes=%llu\n", p->free_bytes);
    fprintf(f, "mode=%s\n", plan_mode_name(p->mode));
    fprintf(f, "keep_esp=%d\n", p->keep_esp ? 1 : 0);
    fprintf(f, "root_gb=%d\n", p->root_gb);
    fprintf(f, "separate_home=%d\n", p->separate_home ? 1 : 0);
    fprintf(f, "encrypt=%d\n", 0);
    fprintf(f, "fullname=%s\n", p->fullname);
    fprintf(f, "user=%s\n", p->user);
    fprintf(f, "password=%s\n", p->password);
    fprintf(f, "hostname=%s\n", p->hostname);
    fprintf(f, "ask_password=%d\n", p->ask_password ? 1 : 0);
    fprintf(f, "guest=%d\n", p->guest ? 1 : 0);
    fprintf(f, "copy_wifi=%d\n", p->copy_wifi ? 1 : 0);
    fprintf(f, "wifi_ssid=%s\n", p->wifi_ssid);
    fprintf(f, "compositor=%s\n", p->compositor);
    fprintf(f, "compositor_ms=%.2f\n", p->compositor_ms);
    fprintf(f, "firmware=%s\n", plan_firmware_name(p->firmware));
    if (fclose(f) || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int plan_read(const char *path, struct install_plan *p)
{
    FILE *f = fopen(path, "r");

    plan_init(p);
    if (!f)
        return -1;
    int rc = plan_read_file(f, p);
    fclose(f);
    return rc;
}

/* The helper opens and checks the file itself (owner, mode, not a link),
   then reads that same open file: reading it again by name could read
   another file put there in between. */
int plan_read_file(FILE *f, struct install_plan *p)
{
    char line[PLAN_LINE];

    plan_init(p);
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] == '#' || !line[0])
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        if (!strcmp(line, "lang"))
            set_str(p->lang, sizeof p->lang, eq);
        else if (!strcmp(line, "lang_name"))
            set_str(p->lang_name, sizeof p->lang_name, eq);
        else if (!strcmp(line, "cjk"))
            p->cjk = 1;                 /* an old plan's "no" doesn't count: always */
        else if (!strcmp(line, "keyboard"))
            set_str(p->keyboard, sizeof p->keyboard, eq);
        else if (!strcmp(line, "keyboard_name"))
            set_str(p->keyboard_name, sizeof p->keyboard_name, eq);
        else if (!strcmp(line, "timezone"))
            set_str(p->timezone, sizeof p->timezone, eq);
        else if (!strcmp(line, "ntp"))
            p->ntp = atoi(eq);
        else if (!strcmp(line, "hwclock"))
            p->hwclock_local = !strcmp(eq, "local");
        else if (!strcmp(line, "disk"))
            set_str(p->disk, sizeof p->disk, eq);
        else if (!strcmp(line, "disk_name"))
            set_str(p->disk_name, sizeof p->disk_name, eq);
        else if (!strcmp(line, "disk_bytes"))
            p->disk_bytes = strtoull(eq, NULL, 10);
        else if (!strcmp(line, "free_bytes"))
            p->free_bytes = strtoull(eq, NULL, 10);
        else if (!strcmp(line, "mode"))
            p->mode = mode_from(eq);
        else if (!strcmp(line, "keep_esp"))
            p->keep_esp = atoi(eq);
        else if (!strcmp(line, "root_gb"))
            p->root_gb = atoi(eq);
        else if (!strcmp(line, "separate_home"))
            p->separate_home = atoi(eq);
        else if (!strcmp(line, "encrypt"))
            p->encrypt = 0;
        else if (!strcmp(line, "fullname"))
            set_str(p->fullname, sizeof p->fullname, eq);
        else if (!strcmp(line, "user"))
            set_str(p->user, sizeof p->user, eq);
        else if (!strcmp(line, "password"))
            set_str(p->password, sizeof p->password, eq);
        else if (!strcmp(line, "hostname"))
            set_str(p->hostname, sizeof p->hostname, eq);
        else if (!strcmp(line, "ask_password"))
            p->ask_password = atoi(eq);
        else if (!strcmp(line, "guest"))
            p->guest = atoi(eq);
        else if (!strcmp(line, "copy_wifi"))
            p->copy_wifi = atoi(eq);
        else if (!strcmp(line, "wifi_ssid"))
            set_str(p->wifi_ssid, sizeof p->wifi_ssid, eq);
        else if (!strcmp(line, "compositor"))
            set_str(p->compositor, sizeof p->compositor, eq);
        else if (!strcmp(line, "compositor_ms"))
            p->compositor_ms = atof(eq);
        else if (!strcmp(line, "firmware"))
            p->firmware = !strcmp(eq, "bios") ? FW_BIOS : FW_UEFI;
    }
    explicit_bzero(line, sizeof line);  /* the password line was here */
    return 0;
}

/* A language or keyboard name: these go into a shell script that every
   login runs (/etc/profile.d/lang.sh) and into Xorg's settings, so nothing
   that could end a quote or start a command. en_US.UTF-8, us, de(nodeadkeys). */
static int good_word(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("_.-@()+", *s))
            return 0;
    return 1;
}

int plan_check(const struct install_plan *p, char *err, size_t errlen)
{
    if (p->mode == MODE_UPGRADE) {
        if (!p->disk[0] || strncmp(p->disk, "/dev/", 5)) {
            snprintf(err, errlen, "Pick a disk to upgrade.");
            return -1;
        }
        return 0;
    }
    if (!p->lang[0] || !p->keyboard[0] || !p->timezone[0]) {
        snprintf(err, errlen, "Language, keyboard and time zone are required.");
        return -1;
    }
    if (!good_word(p->lang) || !good_word(p->keyboard)) {
        snprintf(err, errlen, "The language or keyboard name has characters it can't have.");
        return -1;
    }
    if (!plan_good_zone(p->timezone)) {
        snprintf(err, errlen, "The time zone isn't a name tzdata uses.");
        return -1;
    }
    if (!plan_good_fullname(p->fullname)) {
        snprintf(err, errlen, "The full name can't be empty or contain : or ,");
        return -1;
    }
    if (!plan_good_user(p->user)) {
        snprintf(err, errlen, "The user name must start with a small letter and use only small letters, digits, - and _.");
        return -1;
    }
    if (!p->password[0]) {
        snprintf(err, errlen, "The new account needs a password.");
        return -1;
    }
    if (!plan_good_hostname(p->hostname)) {
        snprintf(err, errlen, "The computer name must be letters, digits and -, and not start or end with -.");
        return -1;
    }
    if (!p->disk[0] || strncmp(p->disk, "/dev/", 5)) {
        snprintf(err, errlen, "Pick a disk to install to.");
        return -1;
    }
    if (p->root_gb < MIN_INSTALL_GB) {
        snprintf(err, errlen, "MyOS needs at least %d GB for the system.", MIN_INSTALL_GB);
        return -1;
    }
    if (strcmp(p->compositor, "none") && strcmp(p->compositor, "xrender") && strcmp(p->compositor, "gl")) {
        snprintf(err, errlen, "Desktop effects must be none, xrender or gl.");
        return -1;
    }
    if (p->mode == MODE_FREE && p->free_bytes &&
        p->free_bytes < (unsigned long long)MIN_INSTALL_GB * 1000ULL * 1000 * 1000) {
        snprintf(err, errlen, "There is not enough free space (need %d GB).", MIN_INSTALL_GB);
        return -1;
    }
    return 0;
}

static void bytes_gb(unsigned long long n, char *out, size_t len)
{
    unsigned long long gb = (n + 500000000ULL) / 1000000000ULL;
    snprintf(out, len, "%llu GB", gb);
}

void plan_describe(const struct install_plan *p, char *out, size_t n)
{
    char disk_sz[32], data_sz[32];

    if (p->mode == MODE_UPGRADE) {
        char sz[32];
        bytes_gb(p->disk_bytes, sz, sizeof sz);
        snprintf(out, n,
                 "Upgrade\tMyOS on %s%s%s%s\n"
                 "Kept\tfiles on myos-data, accounts, language, keyboard, "
                 "time zone, Wi-Fi, kernel settings\n"
                 "Replaced\tthe system on myos-root and the boot files\n"
                 "Not touched\tpartition table, myos-data, myos-config\n",
                 p->disk_name[0] ? p->disk_name : p->disk,
                 p->disk_bytes ? " (" : "", p->disk_bytes ? sz : "",
                 p->disk_bytes ? ")" : "");
        return;
    }
    unsigned long long pool = p->mode == MODE_ERASE ? p->disk_bytes : p->free_bytes;
    unsigned long long reserved = (p->firmware == FW_UEFI ? ESP_MIB : BOOT_AREA_MIB) * 1024ULL * 1024;
    unsigned long long root = (unsigned long long)p->root_gb * 1000ULL * 1000 * 1000;
    unsigned long long data = pool > reserved + root ? pool - reserved - root : 0;

    bytes_gb(p->disk_bytes, disk_sz, sizeof disk_sz);
    bytes_gb(data, data_sz, sizeof data_sz);

    snprintf(out, n,
             "Language\t%s%s\n"
             "Keyboard\t%s\n"
             "Time zone\t%s; clock %s%s\n"
             "Account\t%s (%s), administrator; computer %s\n"
             "Disk\t%s%s%s%s\n"
             "Will create\tmyos-root %d GB (ext2, /)%s%s%s\n"
             "Will add\t%s\n"
             "Won't touch\t%s\n"
             "Desktop effects\t%s%.1f ms to redraw the screen)\n",
             p->lang_name[0] ? p->lang_name : p->lang,
             p->cjk ? ", with Chinese fonts" : "",
             p->keyboard_name[0] ? p->keyboard_name : p->keyboard,
             p->timezone, p->ntp ? "from the network" : "not from the network",
             p->hwclock_local ? "; hardware clock in local time (Windows)" : "",
             p->fullname, p->user, p->hostname,
             p->disk_name[0] ? p->disk_name : p->disk,
             p->disk_bytes ? " (" : "", p->disk_bytes ? disk_sz : "", p->disk_bytes ? ")" : "",
             p->root_gb,
             p->separate_home ? ", myos-data " : "",
             p->separate_home && data ? data_sz : "",
             p->separate_home ? " (ext2, /home)" : "",
             p->firmware == FW_UEFI
                 ? "a new EFI partition with MyOS's start files (EFI/BOOT, EFI/MYOS)"
                 : "the BIOS boot area (MBR + stage 2, as make hd does)",
             p->mode == MODE_ERASE ? "nothing: everything on the disk is erased"
                                   : (p->free_bytes ? "existing partitions; only the free space is used"
                                                    : "existing partitions (free space unknown)"),
             !strcmp(p->compositor, "gl") ? "Full (measured: "
                                          : !strcmp(p->compositor, "xrender") ? "Simple (measured: "
                                                                             : "Off (measured: ",
             p->compositor_ms);
}
