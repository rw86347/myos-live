/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kconfig.c — the kernel config: text form, checks, the bytes on the disk
 * (kconfig.h; docs/KERNEL-CONFIG.md). Every rule here is one of
 * tools/kconfig/kconfig.py's, and the bytes are the same: the reference
 * decodes what this writes.
 */
#include "kconfig.h"

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kconfig_features.h"       /* made from tools/kconfig/powerfeatures.py */
_Static_assert(KF_FAMILIES == sizeof ((struct kfeature *)0)->support, "kconfig.h: one support per family");

static const char *const ARCH[] = {NULL, "i386", "x86_64", "arm", "arm64"};
static const char *const MODE[] = {"realtime", "soft-realtime", "power-saving"};
static const char *const RESERVED[] = {"os", "idle", "main", "kernel", NULL};

int kc_nfeatures(void) { return KF_FEATURES; }
const struct kfeature *kc_feature(int i) { return &kfeatures[i]; }
int kc_ncstates(void) { return KF_CSTATES; }
const struct kcstate *kc_cstate(int i) { return &kcstates[i]; }
const char *kc_family_name(int f) { return f >= 0 && f < KF_FAMILIES ? kf_family_name[f] : "this processor"; }

static const struct kfeature *feature_by_key(const char *key)
{
    for (int i = 0; i < KF_FEATURES; i++)
        if (!strcmp(kfeatures[i].key, key))
            return &kfeatures[i];
    return NULL;
}

void kc_default(struct kc_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = KC_SOFT;
#ifdef __x86_64__
    strcpy(c->arch, "x86_64");
#else
    strcpy(c->arch, "i386");
#endif
    strcpy(c->slots[0], "os");
    strcpy(c->slots[10], "os");
    c->rt_flags = KC_RT_LEFTOVER | KC_RT_DEFER_IRQ;
    c->latency_budget_us = 100;
    c->cpu_min_pct = 30;
    c->cpu_max_pct = 100;
    c->epp = 128;
    c->cstate_max = 3;
    c->spindown_min = 10;
}

int kc_good_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > KC_NAME - 1 || !(s[0] >= 'a' && s[0] <= 'z'))
        return 0;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_'))
            return 0;
    for (int i = 0; RESERVED[i]; i++)
        if (!strcmp(s, RESERVED[i]))
            return 0;
    return 1;
}

/* The task names in first-slot order: task 1 is the earliest. */
int kc_tasks(const struct kc_config *c, char names[KC_MAX_TASKS][KC_NAME])
{
    int n = 0;
    for (int s = 0; s < KC_SLOTS; s++) {
        const char *x = c->slots[s];
        if (!x[0] || !strcmp(x, "os"))
            continue;
        int seen = 0;
        for (int k = 0; k < n; k++)
            seen |= !strcmp(names[k], x);
        if (!seen && n < KC_MAX_TASKS)
            snprintf(names[n++], KC_NAME, "%s", x);
    }
    return n;
}

int kc_task_slots(const struct kc_config *c, const char *name, int slots[KC_SLOTS])
{
    int n = 0;
    for (int s = 0; s < KC_SLOTS; s++)
        if (!strcmp(c->slots[s], name))
            slots[n++] = s;
    return n;
}

struct kc_task *kc_prog(struct kc_config *c, const char *name, int create)
{
    for (int i = 0; i < c->nprog; i++)
        if (!strcmp(c->prog[i].name, name))
            return &c->prog[i];
    if (!create)
        return NULL;
    if (c->nprog == KC_MAX_TASKS) {     /* full: drop one no slot uses */
        char names[KC_MAX_TASKS][KC_NAME];
        int n = kc_tasks(c, names), i;
        for (i = 0; i < c->nprog; i++) {
            int used = 0;
            for (int k = 0; k < n; k++)
                used |= !strcmp(names[k], c->prog[i].name);
            if (!used)
                break;
        }
        if (i == c->nprog)
            return NULL;
        c->prog[i] = c->prog[--c->nprog];
    }
    struct kc_task *t = &c->prog[c->nprog++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    snprintf(t->program, sizeof t->program, "/bin/%s", name);   /* a new name runs /bin/<name> */
    t->uid = t->gid = 65534;                                      /* nobody */
    t->start_at_boot = t->restart = 1;
    t->restart_ms = 1000;
    return t;
}

/* "every 5 ms", or the gaps when they differ. The frame wraps. */
void kc_cadence(const int *slots, int n, char *out, size_t size)
{
    if (n <= 1) {
        snprintf(out, size, "every 20 ms");
        return;
    }
    int gaps[KC_SLOTS], same = 1;
    for (int i = 0; i < n; i++) {
        gaps[i] = ((slots[(i + 1) % n] - slots[i]) % KC_SLOTS + KC_SLOTS) % KC_SLOTS;
        same &= gaps[i] == gaps[0];
    }
    if (same) {
        snprintf(out, size, "every %d ms", gaps[0]);
        return;
    }
    size_t at = (size_t)snprintf(out, size, "uneven: ");
    for (int i = 0; i < n && at < size; i++)
        at += (size_t)snprintf(out + at, size - at, "%s%d", i ? "/" : "", gaps[i]);
    if (at < size)
        snprintf(out + at, size - at, " ms");
}

/* --- the text form (docs/kernel-config/example.conf) ------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static uint32_t bits_of(const char *list, int *bad)
{
    uint32_t b = 0;
    char buf[1024], *save = NULL;
    snprintf(buf, sizeof buf, "%s", list);
    for (char *w = strtok_r(buf, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
        const struct kfeature *f = feature_by_key(w);
        if (f)
            b |= 1u << f->bit;
        else
            *bad = 1;
    }
    return b;
}

int kc_parse(const char *text, struct kc_config *c, char *err, size_t n)
{
    char section[64] = "", *copy = strdup(text), *save = NULL;
    struct kc_task *task = NULL;
    int line = 0;
    if (!copy)
        return snprintf(err, n, "out of memory"), -1;
    kc_default(c);
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        line++;
        char *hash = strchr(l, '#');
        if (hash)
            *hash = 0;
        l = trim(l);
        if (!*l)
            continue;
        size_t len = strlen(l);
        if (l[0] == '[' && l[len - 1] == ']') {
            l[len - 1] = 0;
            snprintf(section, sizeof section, "%s", trim(l + 1));
            task = !strncmp(section, "task ", 5) ? kc_prog(c, trim(section + 5), 1) : NULL;
            if (!strncmp(section, "task ", 5) && task) {
                task->program[0] = 0;   /* the section says which program */
                task->args[0] = 0;
            }
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            snprintf(err, n, "line %d: expected key = value", line);
            free(copy);
            return -1;
        }
        *eq = 0;
        char *key = trim(l), *val = trim(eq + 1);
        long num = strtol(val, NULL, 0);
        int bad = 0;
        if (!strncmp(section, "task ", 5)) {
            if (!task)
                bad = 1;
            else if (!strcmp(key, "program"))
                snprintf(task->program, sizeof task->program, "%s", val);
            else if (!strcmp(key, "args"))
                snprintf(task->args, sizeof task->args, "%s", val);
            else if (!strcmp(key, "uid"))
                task->uid = (unsigned)num;
            else if (!strcmp(key, "gid"))
                task->gid = (unsigned)num;
            else if (!strcmp(key, "restart_ms"))
                task->restart_ms = (unsigned)num;
            else if (!strcmp(key, "start"))
                task->start_at_boot = !strcmp(val, "boot");
            else if (!strcmp(key, "restart"))
                task->restart = !strcmp(val, "yes");
            else
                bad = 1;
        } else if (!strcmp(section, "slots")) {
            char *end;
            long s = strtol(key, &end, 10);
            if (*end || s < 0 || s >= KC_SLOTS || strlen(val) >= KC_NAME)
                bad = 1;
            else
                snprintf(c->slots[s], KC_NAME, "%s", val);
        } else if (!strcmp(section, "power") && !strcmp(key, "on"))
            c->power_on = bits_of(val, &bad);
        else if (!strcmp(section, "power") && !strcmp(key, "seen"))
            c->power_seen = bits_of(val, &bad);
        else if (!strcmp(key, "mode")) {
            int m;
            for (m = 0; m < 3 && strcmp(val, MODE[m]); m++)
                ;
            if (m == 3)
                bad = 1;
            else
                c->mode = m;
        } else if (!strcmp(key, "arch"))
            snprintf(c->arch, sizeof c->arch, "%s", val);
        else if (!strcmp(key, "byte_order"))
            c->big = !strcmp(val, "big");
        else if (!strcmp(key, "latency_budget_us"))
            c->latency_budget_us = (unsigned)num;
        else if (!strcmp(key, "rt_flags"))
            c->rt_flags = (unsigned)num;
        else if (!strcmp(key, "cpu_min_pct"))
            c->cpu_min_pct = (int)num;
        else if (!strcmp(key, "cpu_max_pct"))
            c->cpu_max_pct = (int)num;
        else if (!strcmp(key, "epp"))
            c->epp = (int)num;
        else if (!strcmp(key, "cstate_max"))
            c->cstate_max = (int)num;
        else if (!strcmp(key, "spindown_min"))
            c->spindown_min = (int)num;
        else if (!strcmp(key, "generation"))
            c->generation = (uint32_t)num;
        else
            bad = 1;
        if (bad) {
            snprintf(err, n, "line %d: '%s' isn't something the config has", line, key);
            free(copy);
            return -1;
        }
    }
    free(copy);
    return 0;
}

static void addf(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (w >= 0 && (size_t)w < *cap - *len) {
            *len += (size_t)w;
            return;
        }
        *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
}

char *kc_format(const struct kc_config *c)
{
    size_t len = 0, cap = 2048;
    char *b = malloc(cap), names[KC_MAX_TASKS][KC_NAME];
    int n = kc_tasks(c, names);
    addf(&b, &len, &cap, "# MyOS kernel configuration (docs/KERNEL-CONFIG.md)\n");
    addf(&b, &len, &cap, "mode = %s\narch = %s\n", MODE[c->mode], c->arch);
    if (c->big)
        addf(&b, &len, &cap, "byte_order = big\n");
    addf(&b, &len, &cap, "latency_budget_us = %u\nrt_flags = %u\n", c->latency_budget_us, c->rt_flags);
    if (c->generation)
        addf(&b, &len, &cap, "generation = %u\n", c->generation);
    addf(&b, &len, &cap, "\n[slots]\n");
    for (int s = 0; s < KC_SLOTS; s++)
        if (c->slots[s][0] && strcmp(c->slots[s], "os"))
            addf(&b, &len, &cap, "%d = %s\n", s, c->slots[s]);
    for (int i = 0; i < n; i++) {
        const struct kc_task *t = kc_prog((struct kc_config *)c, names[i], 0);
        if (!t)
            continue;
        addf(&b, &len, &cap, "\n[task %s]\nprogram = %s\n", t->name, t->program);
        if (t->args[0])
            addf(&b, &len, &cap, "args = %s\n", t->args);
        addf(&b, &len, &cap, "uid = %u\ngid = %u\nstart = %s\nrestart = %s\nrestart_ms = %u\n", t->uid, t->gid,
             t->start_at_boot ? "boot" : "manual", t->restart ? "yes" : "no", t->restart_ms);
    }
    addf(&b, &len, &cap, "\n[power]\non =");
    for (int i = 0; i < KF_FEATURES; i++)
        if (c->power_on >> kfeatures[i].bit & 1)
            addf(&b, &len, &cap, " %s", kfeatures[i].key);
    addf(&b, &len, &cap, "\nseen =");
    for (int i = 0; i < KF_FEATURES; i++)
        if (c->power_seen >> kfeatures[i].bit & 1)
            addf(&b, &len, &cap, " %s", kfeatures[i].key);
    addf(&b, &len, &cap, "\ncpu_min_pct = %d\ncpu_max_pct = %d\nepp = %d\ncstate_max = %d\nspindown_min = %d\n",
         c->cpu_min_pct, c->cpu_max_pct, c->epp, c->cstate_max, c->spindown_min);
    return b;
}

/* --- this computer ----------------------------------------------------------------- */

static int read_line_of(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, strlen(key))) {
            char *v = strchr(line, ':');
            snprintf(out, n, "%s", v ? trim(v + 1) : "");
            fclose(f);
            return 0;
        }
    fclose(f);
    return -1;
}

static int any_entry(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d)
        return 0;
    while (!found && (e = readdir(d)))
        found = !strncmp(e->d_name, prefix, strlen(prefix));
    closedir(d);
    return found;
}

/* A fixed (not removable) disk whose name starts with prefix: a USB stick
   is "sd" too, but it doesn't spin and isn't on a SATA link. */
static int fixed_disk(const char *prefix)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int found = 0;
    if (!d)
        return 0;
    while (!found && (e = readdir(d))) {
        char path[300];
        unsigned long long rem = 1;
        if (strncmp(e->d_name, prefix, strlen(prefix)))
            continue;
        snprintf(path, sizeof path, "/sys/block/%s/removable", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%llu", &rem) != 1)
                rem = 1;
            fclose(f);
        }
        found = rem == 0;
    }
    closedir(d);
    return found;
}

static void machine_has(struct kc_machine *m, const char *key, int has, const char *why)
{
    const struct kfeature *f = feature_by_key(key);
    if (f) {
        m->have[f->bit] = (unsigned char)has;
        m->why[f->bit] = has == KC_LACKS ? why : NULL;
    }
}

/* CPUID leaf `leaf`, or zeros if the processor doesn't have it (or isn't
   an x86: the host test on the Mac). */
static void cpuid(unsigned leaf, unsigned r[4])
{
    r[0] = r[1] = r[2] = r[3] = 0;
#if defined(__i386__) || defined(__x86_64__)
    __get_cpuid_count(leaf, 0, &r[0], &r[1], &r[2], &r[3]);  /* checks the leaf exists */
#else
    (void)leaf;
#endif
}

/* What this computer has, as far as a program can see: the processor
   answers CPUID itself; the devices are in /sys and /dev. Some savings only
   the kernel can tell (a SATA link's power states, PCIe ASPM, USB): those
   are KC_MAYBE, switchable, and the kernel skips what isn't there when it
   starts (docs/KERNEL-CONFIG.md §7, "bits a machine lacks"). */
void kc_detect(struct kc_machine *m)
{
    char vendor[32] = "";
    unsigned r1[4], r6[4], e7[4], e8[4];
    memset(m, 0, sizeof *m);
    m->family = -1;
    read_line_of("/proc/cpuinfo", "vendor_id", vendor, sizeof vendor);
    read_line_of("/proc/cpuinfo", "model name", m->cpu, sizeof m->cpu);
    if (strstr(m->cpu, "Pentium(R) M") || strstr(m->cpu, "Pentium M"))
        m->family = 1;
    else if (strstr(m->cpu, "Pentium(R) 4") || strstr(m->cpu, "Pentium 4"))
        m->family = 0;
    else if (!strcmp(vendor, "AuthenticAMD") && strstr(m->cpu, "Ryzen"))
        m->family = 2;
    cpuid(1, r1);
    cpuid(6, r6);
    cpuid(0x80000007, e7);
    cpuid(0x80000008, e8);
    /* Intel Core from Skylake on: the ones with HWP (the P2 Max, the Katana). */
    if (m->family < 0 && !strcmp(vendor, "GenuineIntel") && strstr(m->cpu, "Core") && (r6[0] >> 7 & 1))
        m->family = 3;
    m->virt = r1[2] >> 31 & 1;                          /* the hypervisor bit */
    const char *vm = "A virtual machine's processor: the host (your Mac) manages its power";

    /* The processor, by its own feature bits. */
    int eist = r1[2] >> 7 & 1, cppc = e8[1] >> 27 & 1, hwpstate = e7[3] >> 7 & 1;
    machine_has(m, "cpu_speed", eist || cppc || hwpstate || m->family == 1 ? KC_HAS : KC_LACKS,
                m->virt ? vm : "This processor can't change its speed (no SpeedStep or CPPC in CPUID)");
    int boost = (e7[3] >> 9 & 1) || (r6[0] >> 1 & 1);    /* AMD CPB, Intel Turbo Boost */
    machine_has(m, "no_boost", boost ? KC_HAS : KC_LACKS, m->virt ? vm : "This processor has no boost");
    int mwait = r1[2] >> 3 & 1;
    machine_has(m, "deep_idle", m->family == 1 ? KC_HAS : mwait ? KC_MAYBE : KC_LACKS,
                m->virt ? vm : "No idle states deeper than halt (no MWAIT, not a Pentium M)");
    machine_has(m, "c1e", m->family == 1 ? KC_LACKS : m->virt ? KC_LACKS : KC_MAYBE,
                m->virt ? vm : "Not on a Pentium M");
    int odcm = (r1[3] >> 22 & 1) && (r1[3] >> 29 & 1); /* ACPI clock modulation + TM */
    machine_has(m, "throttle", odcm ? KC_HAS : KC_LACKS, m->virt ? vm : "No clock modulation (CPUID)");
    machine_has(m, "tickless", r1[3] >> 9 & 1 ? KC_HAS : KC_LACKS, "No local APIC");
    machine_has(m, "smt_off", KC_LACKS, "Needs MyOS on more than one core");
    machine_has(m, "core_park", KC_LACKS, "Needs MyOS on more than one core");

    /* The devices. */
    int ata = fixed_disk("hd"), sata = fixed_disk("sd");
    machine_has(m, "disk_spindown", ata || sata ? KC_MAYBE : KC_LACKS, "No hard disk (a USB stick or SSD has no motor)");
    machine_has(m, "sata_lpm", sata ? KC_MAYBE : KC_LACKS, "No SATA disk");
    machine_has(m, "nvme_apst", any_entry("/sys/block", "nvme") ? KC_HAS : KC_LACKS, "No NVMe SSD");
    machine_has(m, "pcie_aspm", KC_MAYBE, NULL);
    machine_has(m, "usb_autosuspend", KC_MAYBE, NULL);
    machine_has(m, "wifi_ps", any_entry("/sys/class/net", "wlan") ? KC_HAS : KC_LACKS, "No Wi-Fi");
    machine_has(m, "audio_pm", any_entry("/dev/snd", "pcm") || any_entry("/dev/snd", "control") ? KC_HAS : KC_LACKS,
                "No sound device: MyOS has no driver for this one yet");
    machine_has(m, "nic_idle", any_entry("/sys/class/net", "eth") ? KC_HAS : KC_LACKS,
                "No wired Ethernet (or no driver for it yet)");
    machine_has(m, "gpu_pm", KC_LACKS, "MyOS has no graphics driver with power management yet");
}

int kc_feature_ok(const struct kc_machine *m, const struct kfeature *f, const char **why)
{
    static char buf[160];
    /* A family we know rules out what it never has, whatever the bits say. */
    if (m->family >= 0 && f->support[m->family] == KF_NO) {
        snprintf(buf, sizeof buf, "Not on a %s", kf_family_name[m->family]);
        *why = buf;
        return KC_LACKS;
    }
    if (m->have[f->bit] == KC_LACKS) {
        *why = m->why[f->bit] ? m->why[f->bit] : "Not in this computer";
        return KC_LACKS;
    }
    *why = m->have[f->bit] == KC_MAYBE ? "Checked when MyOS starts" : NULL;
    return m->have[f->bit];
}

/* An idle state this processor may have: its family's, or for one we don't
   know that has MWAIT, the usual ones (the kernel takes ACPI's list). */
int kc_cstate_ok(const struct kc_machine *m, int n)
{
    const struct kfeature *deep = feature_by_key("deep_idle");
    for (int i = 0; i < KF_CSTATES; i++)
        if (kcstates[i].n == n) {
            if (n == 1)
                return 1;
            if (m->family >= 0)
                return kcstates[i].families >> m->family & 1;
            return m->have[deep->bit] != KC_LACKS && n != 4;    /* C4 is the Pentium M's */
        }
    return 0;
}

const struct kcstate *kc_soft_cstate(const struct kc_machine *m, unsigned budget_us)
{
    const struct kcstate *best = &kcstates[0];
    for (int i = 0; i < KF_CSTATES; i++) {      /* as powerfeatures.py: down while each fits */
        if (!kc_cstate_ok(m, kcstates[i].n))
            continue;
        if ((unsigned)kcstates[i].wake_us > budget_us)
            break;
        best = &kcstates[i];
    }
    return best;
}

/* --- the checks Save runs ----------------------------------------------------------- */

#define ERR(...)  do { if (o->nerr < KC_MSGS) snprintf(o->err[o->nerr++], KC_MSG, __VA_ARGS__); } while (0)
#define WARN(...) do { if (o->nwarn < KC_MSGS) snprintf(o->warn[o->nwarn++], KC_MSG, __VA_ARGS__); } while (0)
#define NOTE(...) do { if (o->nnote < KC_MSGS) snprintf(o->note[o->nnote++], KC_MSG, __VA_ARGS__); } while (0)

/* The program must exist, be a program, and be writable only by root (nor
   may its folders be): whoever could change it would own a real-time slot. */
static const char *program_problem(const struct kc_task *t, int check_files)
{
    static char buf[KC_MSG];
    const char *p = t->program;
    if (p[0] != '/' || strstr(p, "/../") || strstr(p, "//") || (strlen(p) >= 3 && !strcmp(p + strlen(p) - 3, "/..")))
        return snprintf(buf, sizeof buf, "the program must be a full path, like /bin/%s", t->name), buf;
    for (const char *q = p; *q; q++)
        if (!isprint((unsigned char)*q))
            return "the program path has a character it can't have";
    if (!check_files)
        return NULL;
    struct stat st;
    if (stat(p, &st))
        return snprintf(buf, sizeof buf, "%s isn't there", p), buf;
    if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111))
        return snprintf(buf, sizeof buf, "%s isn't a program (a file with x permission)", p), buf;
    if ((st.st_mode & 022) || (t->uid == 0 && st.st_uid != 0))
        return snprintf(buf, sizeof buf, "%s can be changed by others: only root may write it", p), buf;
    char dir[KC_PROGRAM];
    snprintf(dir, sizeof dir, "%s", p);
    for (char *slash = strrchr(dir, '/'); slash && slash != dir; slash = strrchr(dir, '/')) {
        *slash = 0;
        if (!stat(dir, &st) && (st.st_mode & 002))
            return snprintf(buf, sizeof buf, "%s can be written by anyone", dir), buf;
    }
    return NULL;
}

void kc_check(const struct kc_config *c, const struct kc_machine *m, int check_files, struct kc_msgs *o)
{
    char names[KC_MAX_TASKS][KC_NAME];
    memset(o, 0, sizeof *o);
    int known_arch = 0;
    for (int a = 1; a <= 4; a++)
        known_arch |= !strcmp(c->arch, ARCH[a]);
    if (!known_arch)
        ERR("unknown arch '%s'", c->arch);
    if (c->mode < 0 || c->mode > 2)
        ERR("the mode is realtime, soft realtime or power savings");
    for (int s = 0; s < KC_SLOTS; s++) {
        const char *x = c->slots[s];
        if ((s == 0 || s == 10) && strcmp(x, "os"))
            ERR("slot %d belongs to the OS", s);
        else if (s != 0 && s != 10 && !strcmp(x, "os"))
            ERR("slot %d: only slots 0 and 10 are the OS", s);
        else if (s != 0 && s != 10 && x[0] && !kc_good_name(x))
            ERR("slot %d: \"%s\" must start with a small letter and use only a-z, 0-9, - and _ "
                "(not os, idle, main or kernel)", s, x);
    }
    int n = kc_tasks(c, names), free_slots = 0;
    for (int s = 0; s < KC_SLOTS; s++)
        free_slots += !c->slots[s][0];
    if (c->mode != KC_POWER) {
        for (int i = 0; i < n; i++) {
            int sl[KC_SLOTS], k = kc_task_slots(c, names[i], sl);
            char cad[64], list[80] = "";
            size_t at = 0;
            kc_cadence(sl, k, cad, sizeof cad);
            for (int j = 0; j < k && at < sizeof list; j++)
                at += (size_t)snprintf(list + at, sizeof list - at, "%s%d", j ? ", " : "", sl[j]);
            NOTE("%s: slot%s %s - %s", names[i], k > 1 ? "s" : "", list, cad);
        }
        if (free_slots < 2)
            WARN("only %d free slot%s: ordinary programs (the desktop too) get little time", free_slots,
                 free_slots == 1 ? "" : "s");
        if (!(c->rt_flags & KC_RT_LEFTOVER) && free_slots == 0)
            ERR("no free slots and leftover time is off: nothing but the named tasks could run");
    }
    if (c->mode == KC_SOFT) {
        if (c->latency_budget_us < 1 || c->latency_budget_us > 1000)
            ERR("the latency budget is 1 to 1000 us");
        else {
            const struct kcstate *cs = kc_soft_cstate(m, c->latency_budget_us);
            NOTE("soft realtime may idle down to %s (wakes in about %d us)", cs->name, cs->wake_us);
        }
    }
    for (int i = 0; i < n; i++) {
        const struct kc_task *t = kc_prog((struct kc_config *)c, names[i], 0);
        if (!t || !t->program[0]) {
            ERR("%s: which program runs in its slots? (e.g. /bin/%s)", names[i], names[i]);
            continue;
        }
        const char *why = program_problem(t, check_files);
        if (why)
            ERR("%s: %s", t->name, why);
        size_t stored = 1;                          /* the final empty argument */
        int odd = 0;
        for (const char *q = t->args; *q; q++)
            odd |= !isprint((unsigned char)*q);
        char a[KC_ARGS], *save = NULL;
        snprintf(a, sizeof a, "%s", t->args);
        for (char *w = strtok_r(a, " \t", &save); w; w = strtok_r(NULL, " \t", &save))
            stored += strlen(w) + 1;
        if (stored > KC_ARGS)
            ERR("%s: the arguments are longer than %d bytes", t->name, KC_ARGS - 1);
        if (odd)
            ERR("%s: an argument has a character it can't have", t->name);
        if (t->uid == 0)
            WARN("%s runs as root, with full control of the computer", t->name);
        if (!t->start_at_boot)
            WARN("%s doesn't start at boot: its slots stay empty until something starts it", t->name);
        if (t->restart_ms > 60000)
            ERR("%s: restart after 0 to 60000 ms", t->name);
    }
    if (c->mode == KC_POWER && n)
        NOTE("the tasks still start, as ordinary programs: there are no slots in this mode");
    for (int i = 0; i < KF_FEATURES; i++) {
        const struct kfeature *f = &kfeatures[i];
        const char *why;
        if (!(c->power_on >> f->bit & 1))
            continue;
        if (!kc_feature_ok(m, f, &why) && c->mode == KC_POWER)
            ERR("%s: %s", f->label, why);
    }
    if (c->mode == KC_POWER) {
        if (!c->power_on)
            WARN("every saving is off: soft realtime saves more than this");
        if (!(10 <= c->cpu_min_pct && c->cpu_min_pct <= c->cpu_max_pct && c->cpu_max_pct <= 100))
            ERR("processor speed: 10%% <= slowest <= fastest <= 100%%");
        if (c->epp < 0 || c->epp > 255)
            ERR("the energy preference is 0 to 255");
        if ((c->power_on >> feature_by_key("deep_idle")->bit & 1) && !kc_cstate_ok(m, c->cstate_max))
            ERR("C%d isn't an idle state of %s", c->cstate_max, kc_family_name(m->family));
        if (c->power_on >> feature_by_key("disk_spindown")->bit & 1) {
            if (c->spindown_min < 1 || c->spindown_min > 600)
                ERR("spin down after 1 to 600 minutes");
            else if (c->spindown_min < 10)
                WARN("a disk stopped and started every few minutes wears out sooner (10 min or more)");
        }
    }
}

/* --- the bytes ---------------------------------------------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc32_of(const uint8_t *p, size_t n, size_t skip)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (i >= skip && i < skip + 4) ? 0 : p[i];  /* the crc32 field counts as 0 */
        c = crc_table[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, unsigned v, int big)
{
    p[big ? 1 : 0] = (uint8_t)v;
    p[big ? 0 : 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v, int big)
{
    for (int i = 0; i < 4; i++)
        p[big ? 3 - i : i] = (uint8_t)(v >> (8 * i));
}

static unsigned get16(const uint8_t *p, int big)
{
    return big ? (unsigned)(p[0] << 8 | p[1]) : (unsigned)(p[1] << 8 | p[0]);
}

static uint32_t get32(const uint8_t *p, int big)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[big ? 3 - i : i] << (8 * i);
    return v;
}

static int arch_code(const char *a)
{
    for (int i = 1; i <= 4; i++)
        if (!strcmp(a, ARCH[i]))
            return i;
    return 0;
}

void kc_encode(const struct kc_config *c, uint32_t generation, uint8_t boot[KC_BOOT], uint8_t tasks[KC_TASKS])
{
    char names[KC_MAX_TASKS][KC_NAME];
    int n = kc_tasks(c, names), big = c->big;
    memset(boot, 0, KC_BOOT);
    memcpy(boot, "MYOSKCFG", 8);
    boot[0x08] = big ? 2 : 1;
    boot[0x09] = 1;
    boot[0x0A] = (uint8_t)arch_code(c->arch);
    boot[0x0B] = (uint8_t)c->mode;
    put16(boot + 0x0C, KC_BOOT, big);
    put16(boot + 0x0E, 1000, big);
    put32(boot + 0x10, generation, big);
    boot[0x18] = KC_SLOTS;
    boot[0x19] = (uint8_t)n;
    boot[0x1A] = 0;
    boot[0x1B] = 10;
    put32(boot + 0x1C, c->rt_flags, big);
    for (int s = 0; s < KC_SLOTS; s++) {
        uint8_t o = 0;
        if (!strcmp(c->slots[s], "os"))
            o = 0xFF;
        else
            for (int k = 0; k < n; k++)
                if (!strcmp(c->slots[s], names[k]))
                    o = (uint8_t)(k + 1);
        boot[0x20 + s] = o;
    }
    put16(boot + 0x34, c->latency_budget_us, big);
    for (int k = 0; k < n; k++)
        memcpy(boot + 0x40 + 16 * k, names[k], strlen(names[k]));
    put32(boot + 0x160, c->power_on, big);
    put32(boot + 0x164, c->power_seen, big);
    boot[0x168] = (uint8_t)c->cpu_min_pct;
    boot[0x169] = (uint8_t)c->cpu_max_pct;
    boot[0x16A] = (uint8_t)c->epp;
    boot[0x16B] = (uint8_t)c->cstate_max;
    put16(boot + 0x16C, (unsigned)c->spindown_min, big);
    put32(boot + 0x14, crc32_of(boot, KC_BOOT, 0x14), big);

    memset(tasks, 0, KC_TASKS);
    memcpy(tasks, "MYOSTASK", 8);
    tasks[0x08] = big ? 2 : 1;
    tasks[0x09] = 1;
    tasks[0x0A] = (uint8_t)n;
    put32(tasks + 0x0C, KC_TASKS, big);
    put32(tasks + 0x10, generation, big);
    put16(tasks + 0x18, 256, big);
    for (int k = 0; k < n; k++) {
        uint8_t *e = tasks + 0x40 + 256 * k;
        const struct kc_task *t = kc_prog((struct kc_config *)c, names[k], 0);
        memcpy(e, names[k], strlen(names[k]));
        if (!t)
            continue;
        memcpy(e + 0x10, t->program, strnlen(t->program, KC_PROGRAM - 1));
        char a[KC_ARGS], *save = NULL;
        size_t at = 0;
        snprintf(a, sizeof a, "%s", t->args);
        for (char *w = strtok_r(a, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
            size_t l = strlen(w);
            if (at + l + 2 > KC_ARGS)
                break;
            memcpy(e + 0x90 + at, w, l);
            at += l + 1;            /* and its NUL; the empty one after is already 0 */
        }
        put32(e + 0xF0, t->uid, big);
        put32(e + 0xF4, t->gid, big);
        put16(e + 0xF8, (t->start_at_boot ? 1u : 0u) | (t->restart ? 2u : 0u), big);
        put16(e + 0xFA, t->restart_ms, big);
    }
    put32(tasks + 0x14, crc32_of(tasks, KC_TASKS, 0x14), big);
}

int kc_decode(const uint8_t *boot, const uint8_t *tasks, struct kc_config *c, char *err, size_t n)
{
    kc_default(c);
    if (memcmp(boot, "MYOSKCFG", 8))
        return snprintf(err, n, "no MYOSKCFG magic (never written)"), -1;
    if (boot[0x08] != 1 && boot[0x08] != 2)
        return snprintf(err, n, "unknown byte order %d", boot[0x08]), -1;
    int big = boot[0x08] == 2;
    if (boot[0x09] != 1 || get16(boot + 0x0C, big) != KC_BOOT)
        return snprintf(err, n, "a version this reader doesn't know"), -1;
    if (get32(boot + 0x14, big) != crc32_of(boot, KC_BOOT, 0x14))
        return snprintf(err, n, "the boot block's CRC doesn't match"), -1;
    if (boot[0x0A] < 1 || boot[0x0A] > 4 || boot[0x0B] > 2 || boot[0x18] != KC_SLOTS ||
        get16(boot + 0x0E, big) != 1000 || boot[0x1A] != 0 || boot[0x1B] != 10 || boot[0x19] > KC_MAX_TASKS)
        return snprintf(err, n, "a boot block this reader doesn't understand"), -1;
    c->big = big;
    snprintf(c->arch, sizeof c->arch, "%s", ARCH[boot[0x0A]]);
    c->mode = boot[0x0B];
    c->generation = get32(boot + 0x10, big);
    c->rt_flags = get32(boot + 0x1C, big);
    c->latency_budget_us = get16(boot + 0x34, big);
    int ntask = boot[0x19];
    char names[KC_MAX_TASKS][KC_NAME];
    for (int k = 0; k < ntask; k++) {
        memcpy(names[k], boot + 0x40 + 16 * k, 15);
        names[k][15] = 0;
        if (!kc_good_name(names[k]))
            return snprintf(err, n, "a task name that isn't one"), -1;
    }
    for (int s = 0; s < KC_SLOTS; s++) {
        uint8_t o = boot[0x20 + s];
        if (o == 0xFF)
            strcpy(c->slots[s], "os");
        else if (o == 0)
            c->slots[s][0] = 0;
        else if (o <= ntask)
            snprintf(c->slots[s], KC_NAME, "%s", names[o - 1]);
        else
            return snprintf(err, n, "slot %d names task %d of %d", s, o, ntask), -1;
    }
    c->power_on = get32(boot + 0x160, big);
    c->power_seen = get32(boot + 0x164, big);
    c->cpu_min_pct = boot[0x168];
    c->cpu_max_pct = boot[0x169];
    c->epp = boot[0x16A];
    c->cstate_max = boot[0x16B];
    c->spindown_min = (int)get16(boot + 0x16C, big);

    if (memcmp(tasks, "MYOSTASK", 8) || tasks[0x08] != boot[0x08])
        return snprintf(err, n, "no task list beside the boot block"), -1;
    if (get32(tasks + 0x14, big) != crc32_of(tasks, KC_TASKS, 0x14))
        return snprintf(err, n, "the task list's CRC doesn't match"), -1;
    if (get32(tasks + 0x10, big) != c->generation)
        return snprintf(err, n, "task list generation %u, boot block %u: a save was cut off",
                        get32(tasks + 0x10, big), c->generation), -1;
    if (tasks[0x0A] != ntask || get16(tasks + 0x18, big) != 256 || get32(tasks + 0x0C, big) != KC_TASKS)
        return snprintf(err, n, "the task list doesn't match the boot block"), -1;
    for (int k = 0; k < ntask; k++) {
        const uint8_t *e = tasks + 0x40 + 256 * k;
        if (strncmp((const char *)e, names[k], 16))
            return snprintf(err, n, "task list entry %d isn't %s", k, names[k]), -1;
        struct kc_task *t = kc_prog(c, names[k], 1);
        memcpy(t->program, e + 0x10, KC_PROGRAM - 1);
        t->program[KC_PROGRAM - 1] = 0;
        size_t at = 0, out = 0;
        while (at < KC_ARGS && e[0x90 + at]) {
            size_t l = strnlen((const char *)e + 0x90 + at, KC_ARGS - at);
            if (out + l + 2 > KC_ARGS)
                break;
            if (out)
                t->args[out++] = ' ';
            memcpy(t->args + out, e + 0x90 + at, l);
            out += l;
            at += l + 1;
        }
        t->args[out] = 0;
        t->uid = get32(e + 0xF0, big);
        t->gid = get32(e + 0xF4, big);
        unsigned fl = get16(e + 0xF8, big);
        t->start_at_boot = fl & 1;
        t->restart = (fl >> 1) & 1;
        t->restart_ms = get16(e + 0xFA, big);
    }
    return 0;
}

/* --- the partition ------------------------------------------------------------------ */

/* The disk "/" is on: /dev/sda2 -> sda, /dev/nvme0n1p2 -> nvme0n1. */
static int root_disk(char *disk, size_t n)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[512], dev[128], dir[256];
    int found = -1;
    if (!f)
        return -1;
    while (found && fgets(line, sizeof line, f))
        if (sscanf(line, "%127s %255s", dev, dir) == 2 && !strcmp(dir, "/") && !strncmp(dev, "/dev/", 5)) {
            size_t l = strlen(dev);
            while (l > 5 && isdigit((unsigned char)dev[l - 1]))
                l--;
            if (l > 6 && dev[l - 1] == 'p' && isdigit((unsigned char)dev[l - 2]))
                l--;                    /* nvme0n1p2 */
            dev[l] = 0;
            snprintf(disk, n, "%s", dev + 5);
            found = 0;
        }
    fclose(f);
    return found;
}

static int read_all(int fd, uint8_t *buf, size_t n, off_t at);

static uint64_t get64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* MyOS's partition type for the config (tools/mkuefi.py), as GPT stores a
   GUID: the first three fields little-endian. */
static const uint8_t CONFIG_GUID[16] = {0xB2, 0xAF, 0x90, 0x93, 0x45, 0xC5, 0x7E, 0x48,
                                        0xAF, 0x50, 0x36, 0x48, 0xE5, 0x6B, 0x73, 0x6C};

/* The first sector of the GPT entry named myos-config (or of MyOS's config
   type) on the whole disk, read-only; 0 if there is none. For a disk whose
   hybrid MBR the kernel reads instead of its GPT: the partition is there,
   but /sys has no PARTNAME for it, only its start. */
static uint64_t gpt_config_start(const char *disk)
{
    char dev[80];
    uint8_t hdr[KC_SECTOR], ent[128];
    uint64_t found = 0;
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    if (!read_all(fd, hdr, KC_SECTOR, KC_SECTOR) && !memcmp(hdr, "EFI PART", 8)) {
        uint64_t lba = get64le(hdr + 72);
        uint32_t count = get32(hdr + 80, 0), size = get32(hdr + 84, 0);
        for (uint32_t i = 0; i < count && i < 128 && size >= 128 && !found; i++) {
            if (read_all(fd, ent, sizeof ent, (off_t)(lba * KC_SECTOR + (uint64_t)i * size)))
                break;
            int named = 1;
            const char *want = "myos-config";
            for (int c = 0; c < 12; c++)        /* the name is UTF-16LE, NUL-ended */
                named &= ent[56 + 2 * c] == (uint8_t)want[c] && ent[57 + 2 * c] == 0;
            if (named || !memcmp(ent, CONFIG_GUID, 16))
                found = get64le(ent + 32);
        }
    }
    close(fd);
    return found;
}

int kc_find_partition(char *path, size_t n)
{
    char disk[64], dir[160];
    if (root_disk(disk, sizeof disk))
        return -1;
    snprintf(dir, sizeof dir, "/sys/block/%s", disk);
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = -1;
    if (!d)
        return -1;
    while (found && (e = readdir(d))) {
        char uevent[400], line[128];
        if (strncmp(e->d_name, disk, strlen(disk)) || !e->d_name[strlen(disk)])
            continue;
        snprintf(uevent, sizeof uevent, "%s/%s/uevent", dir, e->d_name);
        FILE *f = fopen(uevent, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f))
            if (!strcmp(trim(line), "PARTNAME=myos-config")) {
                snprintf(path, n, "/dev/%s", e->d_name);
                found = 0;
            }
        fclose(f);
    }
    closedir(d);
    if (!found)
        return 0;
    /* No name in /sys: the partition whose first sector is the GPT's. */
    uint64_t start = gpt_config_start(disk);
    if (!start || !(d = opendir(dir)))
        return -1;
    while (found && (e = readdir(d))) {
        char file[400];
        unsigned long long s = 0;
        if (strncmp(e->d_name, disk, strlen(disk)) || !e->d_name[strlen(disk)])
            continue;
        snprintf(file, sizeof file, "%s/%s/start", dir, e->d_name);
        FILE *f = fopen(file, "r");
        if (f && fscanf(f, "%llu", &s) == 1 && s == start) {
            snprintf(path, n, "/dev/%s", e->d_name);
            found = 0;
        }
        if (f)
            fclose(f);
    }
    closedir(d);
    return found;
}

static int read_all(int fd, uint8_t *buf, size_t n, off_t at)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, buf + done, n - done, at + (off_t)done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t n, off_t at)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(fd, buf + done, n - done, at + (off_t)done);
        if (w <= 0)
            return -1;
        done += (size_t)w;
    }
    return 0;
}

static const int BOOT_AT[2] = {0, 1}, TASKS_AT[2] = {16, 32};      /* copies A, B (sectors) */

static int newer(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    return d != 0 && d < 0x80000000u;
}

/* Both copies of an open partition: which are good, and the newer one. */
static int pick(int fd, struct kc_config *best, int *which, int good[2], char *err, size_t n)
{
    static uint8_t boot[KC_BOOT], tasks[KC_TASKS];
    struct kc_config c;
    char why[2][120] = {"", ""};
    *which = -1;
    for (int k = 0; k < 2; k++) {
        good[k] = 0;
        if (read_all(fd, boot, KC_BOOT, (off_t)BOOT_AT[k] * KC_SECTOR) ||
            read_all(fd, tasks, KC_TASKS, (off_t)TASKS_AT[k] * KC_SECTOR)) {
            snprintf(why[k], sizeof why[k], "can't read it");
            continue;
        }
        if (kc_decode(boot, tasks, &c, why[k], sizeof why[k]))
            continue;
        good[k] = 1;
        if (*which < 0 || newer(c.generation, best->generation)) {
            *best = c;
            *which = k;
        }
    }
    if (*which < 0)
        snprintf(err, n, "nothing saved yet (copy A: %s; copy B: %s)", why[0], why[1]);
    return *which < 0 ? -1 : 0;
}

int kc_read_partition(const char *path, struct kc_config *c, char *which, char *err, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), good[2], w;
    if (fd < 0)
        return snprintf(err, n, "can't open %s (%s)", path, strerror(errno)), -1;
    int rc = pick(fd, c, &w, good, err, n);
    close(fd);
    if (!rc && which)
        *which = w ? 'B' : 'A';
    return rc;
}

/* Save: the copy not in use gets generation + 1, its task list first and
   then its boot block, each flushed to the drive, then both read back. A
   power cut in between leaves the copy in use as it was. */
int kc_write_partition(const char *path, const struct kc_config *c, char *which, char *err, size_t n)
{
    static uint8_t boot[KC_BOOT], tasks[KC_TASKS], back[KC_TASKS];
    struct kc_config cur;
    int fd = open(path, O_RDWR | O_CLOEXEC), good[2], w;
    char ignore[200];
    if (fd < 0)
        return snprintf(err, n, "can't open %s (%s)", path, strerror(errno)), -1;
    uint32_t gen = 1;
    int target = 0;
    if (!pick(fd, &cur, &w, good, ignore, sizeof ignore)) {
        gen = cur.generation + 1;
        target = !w;
    }
    kc_encode(c, gen, boot, tasks);
    if (write_all(fd, tasks, KC_TASKS, (off_t)TASKS_AT[target] * KC_SECTOR) || fsync(fd) ||
        write_all(fd, boot, KC_BOOT, (off_t)BOOT_AT[target] * KC_SECTOR) || fsync(fd)) {
        snprintf(err, n, "writing %s failed (%s)", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (read_all(fd, back, KC_TASKS, (off_t)TASKS_AT[target] * KC_SECTOR) || memcmp(back, tasks, KC_TASKS) ||
        read_all(fd, back, KC_BOOT, (off_t)BOOT_AT[target] * KC_SECTOR) || memcmp(back, boot, KC_BOOT)) {
        snprintf(err, n, "what was written to %s doesn't read back the same", path);
        close(fd);
        return -1;
    }
    close(fd);
    if (which)
        *which = target ? 'B' : 'A';
    return 0;
}
