#include "disks.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_u64(const char *path, unsigned long long *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fscanf(f, "%llu", out) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static int read_trim(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
        out[--len] = 0;
    return 0;
}

void disks_format_bytes(unsigned long long n, char *out, size_t len)
{
    if (n >= 1000000000000ULL)
        snprintf(out, len, "%llu TB", (n + 500000000000ULL) / 1000000000000ULL);
    else if (n >= 1000000000ULL)
        snprintf(out, len, "%llu GB", (n + 500000000ULL) / 1000000000ULL);
    else
        snprintf(out, len, "%llu MB", (n + 500000ULL) / 1000000ULL);
}

void disks_part_path(const char *disk, int partno, char *out, size_t n)
{
    size_t len = strlen(disk);
    if (len && isdigit((unsigned char)disk[len - 1]))
        snprintf(out, n, "%sp%d", disk, partno);
    else
        snprintf(out, n, "%s%d", disk, partno);
}

int disks_firmware_uefi(void)
{
    return access("/sys/firmware/efi", F_OK) == 0;
}

static void slug(const char *in, char *out, size_t n)
{
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && *p != ' ' && i + 1 < n; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (islower(c) || isdigit(c))
            out[i++] = (char)c;
    }
    out[i] = 0;
}

void disks_suggest_hostname(char *out, size_t n, const char *user)
{
    char product[80] = "", family[80] = "", machine[32] = "pc";

    if (read_trim("/sys/class/dmi/id/product_family", family, sizeof family) == 0)
        slug(family, machine, sizeof machine);
    if ((!machine[0] || !strcmp(machine, "pc")) &&
        read_trim("/sys/class/dmi/id/product_name", product, sizeof product) == 0)
        slug(product, machine, sizeof machine);
    if (!machine[0])
        strcpy(machine, "pc");
    if (strstr(family, "Katana") || strstr(product, "Katana") || strstr(family, "MSI") ||
        strstr(product, "MS-"))
        strcpy(machine, "msi");
    snprintf(out, n, "%s-%s", user && user[0] ? user : "myos", machine);
}

/* First quoted ssid= in wpa_supplicant's config, if any. */
int disks_wifi_ssid(char *out, size_t n)
{
    static const char *const files[] = {
        "/etc/wpa_supplicant/wpa_supplicant.conf",
        "/etc/wpa_supplicant.conf",
        NULL
    };
    out[0] = 0;
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "r");
        char line[256];
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            char *s = strstr(line, "ssid=");
            if (!s)
                continue;
            s += 5;
            if (*s == '"') {
                s++;
                char *e = strchr(s, '"');
                if (e)
                    *e = 0;
            } else {
                s[strcspn(s, " \t\n")] = 0;
            }
            if (*s && strcmp(s, "\"\"")) {
                snprintf(out, n, "%s", s);
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    return -1;
}

const struct disk_info *disks_find(const struct disk_info *list, int n, const char *path)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].path, path))
            return &list[i];
    return NULL;
}

static int skip_disk(const char *name)
{
    static const char *const skip[] = { "loop", "ram", "sr", "fd", "dm-", "zram", "md", NULL };
    for (int i = 0; skip[i]; i++)
        if (!strncmp(name, skip[i], strlen(skip[i])))
            return 1;
    return 0;
}

/* Is the mounted device dev (/dev/sda2, /dev/nvme0n1p1, or a whole disk)
   on the disk called sysname? A name ending in a digit (nvme0n1, mmcblk0)
   numbers its partitions after a 'p'; sda's follow it directly. */
static int on_disk(const char *dev, const char *sysname)
{
    size_t n = strlen(sysname);
    if (strncmp(dev, "/dev/", 5) || strncmp(dev + 5, sysname, n))
        return 0;
    const char *rest = dev + 5 + n;
    if (!*rest)
        return 1;
    if (n && isdigit((unsigned char)sysname[n - 1])) {
        if (*rest++ != 'p')
            return 0;
    }
    if (!isdigit((unsigned char)*rest))
        return 0;
    while (isdigit((unsigned char)*rest))
        rest++;
    return *rest == 0;
}

/* The disks MyOS is running from: any with something mounted from it (the
   live system's "/", its boot partition, /data). Never offered as the
   place to install: erasing one would pull the system out from under
   itself, and the kernel refuses to open a mounted disk for writing
   anyway. 0 if /proc/mounts can't be read (then nothing is known). */
#define MOUNTED_MAX 16
static int mounted_devs(char devs[][64], int max)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[256];
    int n = 0;

    if (!f)
        return -1;
    while (n < max && fgets(line, sizeof line, f)) {
        char dev[64];
        if (sscanf(line, "%63s", dev) == 1 && !strncmp(dev, "/dev/", 5))
            snprintf(devs[n++], 64, "%s", dev);
    }
    fclose(f);
    return n;
}

static void parse_uevent(const char *path, char *partname, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[128];

    partname[0] = 0;
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "PARTNAME=", 9)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(partname, n, "%s", line + 9);
            break;
        }
    fclose(f);
}

static int collect_parts(struct disk_info *d)
{
    char dir[128];
    DIR *dp;
    struct dirent *e;
    unsigned long long used = 0;

    snprintf(dir, sizeof dir, "/sys/block/%s", d->sysname);
    dp = opendir(dir);
    if (!dp)
        return 0;
    while ((e = readdir(dp))) {
        char pdir[192], file[256];
        unsigned long long sectors, start = 0;
        struct disk_part *p;

        if (strncmp(e->d_name, d->sysname, strlen(d->sysname)))
            continue;
        snprintf(pdir, sizeof pdir, "%s/%s", dir, e->d_name);
        snprintf(file, sizeof file, "%s/partition", pdir);
        if (access(file, F_OK) != 0)
            continue;
        snprintf(file, sizeof file, "%s/size", pdir);
        if (read_u64(file, &sectors))
            continue;
        snprintf(file, sizeof file, "%s/start", pdir);
        read_u64(file, &start);
        if (d->npart >= DISK_PARTS)
            continue;
        p = &d->parts[d->npart++];
        snprintf(p->name, sizeof p->name, "%s", e->d_name);
        p->bytes = sectors * 512ULL;
        p->start = start;
        snprintf(file, sizeof file, "%s/uevent", pdir);
        parse_uevent(file, p->label, sizeof p->label);
        if (p->bytes >= 80ULL * 1024 * 1024 && p->bytes <= 1024ULL * 1024 * 1024 &&
            (strcasestr(p->label, "efi") || strcasestr(p->label, "ESP") || p->start <= 2048 * 4))
            p->is_esp = 1;
        if (strcasestr(p->label, "myos"))
            p->is_myos = 1;
        used += p->bytes;
    }
    closedir(dp);
    if (d->bytes > used)
        d->free_bytes = d->bytes - used;
    return d->npart;
}

int disks_scan(struct disk_info *out, int max)
{
    DIR *dp = opendir("/sys/block");
    struct dirent *e;
    char mounted[MOUNTED_MAX][64];
    int n = 0, nmounted;

    if (!dp)
        return 0;
    nmounted = mounted_devs(mounted, MOUNTED_MAX);
    while ((e = readdir(dp)) && n < max) {
        char file[192];
        unsigned long long sectors = 0, rem = 0;
        struct disk_info *d;
        int nvme;

        if (e->d_name[0] == '.' || skip_disk(e->d_name))
            continue;
        snprintf(file, sizeof file, "/sys/block/%s/size", e->d_name);
        if (read_u64(file, &sectors) || sectors < 2048)
            continue;
        d = &out[n];
        memset(d, 0, sizeof *d);
        snprintf(d->sysname, sizeof d->sysname, "%s", e->d_name);
        snprintf(d->path, sizeof d->path, "/dev/%s", e->d_name);
        d->bytes = sectors * 512ULL;
        snprintf(file, sizeof file, "/sys/block/%s/removable", e->d_name);
        if (read_u64(file, &rem) == 0)
            d->removable = rem != 0;
        snprintf(file, sizeof file, "/sys/block/%s/device/model", e->d_name);
        if (read_trim(file, d->model, sizeof d->model) != 0) {
            snprintf(file, sizeof file, "/sys/block/%s/device/name", e->d_name);
            read_trim(file, d->model, sizeof d->model);
        }
        nvme = !strncmp(e->d_name, "nvme", 4);
        if (d->model[0])
            snprintf(d->name, sizeof d->name, "%s%s", d->model, nvme ? " (NVMe)" : "");
        else
            snprintf(d->name, sizeof d->name, "%s", d->path);
        collect_parts(d);
        if (nmounted < 0)
            d->in_use = d->removable;   /* no mount table: the old guess, a USB stick */
        for (int k = 0; k < nmounted && !d->in_use; k++)
            d->in_use = on_disk(mounted[k], d->sysname);
        for (int i = 0; i < d->npart; i++)
            if (d->parts[i].is_esp)
                d->has_windows = 1; /* an ESP plus a large partition: likely Windows */
        if (d->has_windows) {
            int large = 0;
            for (int i = 0; i < d->npart; i++)
                if (d->parts[i].bytes > 8ULL * 1000 * 1000 * 1000)
                    large = 1;
            d->has_windows = large;
        }
        if (d->in_use)
            snprintf(d->summary, sizeof d->summary, "MyOS is running from this disk");
        else if (d->has_windows)
            snprintf(d->summary, sizeof d->summary, "Windows likely");
        else if (d->npart)
            snprintf(d->summary, sizeof d->summary, "%d partition%s", d->npart, d->npart == 1 ? "" : "s");
        else
            snprintf(d->summary, sizeof d->summary, "empty");
        n++;
    }
    closedir(dp);
    return n;
}
