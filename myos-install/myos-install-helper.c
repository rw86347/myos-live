/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-install-helper — the only process that may write a disk.
 *
 * The GTK / --text installer writes a plan (docs/INSTALLER-PLAN.md) and
 * runs this, setuid root, like myos-adduser. We trust nothing from the
 * window: the plan is parsed and checked again, the disk is looked up
 * again, and only an administrator (group wheel) may run it at all.
 *
 * Progress lines on stdout (the window turns these into the bar):
 *   step <name>
 *   progress <0-100>
 *   file <what is happening>
 *   ok
 *   error: <sentence>
 * Everything else (what sfdisk, mke2fs and rsync print) goes to the log,
 * /tmp/myos-install.log.
 *
 *   myos-install-helper --check PLAN
 *   myos-install-helper --dry-run PLAN      what --apply would do
 *   myos-install-helper --apply PLAN
 *
 * What --apply does on a UEFI computer:
 *   partition    sfdisk. Erase: a new GPT with "EFI System", myos-root and
 *                myos-data. Free space: the same three added after what is
 *                there (--append), only on a disk with no EFI partition yet.
 *                Then BLKRRPART, so the kernel's partitions are the new ones.
 *   filesystems  mke2fs on myos-root and myos-data, found by their GPT names
 *                (never by guessing numbers).
 *   boot         myos-mkesp: FAT32 on the new EFI partition, holding a copy
 *                of the stick's own (EFI/BOOT/BOOTX64.EFI finds the kernel,
 *                the firmware finds BOOTX64.EFI).
 * On a BIOS computer (the old PCs) the disk gets what `make hd` and the
 * 32-bit live stick have (bios_partition, bios_boot): an MBR whose boot
 * code loads stage 2 from sector 1, partition 1 the boot area (stage 2,
 * the kernel, its initramfs: the stick's, copied sector for sector), then
 * myos-root, myos-config and myos-data. The whole disk only: MyOS can't
 * start another system yet, so it can't share a BIOS disk.
 *   copy         myos-root at /mnt/myos-root (and myos-data at its /data),
 *                then rsync of the running system.
 *   configure    the account, computer name, language, keyboard, clock; the
 *                live user, the stick's ssh keys and the installer removed.
 *   finish       everything written to the disk.
 * Its mounts come off again at the end (unmount_all), so the new system
 * starts with its file systems marked cleanly unmounted, and after a failed
 * try the next one needs no restart.
 */
#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "disks.h"
#include "plan.h"

#ifndef BLKRRPART
#define BLKRRPART   0x125F              /* _IO(0x12, 95): read the partition table again */
#endif

#define NEWROOT     "/mnt/myos-root"
#define LIVE_ESP    "/mnt/myos-live-esp"
#define MKESP       "/usr/libexec/myos-mkesp"
#define ESP_NAME    "EFI System"
#define GPT_ESP     "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define GPT_LINUX   "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
/* The kernel config (docs/KERNEL-CONFIG.md §8): MyOS's own type, 1 MiB,
   right after myos-root. Settings › Kernel writes it; the kernel reads it
   at boot from the root's disk. */
#define GPT_MYOS_CONFIG "9390AFB2-C545-487E-AF50-3648E56B736C"
#define CONFIG_MIB  1
#define CONFIG_USED (42 * 512)          /* both copies: sectors 0-41 */
#define GB          1000000000ULL       /* the window's sizes are in GB, as disks are sold */
#define MIB         1048576ULL

static int logfd = -1;
static int apply;

/* What this run will make, decided once from the disk as it is now. */
static struct layout {
    unsigned long long root_mib;
    int data;                           /* a myos-data partition with the rest */
    int erase;                          /* a new table, or --append to the old one */
    unsigned long long before[DISK_PARTS];  /* start sectors that were already there */
    int nbefore;
    char esp[64], root[64], datadev[64], config[64];   /* found after sfdisk, by GPT name */
    /* BIOS: the disk's size, and the stick's boot code and boot area. */
    int bios;
    unsigned long long sectors;
    char livedisk[64];
    unsigned char bootcode[446];
    unsigned long long boot_sectors;    /* the stick's: its MBR and boot area */
} L;

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (logfd >= 0) {
        va_start(ap, fmt);
        vdprintf(logfd, fmt, ap);
        va_end(ap);
        dprintf(logfd, "\n");
    }
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* A step begins: the window's list, and the time so far in the log. */
static void step(const char *name)
{
    static struct timespec t0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!t0.tv_sec)
        t0 = now;
    say("step %s", name);
    logline("[%lds] %s", (long)(now.tv_sec - t0.tv_sec), name);
}

static int fail(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say("error: %s", buf);
    logline("error: %s", buf);
    return -1;
}

/* The log is in /tmp, where anyone may leave a file or a link under its
   name, and we are root: an old one is removed and a new one made, never
   followed or written through (O_EXCL). */
static void open_log(void)
{
    unlink(PLAN_LOG_DEFAULT);
    logfd = open(PLAN_LOG_DEFAULT, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
}

/* Run a program, its output to the log; with `input`, that is its stdin.
   0 if it exited with 0. Children get the clean environment main() made. */
static int run_in(char *const argv[], const char *input)
{
    char line[512];
    size_t at = 0;
    int in[2] = { -1, -1 }, st;
    pid_t pid;

    for (int i = 0; argv[i] && at < sizeof line - 2; i++)
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? " " : "", argv[i]);
    logline("+ %s", line);
    if (!apply)
        return 0;
    if (input && pipe2(in, O_CLOEXEC))
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(input ? in[0] : null, 0);
        dup2(logfd >= 0 ? logfd : null, 1);
        dup2(logfd >= 0 ? logfd : null, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (input) {
        close(in[0]);
        size_t n = strlen(input), done = 0;
        while (done < n) {
            ssize_t w = write(in[1], input + done, n - done);
            if (w <= 0)
                break;
            done += (size_t)w;
        }
        close(in[1]);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run(char *const argv[])
{
    return run_in(argv, NULL);
}

/* rsync --info=progress2: its "NN%" becomes our bar, from lo to hi.
   Lines end in \r while it counts, so both \r and \n end one. */
static int run_rsync(char *const argv[], int lo, int hi)
{
    int out[2], st, shown = -1;
    char buf[4096], line[256];
    size_t len = 0;
    pid_t pid;

    logline("+ rsync (the running system to %s)", NEWROOT);
    if (!apply)
        return 0;
    if (pipe2(out, O_CLOEXEC))
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(out[1], 1);
        dup2(logfd >= 0 ? logfd : null, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(out[1]);
    for (;;) {
        ssize_t n = read(out[0], buf, sizeof buf);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\r' && buf[i] != '\n') {
                if (len < sizeof line - 1)
                    line[len++] = buf[i];
                continue;
            }
            line[len] = 0;
            len = 0;
            char *pct = strchr(line, '%');
            if (!pct)
                continue;
            char *num = pct;
            while (num > line && isdigit((unsigned char)num[-1]))
                num--;
            int v = num < pct ? atoi(num) : -1;
            if (v >= 0 && v <= 100 && v != shown) {
                shown = v;
                say("progress %d", lo + (hi - lo) * v / 100);
                if (v % 10 == 0)
                    logline("rsync: %s", line);
            }
        }
    }
    close(out[0]);
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int is_block(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

/* Where `dev` is mounted, from the mount table; 0 if it is. */
static int mounted_at(const char *dev, char *dir, size_t n)
{
    FILE *f = fopen("/proc/self/mounts", "r");
    char line[512], src[128], at[256];
    int found = -1;

    if (!f)
        return -1;
    while (found && fgets(line, sizeof line, f))
        if (sscanf(line, "%127s %255s", src, at) == 2 && !strcmp(src, dev)) {
            snprintf(dir, n, "%s", at);
            found = 0;
        }
    fclose(f);
    return found;
}

/* The caller's own plan only: a regular file (not a link) that they or
   root own, mode 0600 if it holds a password. It is read from the file we
   checked, not opened again by name. */
static int open_plan(const char *path, uid_t caller, struct install_plan *p, int need_secret)
{
    struct stat st;
    char err[256];
    FILE *f;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return fail("Can't open the plan (%s).", strerror(errno));
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return fail("The plan is not a regular file.");
    }
    if (st.st_uid != caller && st.st_uid != 0) {
        close(fd);
        return fail("The plan belongs to someone else.");
    }
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return fail("Can't read the plan.");
    }
    int bad = plan_read_file(f, p);
    fclose(f);
    if (bad)
        return fail("Can't parse the plan.");
    if (need_secret && p->password[0] && (st.st_mode & 077) != 0) {
        explicit_bzero(p->password, sizeof p->password);
        return fail("A plan that holds a password must be mode 0600.");
    }
    if (plan_check(p, err, sizeof err)) {
        explicit_bzero(p->password, sizeof p->password);
        return fail("%s", err);
    }
    return 0;
}

/* Root, or a member of wheel (the administrators, as doas has them). */
static int may_install(uid_t caller)
{
    struct group *g = getgrnam("wheel");
    gid_t groups[64];
    int n;

    if (caller == 0)
        return 1;
    if (!g)
        return 0;
    if (getgid() == g->gr_gid)
        return 1;
    n = getgroups(64, groups);
    for (int i = 0; i < n; i++)
        if (groups[i] == g->gr_gid)
            return 1;
    return 0;
}

/* ---- BIOS computers (the old PCs): an MBR disk ----

   The BIOS runs the code in the disk's first sector (the MBR), which loads
   stage 2 from the sectors right after it; stage 2 loads the kernel and its
   small initramfs from the sectors after those. That boot area is MBR
   partition 1 (type 0x7F, active) from sector 1 on, as tools/mbr-partition.sh
   writes it for `make hd` and the 32-bit live stick, so the stick's own is
   copied sector for sector. MBR partitions have no names: the kernel finds
   myos-root and myos-data by their ext2 labels, myos-config by its type,
   0xDA (docs/KERNEL-CONFIG.md §8). */
#define BOOT_SECTORS ((unsigned long long)BOOT_AREA_MIB * 2048)

static unsigned long long le32(const unsigned char *b)
{
    return b[0] | (unsigned)b[1] << 8 | (unsigned)b[2] << 16 | (unsigned long long)b[3] << 24;
}

/* The stick MyOS started from: its MBR boot code and how long its boot
   area is. */
static int live_boot_area(void)
{
    struct disk_info disks[DISKS_MAX];
    int nd = disks_scan(disks, DISKS_MAX);
    unsigned char mbr[512];

    for (int i = 0; i < nd && !L.livedisk[0]; i++)
        if (disks[i].running)
            snprintf(L.livedisk, sizeof L.livedisk, "%s", disks[i].path);
    if (!L.livedisk[0])
        return fail("Can't find the disk MyOS started from.");
    int fd = open(L.livedisk, O_RDONLY | O_CLOEXEC);
    int got = fd >= 0 && pread(fd, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr;
    if (fd >= 0)
        close(fd);
    if (!got || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return fail("Can't read the first sector of %s.", L.livedisk);
    for (int k = 0; k < 4; k++) {
        const unsigned char *e = mbr + 446 + 16 * k;
        if (e[4] == 0x7F && le32(e + 8) == 1 && le32(e + 12)) {
            memcpy(L.bootcode, mbr, sizeof L.bootcode);
            L.boot_sectors = 1 + le32(e + 12);
            return 0;
        }
    }
    return fail("%s has no MyOS boot area (an MBR partition of type 7f at sector 1): "
                "start the computer from a MyOS stick made for BIOS.", L.livedisk);
}

/* Decide the layout from the disk as it is now, before anything is
   written; refuse what can't work, with a sentence saying why. */
static int check_layout(const struct install_plan *p)
{
    struct disk_info disks[DISKS_MAX];
    int n = disks_scan(disks, DISKS_MAX);
    const struct disk_info *d = disks_find(disks, n, p->disk);
    unsigned long long esp = (unsigned long long)ESP_MIB * MIB, pool, root, min;

    if (!d || !is_block(p->disk))
        return fail("The disk %s isn't there (any more). Nothing was written.", p->disk);
    if (d->in_use)
        return fail("%s", d->running ? "Refusing to install onto the disk MyOS is running from."
                                     : "That disk is still in use from an earlier try. Restart the "
                                       "computer, then install again. Nothing was written.");
    memset(&L, 0, sizeof L);
    L.erase = p->mode == MODE_ERASE;
    L.bios = p->firmware == FW_BIOS;
    L.sectors = d->bytes / 512;
    if (L.bios) {
        if (!L.erase && d->npart)
            return fail("On a computer that starts with BIOS, MyOS needs a whole disk: it can't "
                        "start another system yet. Choose Erase, or another disk. Nothing was written.");
        L.erase = 1;                    /* (an empty disk: nothing to keep) */
        if (L.sectors > 0xFFFFFFFFULL)
            return fail("The disk is bigger than 2 TB, more than an MBR can hold. Nothing was written.");
        if (live_boot_area())
            return -1;
        if (L.boot_sectors > BOOT_SECTORS)
            return fail("The stick's boot area (%llu sectors) is bigger than the %d MiB kept for it.",
                        L.boot_sectors, BOOT_AREA_MIB);
    }
    if (!L.erase) {
        const char *why = disks_free_problem(d, (unsigned long long)MIN_INSTALL_GB * GB);
        if (why)
            return fail("%s Nothing was written.", why);
        for (int i = 0; i < d->npart; i++)
            L.before[L.nbefore++] = d->parts[i].start;
    }
    /* Erase: all but the two GPT copies (34 sectors each end) and the 1 MiB
       alignment in front. Free space: what is left, all of it usable only
       if it is in one piece (sfdisk says so if it isn't). */
    pool = L.erase ? d->bytes - 2 * MIB : d->free_bytes;
    if (L.bios)
        esp = (unsigned long long)BOOT_AREA_MIB * MIB;  /* the boot area, where UEFI has its ESP */
    min = esp + (unsigned long long)MIN_INSTALL_GB * GB;
    if (pool < min)
        return fail("The disk is too small: MyOS needs %d GB. Nothing was written.",
                    MIN_INSTALL_GB + ESP_MIB / 1000 + 1);
    root = (unsigned long long)p->root_gb * GB;
    if (root > pool - esp - CONFIG_MIB * MIB)
        root = pool - esp - CONFIG_MIB * MIB;
    L.root_mib = root / MIB;
    /* myos-data gets the rest, if that is worth a partition (1 GB). */
    L.data = p->separate_home && pool - esp - (L.root_mib + CONFIG_MIB) * MIB >= GB;
    if (p->separate_home && !L.data)
        logline("no room for myos-data: /home stays on myos-root");
    logline("layout on %s (%s): %s; %s %d MiB, myos-root %llu MiB, myos-config %d MiB%s",
            p->disk, d->name, L.bios ? "a new MBR" : L.erase ? "new partition table" : "added in the free space",
            L.bios ? "boot area" : "EFI", L.bios ? BOOT_AREA_MIB : ESP_MIB, L.root_mib, CONFIG_MIB,
            L.data ? ", myos-data the rest" : "");
    return 0;
}

/* After sfdisk: our three, by GPT name, among the partitions that weren't
   there before. */
static int find_new_parts(const struct install_plan *p)
{
    struct disk_info disks[DISKS_MAX];
    int n = disks_scan(disks, DISKS_MAX);
    const struct disk_info *d = disks_find(disks, n, p->disk);

    if (!d)
        return fail("The disk went away after the partition table was written.");
    for (int i = 0; i < d->npart; i++) {
        const struct disk_part *q = &d->parts[i];
        int old = 0;
        for (int k = 0; k < L.nbefore; k++)
            old |= L.before[k] == q->start;
        if (old)
            continue;
        char *slot = !strcmp(q->label, ESP_NAME) ? L.esp
                   : !strcmp(q->label, "myos-root") ? L.root
                   : !strcmp(q->label, "myos-config") ? L.config
                   : !strcmp(q->label, "myos-data") ? L.datadev : NULL;
        if (slot && !slot[0])
            snprintf(slot, sizeof L.esp, "/dev/%s", q->name);
    }
    if (!L.esp[0] || !L.root[0] || !L.config[0] || (L.data && !L.datadev[0]))
        return fail("The new partitions didn't appear on %s (the kernel didn't read the new table).",
                    p->disk);
    if (!is_block(L.esp) || !is_block(L.root) || !is_block(L.config) ||
        (L.data && !is_block(L.datadev)))
        return fail("The new partitions have no /dev files.");
    logline("new: EFI %s, myos-root %s, myos-config %s%s%s", L.esp, L.root, L.config,
            L.data ? ", myos-data " : "", L.data ? L.datadev : "");
    return 0;
}

/* The config partition starts empty ("nothing saved": the kernel keeps its
   built-in config). Whatever the disk held there before is cleared, so a
   reinstall can't pick up an old system's settings by accident. */
static int clear_config(void)
{
    static const char zero[CONFIG_USED];
    int fd = open(L.config, O_WRONLY | O_CLOEXEC);
    int ok = fd >= 0 && write(fd, zero, sizeof zero) == (ssize_t)sizeof zero && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    return ok ? 0 : fail("Can't clear the new myos-config partition %s (%s).", L.config,
                         strerror(errno));
}

static void mbr_entry(unsigned char *mbr, int k, int active, int type,
                      unsigned long long start, unsigned long long count)
{
    unsigned char *e = mbr + 446 + 16 * k;
    e[0] = active ? 0x80 : 0;
    /* CHS: "use the LBA fields" (0xFE 0xFF 0xFF), but the boot area's
       start, sector 1, which old BIOSes may read as cylinder 0, head 0,
       sector 2 (as tools/mbr-partition.sh writes it). */
    e[1] = start == 1 ? 0 : 0xFE, e[2] = start == 1 ? 2 : 0xFF, e[3] = start == 1 ? 0 : 0xFF;
    e[4] = (unsigned char)type;
    e[5] = 0xFE, e[6] = 0xFF, e[7] = 0xFF;
    for (int i = 0; i < 4; i++) {
        e[8 + i] = (unsigned char)(start >> (8 * i));
        e[12 + i] = (unsigned char)(count >> (8 * i));
    }
}

/* After the MBR is written: its partitions, by where they start. */
static int bios_find_parts(const struct install_plan *p, unsigned long long root,
                           unsigned long long config, unsigned long long data)
{
    struct disk_info disks[DISKS_MAX];
    int n = disks_scan(disks, DISKS_MAX);
    const struct disk_info *d = disks_find(disks, n, p->disk);

    for (int i = 0; d && i < d->npart; i++) {
        const struct disk_part *q = &d->parts[i];
        char *slot = q->start == root ? L.root : q->start == config ? L.config
                   : L.data && q->start == data ? L.datadev : NULL;
        if (slot)
            snprintf(slot, sizeof L.root, "/dev/%s", q->name);
    }
    if (!L.root[0] || !L.config[0] || (L.data && !L.datadev[0]))
        return fail("The new partitions didn't appear on %s (the kernel didn't read the new table).",
                    p->disk);
    if (!is_block(L.root) || !is_block(L.config) || (L.data && !is_block(L.datadev)))
        return fail("The new partitions have no /dev files.");
    logline("new: myos-root %s, myos-config %s%s%s", L.root, L.config,
            L.data ? ", myos-data " : "", L.data ? L.datadev : "");
    return 0;
}

/* A new MBR: the boot area, myos-root, myos-config, myos-data. No boot
   code yet (bios_boot): until the copy is done the disk doesn't start. */
static int bios_partition(const struct install_plan *p)
{
    static const unsigned char zero[33 * 512];
    unsigned char mbr[512];
    unsigned long long root = BOOT_SECTORS, config = root + L.root_mib * 2048,
                       data = config + CONFIG_MIB * 2048;
    int fd, ok, rc;

    memset(mbr, 0, sizeof mbr);
    mbr_entry(mbr, 0, 1, 0x7F, 1, BOOT_SECTORS - 1);
    mbr_entry(mbr, 1, 0, 0x83, root, L.root_mib * 2048);
    mbr_entry(mbr, 2, 0, 0xDA, config, CONFIG_MIB * 2048);
    if (L.data)
        mbr_entry(mbr, 3, 0, 0x83, data, L.sectors - data);
    mbr[510] = 0x55, mbr[511] = 0xAA;
    logline("MBR for %s: boot area sectors 1-%llu (type 7f, active), myos-root at %llu, "
            "myos-config at %llu (type da)%s", p->disk, BOOT_SECTORS - 1, root, config,
            L.data ? ", myos-data after it" : "");
    if (!apply)
        return 0;
    /* A GPT the disk had keeps a second copy in its last 33 sectors, and
       other systems would still believe it: cleared. (Its first copy, from
       sector 1, is under the boot area, written over in bios_boot.) */
    fd = open(p->disk, O_RDWR | O_CLOEXEC);
    ok = fd >= 0 && pwrite(fd, zero, sizeof zero, 512) == (ssize_t)sizeof zero &&
         pwrite(fd, zero, sizeof zero, (L.sectors - 33) * 512) == (ssize_t)sizeof zero &&
         pwrite(fd, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr && fsync(fd) == 0;
    rc = ok ? ioctl(fd, BLKRRPART) : -1;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return fail("Can't write the partition table on %s (%s).", p->disk, strerror(errno));
    if (rc)
        return fail("The kernel couldn't read the new partition table (%s). Restart the computer "
                    "and install again.", strerror(errno));
    return bios_find_parts(p, root, config, data) || clear_config();
}

/* The stick's boot code into the new MBR (its partition table stays), and
   the stick's boot area, sector for sector, from sector 1. */
static int bios_boot(const struct install_plan *p)
{
    unsigned char mbr[512], buf[65536];
    unsigned long long at = 512, end = L.boot_sectors * 512;
    int in, out, ok;

    step("boot");
    say("progress 14");
    say("file writing MyOS's start code and boot area");
    logline("boot: %s's MBR code and its boot area (%llu sectors) onto %s", L.livedisk,
            L.boot_sectors - 1, p->disk);
    if (!apply)
        return 0;
    in = open(L.livedisk, O_RDONLY | O_CLOEXEC);
    out = open(p->disk, O_RDWR | O_CLOEXEC);
    ok = in >= 0 && out >= 0 && pread(out, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr;
    while (ok && at < end) {
        size_t n = end - at < sizeof buf ? (size_t)(end - at) : sizeof buf;
        ok = pread(in, buf, n, (off_t)at) == (ssize_t)n && pwrite(out, buf, n, (off_t)at) == (ssize_t)n;
        at += n;
    }
    if (ok) {
        memcpy(mbr, L.bootcode, sizeof L.bootcode);
        ok = pwrite(out, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr && fsync(out) == 0;
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (!ok)
        return fail("Writing the boot area to %s failed (%s).", p->disk, strerror(errno));
    say("progress 20");
    return 0;
}

static int step_partition(const struct install_plan *p)
{
    char script[768];
    int fd, rc;

    step("partition");
    say("progress 3");
    say("file writing the partition table");
    if (L.bios)
        return bios_partition(p);
    snprintf(script, sizeof script,
             "%s"
             "size=%dMiB, type=" GPT_ESP ", name=\"" ESP_NAME "\"\n"
             "size=%lluMiB, type=" GPT_LINUX ", name=myos-root\n"
             "size=%dMiB, type=" GPT_MYOS_CONFIG ", name=myos-config\n"
             "%s",
             L.erase ? "label: gpt\n" : "",
             ESP_MIB, L.root_mib, CONFIG_MIB,
             L.data ? "type=" GPT_LINUX ", name=myos-data\n" : "");
    logline("sfdisk script for %s:\n%s", p->disk, script);
    {
        char *erase[] = { "sfdisk", "--no-reread", "--wipe", "always", (char *)p->disk, NULL };
        char *append[] = { "sfdisk", "--no-reread", "--append", (char *)p->disk, NULL };
        if (run_in(L.erase ? erase : append, script))
            return fail("sfdisk could not write the partition table on %s.", p->disk);
    }
    if (!apply)
        return 0;
    /* The kernel reads the table again: the new partitions get their
       /dev files. It must work; a guess at their names is how the old
       helper formatted the wrong ones. */
    fd = open(p->disk, O_RDONLY | O_CLOEXEC);
    rc = fd < 0 ? -1 : ioctl(fd, BLKRRPART);
    if (fd >= 0)
        close(fd);
    if (rc)
        return fail("The kernel couldn't read the new partition table (%s). Restart the computer "
                    "and install again.", strerror(errno));
    return find_new_parts(p) || clear_config();
}

static int mke2fs(const char *dev, const char *label, const char *ratio)
{
    /* The features MyOS's ext2 reads and writes (fs/ext2.c), as the live
       disk's own myos-root has; 4 KiB blocks. -i: one inode per `ratio`
       bytes; fewer inodes than mke2fs's default make it much faster on a
       big disk (each one is zeroed). */
    char *a[] = { "mke2fs", "-q", "-F", "-t", "ext2", "-O", "none,filetype,sparse_super,large_file",
                  "-b", "4096", "-i", (char *)ratio, "-L", (char *)label, (char *)dev, NULL };
    return run(a);
}

static int step_filesystems(const struct install_plan *p)
{
    (void)p;
    step("filesystems");
    say("progress 6");
    say("file making ext2 on myos-root");
    if (mke2fs(apply ? L.root : "<myos-root>", "myos-root", "65536"))
        return fail("mke2fs failed on %s.", L.root);
    if (L.data) {
        say("progress 10");
        say("file making ext2 on myos-data");
        if (mke2fs(apply ? L.datadev : "<myos-data>", "myos-data", "262144"))
            return fail("mke2fs failed on %s.", L.datadev);
    }
    return 0;
}

/* The stick's EFI partition, mounted read-only (MyOS's FAT only reads):
   on the disk "/" is on, the partition the firmware started us from. */
static int live_esp(char *dir, size_t n)
{
    struct disk_info disks[DISKS_MAX];
    int nd = disks_scan(disks, DISKS_MAX);
    char dev[64] = "";

    for (int i = 0; i < nd && !dev[0]; i++) {
        if (!disks[i].running)
            continue;
        for (int k = 0; k < disks[i].npart && !dev[0]; k++)
            if (disks[i].parts[k].is_esp)
                snprintf(dev, sizeof dev, "/dev/%s", disks[i].parts[k].name);
    }
    if (!dev[0])
        return fail("Can't find the EFI partition MyOS started from.");
    if (mounted_at(dev, dir, n) == 0)
        return 0;
    snprintf(dir, n, "%s", LIVE_ESP);
    if (mkdir(LIVE_ESP, 0700) && errno != EEXIST)
        return fail("Can't make %s.", LIVE_ESP);
    if (mount(dev, LIVE_ESP, "vfat", MS_RDONLY, NULL))
        return fail("Can't read the stick's EFI partition %s (%s).", dev, strerror(errno));
    return 0;
}

static int step_boot(const struct install_plan *p)
{
    char src[256], probe[300];
    struct stat st;

    if (L.bios)
        return bios_boot(p);
    step("boot");
    say("progress 14");
    say("file copying MyOS's start files to the EFI partition");
    logline("FAT32 on the new EFI partition, with the stick's EFI/BOOT and EFI/MYOS "
            "(the firmware starts EFI/BOOT/BOOTX64.EFI by itself; no boot entry is written), "
            "but not INITRD.CPIO: the stick's fallback for disks MyOS can't drive, which "
            "the loader would read at every start, and this disk is one MyOS drives");
    if (!apply) {
        char *a[] = { "python3", "-I", MKESP, "<EFI partition>", "<the stick's>",
                      "--without", "EFI/MYOS/INITRD.CPIO", NULL };
        return run(a);
    }
    if (live_esp(src, sizeof src))
        return -1;
    snprintf(probe, sizeof probe, "%s/EFI/BOOT/BOOTX64.EFI", src);
    if (stat(probe, &st))
        return fail("The stick's EFI partition has no EFI/BOOT/BOOTX64.EFI.");
    {
        char *a[] = { "python3", "-I", MKESP, L.esp, src, "--without", "EFI/MYOS/INITRD.CPIO",
                      NULL };
        if (run(a))
            return fail("Making the EFI partition failed (see %s).", PLAN_LOG_DEFAULT);
    }
    say("progress 20");
    return 0;
}

static int step_copy(const struct install_plan *p)
{
    (void)p;
    step("copy");
    say("progress 20");
    say("file copying the system (several minutes)");
    if (apply) {
        if (mkdir(NEWROOT, 0755) && errno != EEXIST)
            return fail("Can't make %s.", NEWROOT);
        if (mount(L.root, NEWROOT, "ext2", 0, NULL))
            return fail("Can't mount the new myos-root %s (%s).", L.root, strerror(errno));
        if (L.data) {
            /* The kernel mounts myos-data at /data at every start;
               /home is a link into it (made in configure). */
            if (mkdir(NEWROOT "/data", 0755) && errno != EEXIST)
                return fail("Can't make /data on the new system.");
            if (mount(L.datadev, NEWROOT "/data", "ext2", 0, NULL))
                return fail("Can't mount the new myos-data %s (%s).", L.datadev, strerror(errno));
        }
    }
    /* Everything of the running system but what belongs to this one start
       of it: the kernel's own file systems, temporary files, mounts (the
       new disk itself is under /mnt), and the live user's home. -x stays on
       "/"'s own file system where the kernel tells them apart. */
    char *argv[] = {
        "rsync", "-ax", "--inplace", "--numeric-ids", "--no-inc-recursive", "--info=progress2",
        "--exclude=/proc/*", "--exclude=/sys/*", "--exclude=/dev/*", "--exclude=/run/*",
        "--exclude=/tmp/*", "--exclude=/var/tmp/*", "--exclude=/mnt/*", "--exclude=/media/*",
        "--exclude=/home/*", "--exclude=/data/*", "--exclude=/lost+found",
        "/", NEWROOT "/", NULL
    };
    if (run_rsync(argv, 20, 88))
        return fail("Copying the system failed (rsync; see %s).", PLAN_LOG_DEFAULT);
    say("progress 88");
    return 0;
}

static int write_text(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    size_t n, done = 0;

    if (fd < 0)
        return -1;
    n = strlen(text);
    while (done < n) {
        ssize_t w = write(fd, text + done, n - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    if (done != n || fchmod(fd, mode)) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/* A whole file, NUL-ended; NULL if it can't be read. */
static char *slurp(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char *buf;
    size_t done = 0;

    if (fd < 0 || fstat(fd, st) || st->st_size > 1 << 20 || !(buf = malloc((size_t)st->st_size + 1))) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    while (done < (size_t)st->st_size) {
        ssize_t r = read(fd, buf + done, (size_t)st->st_size - done);
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    close(fd);
    buf[done] = 0;
    return buf;
}

/* Replace path with text, keeping the old file's owner and mode: written
   whole to path+, then renamed over it (a reader sees old or new). */
static int replace(const char *path, const char *text, const struct stat *old)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s+", path);
    unlink(tmp);
    if (write_text(tmp, text, old->st_mode & 07777) || chown(tmp, old->st_uid, old->st_gid) ||
        rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* A field of a passwd/group line (0-based); "" if it has fewer. */
static void field(const char *line, int k, char *out, size_t n)
{
    while (k-- > 0 && (line = strchr(line, ':')))
        line++;
    if (!line) {
        out[0] = 0;
        return;
    }
    size_t len = strcspn(line, ":\n");
    if (len >= n)
        len = n - 1;
    memcpy(out, line, len);
    out[len] = 0;
}

/* People's accounts are uid 1000-59999 (Alpine's UID_MIN..); the rest
   belong to the system. The installed system keeps only the system's and
   the new one: not the live user "myos" (no password!), nor anyone added
   while trying MyOS out (their homes weren't copied). */
static int is_person(const char *passwd_line)
{
    char uid[16];
    field(passwd_line, 2, uid, sizeof uid);
    long u = strtol(uid, NULL, 10);
    return uid[0] && u >= 1000 && u < 60000;
}

#define MAX_GONE 32

static int gone(char names[][PLAN_USER], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return 1;
    return 0;
}

static int write_accounts(const struct install_plan *p, unsigned *uid_out)
{
    struct stat pst, gst, sst;
    char *passwd = slurp(NEWROOT "/etc/passwd", &pst), *group = slurp(NEWROOT "/etc/group", &gst),
         *shadow = slurp(NEWROOT "/etc/shadow", &sst);
    char names[MAX_GONE][PLAN_USER], name[PLAN_USER], salt[32], *hash;
    int ngone = 0, rc = -1;
    unsigned uid = 1000;
    static const char *const groups[] = { "wheel", "audio", "video", "input", "netdev", NULL };
    size_t cap;
    char *np = NULL, *ng = NULL, *ns = NULL;

    if (!passwd || !group || !shadow)
        goto out;
    cap = strlen(passwd) + strlen(group) + strlen(shadow) + 1024;
    np = calloc(1, cap), ng = calloc(1, cap), ns = calloc(1, cap);
    if (!np || !ng || !ns)
        goto out;

    /* passwd: the system's lines, then the new account. */
    for (char *l = passwd; *l; ) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
        field(l, 0, name, sizeof name);
        if (is_person(l) || !strcmp(name, p->user)) {
            if (ngone < MAX_GONE)
                snprintf(names[ngone++], PLAN_USER, "%s", name);
        } else
            strncat(np, l, len);
        l += len;
    }
    if (!gone(names, ngone, p->user) && ngone < MAX_GONE)
        snprintf(names[ngone++], PLAN_USER, "%s", p->user);
    snprintf(np + strlen(np), cap - strlen(np), "%s:x:%u:%u:%s:/home/%s:/bin/bash\n",
             p->user, uid, uid, p->fullname, p->user);

    /* group: the people's own groups go, their names leave the member
       lists, and the new account joins the administrator's groups. */
    for (char *l = group; *l; ) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
        char g[64], gid[16], members[512] = "", out[640];
        field(l, 0, g, sizeof g);
        field(l, 2, gid, sizeof gid);
        long id = strtol(gid, NULL, 10);
        if (gone(names, ngone, g) || (id >= 1000 && id < 60000)) {
            l += len;
            continue;
        }
        /* members, minus the removed ones */
        {
            char all[512], *save = NULL;
            field(l, 3, all, sizeof all);
            for (char *m = strtok_r(all, ",", &save); m; m = strtok_r(NULL, ",", &save))
                if (!gone(names, ngone, m))
                    snprintf(members + strlen(members), sizeof members - strlen(members), "%s%s",
                             members[0] ? "," : "", m);
        }
        for (int k = 0; groups[k]; k++)
            if (!strcmp(g, groups[k]))
                snprintf(members + strlen(members), sizeof members - strlen(members), "%s%s",
                         members[0] ? "," : "", p->user);
        char pw[64];
        field(l, 1, pw, sizeof pw);
        snprintf(out, sizeof out, "%s:%s:%s:%s\n", g, pw, gid, members);
        strncat(ng, out, cap - strlen(ng) - 1);
        l += len;
    }
    snprintf(ng + strlen(ng), cap - strlen(ng), "%s:x:%u:\n", p->user, uid);

    /* shadow: root stays locked ("*": no password opens it; doas is the
       way in), the people's lines go, the new one has sha512-crypt. */
    {
        static const char chars[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        unsigned char rnd[16];
        if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd)
            goto out;
        strcpy(salt, "$6$");
        for (int i = 0; i < 16; i++)
            salt[3 + i] = chars[rnd[i] % 64];
        salt[19] = 0;
        hash = crypt(p->password, salt);
        if (!hash || hash[0] != '$')
            goto out;
    }
    for (char *l = shadow; *l; ) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
        field(l, 0, name, sizeof name);
        if (!strcmp(name, "root")) {
            const char *rest = strchr(l, ':');
            rest = rest ? strchr(rest + 1, ':') : NULL;
            if (rest) {
                strcat(ns, "root:*");
                strncat(ns, rest, (size_t)(l + len - rest));
            }
        } else if (!gone(names, ngone, name))
            strncat(ns, l, len);
        l += len;
    }
    snprintf(ns + strlen(ns), cap - strlen(ns), "%s:%s:%ld:0:99999:7:::\n", p->user, hash,
             (long)(time(NULL) / 86400));

    /* passwd last: the account appears once its group and password exist. */
    if (replace(NEWROOT "/etc/shadow", ns, &sst) || replace(NEWROOT "/etc/group", ng, &gst) ||
        replace(NEWROOT "/etc/passwd", np, &pst))
        goto out;
    for (int i = 0; i < ngone; i++)
        if (strcmp(names[i], p->user))
            logline("account %s (from the live system) left out", names[i]);
    *uid_out = uid;
    rc = 0;
out:
    if (ns)
        explicit_bzero(ns, cap);
    if (shadow)
        explicit_bzero(shadow, strlen(shadow));
    free(passwd), free(group), free(shadow), free(np), free(ng), free(ns);
    return rc;
}

static int step_configure(const struct install_plan *p)
{
    char path[300], body[512], home[300];
    unsigned uid = 1000;

    step("configure");
    say("progress 89");
    say("file setting up your account and language");
    logline("account %s (%s), administrator; root locked; computer %s", p->user, p->fullname, p->hostname);
    logline("LANG=%s, keyboard %s, time zone %s, desktop effects %s", p->lang, p->keyboard,
            p->timezone, p->compositor);
    logline("removed from the new system: the live user, the stick's ssh host key and "
            "authorized keys, the installer; ssh login off");
    if (!apply)
        return 0;

    if (write_accounts(p, &uid))
        return fail("Can't write the account into /etc/passwd, group and shadow.");

    /* The home: /home/<user>, on myos-data when there is one. */
    if (L.data) {
        rmdir(NEWROOT "/home");         /* rsync made it, empty */
        if (mkdir(NEWROOT "/data/home", 0755) && errno != EEXIST)
            return fail("Can't make /data/home.");
        struct stat st;
        if ((symlink("/data/home", NEWROOT "/home") && errno != EEXIST) ||
            lstat(NEWROOT "/home", &st) || !S_ISLNK(st.st_mode))
            return fail("Can't link /home to /data/home.");
        snprintf(home, sizeof home, NEWROOT "/data/home/%s", p->user);
    } else
        snprintf(home, sizeof home, NEWROOT "/home/%s", p->user);
    if (mkdir(home, 0700) && errno != EEXIST)
        return fail("Can't make the home folder.");
    {
        char from[] = NEWROOT "/etc/skel/.", ids[32];
        char *cp[] = { "cp", "-a", from, home, NULL };
        snprintf(ids, sizeof ids, "%u:%u", uid, uid);
        char *ch[] = { "chown", "-R", ids, home, NULL };
        if (run(cp))
            logline("warning: /etc/skel not copied");
        if (run(ch) || chmod(home, 0700))
            return fail("Can't give the home folder to %s.", p->user);
    }

    snprintf(path, sizeof path, NEWROOT "/etc/hostname");
    snprintf(body, sizeof body, "%s\n", p->hostname);
    if (write_text(path, body, 0644))
        return fail("Can't write /etc/hostname.");

    mkdir(NEWROOT "/etc/myos", 0755);
    snprintf(body, sizeof body, "compositor=%s\n", p->compositor);
    write_text(NEWROOT "/etc/myos/desktop.conf", body, 0644);

    mkdir(NEWROOT "/etc/profile.d", 0755);
    snprintf(body, sizeof body, "export LANG=%s\nexport LC_ALL=%s\n", p->lang, p->lang);
    write_text(NEWROOT "/etc/profile.d/lang.sh", body, 0644);

    mkdir(NEWROOT "/etc/X11/xorg.conf.d", 0755);
    snprintf(body, sizeof body,
             "Section \"InputClass\"\n"
             "    Identifier \"MyOS keyboard\"\n"
             "    MatchIsKeyboard \"on\"\n"
             "    Option \"XkbLayout\" \"%s\"\n"
             "EndSection\n",
             p->keyboard);
    write_text(NEWROOT "/etc/X11/xorg.conf.d/00-keyboard.conf", body, 0644);

    mkdir(NEWROOT "/etc/doas.d", 0755);
    write_text(NEWROOT "/etc/doas.d/wheel.conf", "permit persist :wheel\n", 0644);

    snprintf(path, sizeof path, NEWROOT "/usr/share/zoneinfo/%s", p->timezone);
    if (access(path, R_OK) == 0) {
        char dest[] = NEWROOT "/etc/localtime";
        char *cp[] = { "cp", path, dest, NULL };
        unlink(dest);
        run(cp);
    }

    /* The live system's own things, which an installed one must not have:
       the stick's ssh host key (every install would share it, and anyone
       with a stick could pose as this computer), the build Mac's key for
       the live user, and ssh login itself (off, as on a new Mac: turn on
       with `doas ln -s /etc/sv/sshd /var/service/`). The installer goes:
       it is setuid root and has nothing left to do. */
    {
        char *rm[] = { "rm", "-rf",
                       NEWROOT "/etc/ssh/ssh_host_ed25519_key", NEWROOT "/etc/ssh/ssh_host_ed25519_key.pub",
                       NEWROOT "/etc/ssh/ssh_host_rsa_key", NEWROOT "/etc/ssh/ssh_host_rsa_key.pub",
                       NEWROOT "/etc/ssh/ssh_host_ecdsa_key", NEWROOT "/etc/ssh/ssh_host_ecdsa_key.pub",
                       NEWROOT "/etc/ssh/authorized_keys",
                       NEWROOT "/etc/runit/runsvdir/default/sshd",
                       NEWROOT "/usr/share/applications/myos-install.desktop",
                       NEWROOT "/usr/bin/myos-install",
                       NEWROOT "/usr/libexec/myos-install-helper",
                       NEWROOT MKESP,
                       NEWROOT "/root/.ash_history", NEWROOT "/root/.bash_history",
                       NULL };
        run(rm);
    }
    /* Wi-Fi: the networks this stick knows (their passwords) only if asked;
       root-only either way. */
    if (!(p->copy_wifi && p->wifi_ssid[0]))
        write_text(NEWROOT "/etc/wpa_supplicant/wpa_supplicant.conf",
                   "# wpa_supplicant's settings: how it is controlled, and the Wi-Fi networks\n"
                   "# this computer joins (added by the Wi-Fi icon in the taskbar).\n"
                   "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
                   "update_config=1\n",
                   0600);
    else
        chmod(NEWROOT "/etc/wpa_supplicant/wpa_supplicant.conf", 0600);
    say("progress 95");
    return 0;
}

/* Take our mounts off, innermost first. One that isn't mounted (an earlier
   step never got that far) is fine; one still in use is logged and stays. */
static void unmount_all(void)
{
    static const char *const at[] = { NEWROOT "/data", NEWROOT, LIVE_ESP, NULL };
    for (int k = 0; at[k]; k++) {
        if (umount2(at[k], 0) == 0)
            logline("unmounted %s", at[k]);
        else if (errno != EINVAL && errno != ENOENT)
            logline("can't unmount %s: %s", at[k], strerror(errno));
    }
}

static int step_finish(const struct install_plan *p)
{
    step("finish");
    say("progress 96");
    say("file writing everything to the disk");
    /* MyOS's sync also flushes each written drive's own cache (bsync);
       the unmount then marks the new ext2 file systems clean. */
    logline("sync, then unmount");
    (void)p;
    if (apply) {
        sync();
        unmount_all();
    }
    say("progress 100");
    return 0;
}

static int run_plan(const struct install_plan *p)
{
    if (check_layout(p))
        return 2;
    if (step_partition(p) || step_filesystems(p) || step_boot(p) || step_copy(p) ||
        step_configure(p) || step_finish(p)) {
        if (apply)
            unmount_all();              /* so another try needs no restart */
        return 3;
    }
    step("done");
    say("ok");
    logline("ok: MyOS is installed on %s", p->disk);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: myos-install-helper --check|--dry-run|--apply PLAN\n");
}

int main(int argc, char **argv)
{
    const char *mode, *path;
    struct install_plan p;
    uid_t caller = getuid();
    int rc;

    if (argc != 3) {
        usage();
        return 1;
    }
    mode = argv[1];
    path = argv[2];
    if (strcmp(mode, "--check") && strcmp(mode, "--dry-run") && strcmp(mode, "--apply")) {
        usage();
        return 1;
    }
    apply = !strcmp(mode, "--apply");

    /* We are setuid root. Nothing of the caller's reaches the programs we
       run: an LD_PRELOAD or PYTHONPATH would otherwise run their code as
       root (the kernel marks only *this* program "secure", not sfdisk). */
    if (clearenv() == 0) {
        setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("LC_ALL", "C", 1);
    }
    umask(022);
    if (!may_install(caller)) {
        fail("Only an administrator may install MyOS.");
        return 1;
    }
    if (setgid(0) || setuid(0)) {
        fail("This helper must be installed setuid root.");
        return 1;
    }
    open_log();

    if (open_plan(path, caller, &p, 1))
        return 2;
    if (!strcmp(mode, "--check")) {
        say("ok");
        explicit_bzero(p.password, sizeof p.password);
        return 0;
    }
    rc = run_plan(&p);
    explicit_bzero(p.password, sizeof p.password);
    if (logfd >= 0)
        close(logfd);
    return rc;
}
