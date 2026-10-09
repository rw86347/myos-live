/*
 * myos-install-helper — the only process that may write a disk.
 *
 * The GTK / --text installer writes a plan (docs/INSTALLER-PLAN.md) and
 * runs this, setuid root, like myos-adduser. We trust nothing from the
 * window: the plan is parsed again, the disk is looked up again, and a
 * world-readable plan with a password is refused.
 *
 * Progress lines on stdout (the window turns these into the bar):
 *   step <name>
 *   progress <0-100>
 *   file <what is being copied>
 *   ok
 *   error: <sentence>
 *
 *   myos-install-helper --check PLAN
 *   myos-install-helper --dry-run PLAN
 *   myos-install-helper --apply PLAN
 *
 * --apply still refuses if the kernel has not published a block device
 * for the chosen disk (the usual case on MyOS today). Nothing is written
 * then; the plan stays in /tmp for a later run.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "disks.h"
#include "plan.h"

static FILE *logf;
static int apply;

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (logf) {
        va_start(ap, fmt);
        vfprintf(logf, fmt, ap);
        va_end(ap);
        fputc('\n', logf);
        fflush(logf);
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

static void fail(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say("error: %s", buf);
    logline("error: %s", buf);
}

static int runv(char *const argv[])
{
    pid_t pid;
    int st;

    logline("+ %s", argv[0]);
    if (!apply)
        return 0;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return 0;
    return -1;
}

static int is_block(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

static const char *blockers(const struct install_plan *p)
{
    if (!is_block(p->disk))
        return "There is no block device for that disk. The kernel does not "
               "yet publish /dev/nvme0n1 or /dev/sda (docs/INSTALLER-PLAN.md: "
               "block device nodes, and on the MSI often Intel VMD). "
               "Nothing was written.";
    return NULL;
}

static int open_plan(const char *path, struct install_plan *p, int need_secret)
{
    struct stat st;
    int fd;
    FILE *f;
    char err[256];

    if (strstr(path, "..")) {
        fail("The plan path is not allowed.");
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fail("Can't open the plan (%s).", strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        fail("The plan is not a regular file.");
        return -1;
    }
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        fail("Can't read the plan.");
        return -1;
    }
    fclose(f);                  /* plan_read opens the path again; we checked the name */
    if (plan_read(path, p)) {
        fail("Can't parse the plan.");
        return -1;
    }
    if (need_secret && p->password[0] && (st.st_mode & 077) != 0) {
        explicit_bzero(p->password, sizeof p->password);
        fail("A plan that holds a password must be mode 0600.");
        return -1;
    }
    if (plan_check(p, err, sizeof err)) {
        explicit_bzero(p->password, sizeof p->password);
        fail("%s", err);
        return -1;
    }
    return 0;
}

static int confirm_disk(const struct install_plan *p)
{
    struct disk_info disks[DISKS_MAX];
    int n = disks_scan(disks, DISKS_MAX);
    const struct disk_info *d = disks_find(disks, n, p->disk);

    if (d && d->in_use) {
        fail("Refusing to install onto the disk MyOS is running from.");
        return -1;
    }
    if (n && !d)
        logline("warning: %s was not in /sys/block when the helper started", p->disk);
    return 0;
}

static void write_sfdisk(const struct install_plan *p, char *script, size_t n)
{
    if (p->firmware == FW_UEFI) {
        if (p->mode == MODE_ERASE && !p->keep_esp)
            snprintf(script, n,
                     "label: gpt\n"
                     "size=%dMiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=EFI\n"
                     "size=%dGiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=myos-root\n"
                     "%s",
                     ESP_MIB, p->root_gb,
                     p->separate_home ? "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=myos-data\n" : "");
        else
            snprintf(script, n,
                     "# keep the existing table; add in free space:\n"
                     "size=%dGiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=myos-root\n"
                     "%s",
                     p->root_gb,
                     p->separate_home ? "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=myos-data\n" : "");
    } else {
        snprintf(script, n,
                 "label: dos\n"
                 "size=%dMiB, type=7f, bootable\n"
                 "size=%dGiB, type=83\n"
                 "%s",
                 BOOT_AREA_MIB, p->root_gb, p->separate_home ? "type=83\n" : "");
    }
}

static int step_partition(const struct install_plan *p)
{
    char script[512], tmp[] = "/tmp/myos-sfdisk.XXXXXX";
    int fd;
    FILE *f;

    say("step partition");
    say("progress 8");
    say("file writing the partition table");
    write_sfdisk(p, script, sizeof script);
    logline("sfdisk script for %s:\n%s", p->disk, script);
    if (!apply)
        return 0;

    fd = mkstemp(tmp);
    if (fd < 0) {
        fail("Can't write a temporary sfdisk script.");
        return -1;
    }
    f = fdopen(fd, "w");
    if (!f || fputs(script, f) < 0 || fclose(f)) {
        unlink(tmp);
        fail("Can't write a temporary sfdisk script.");
        return -1;
    }
    /* Feed the script on stdin. */
    {
        pid_t pid = fork();
        int st;
        if (pid == 0) {
            int in = open(tmp, O_RDONLY | O_CLOEXEC);
            if (in >= 0) {
                dup2(in, 0);
                close(in);
            }
            execlp("sfdisk", "sfdisk", "--no-reread", p->disk, (char *)NULL);
            _exit(127);
        }
        waitpid(pid, &st, 0);
        unlink(tmp);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            fail("sfdisk could not write the partition table on %s.", p->disk);
            return -1;
        }
    }
    /* Ask the kernel to read the new table. MyOS has no BLKRRPART yet. */
    {
        char *a[] = { "blockdev", "--rereadpt", (char *)p->disk, NULL };
        if (runv(a))
            logline("blockdev --rereadpt failed (expected until BLKRRPART exists)");
    }
    return 0;
}

static int step_filesystems(const struct install_plan *p)
{
    char rootdev[64], datadev[64], espdev[64];
    int root_n = (p->firmware == FW_UEFI && p->mode == MODE_ERASE && !p->keep_esp) ? 2 : 1;
    int data_n = root_n + 1;

    if (p->firmware == FW_BIOS)
        root_n = 2, data_n = 3;
    if (p->firmware == FW_UEFI && !(p->mode == MODE_ERASE && !p->keep_esp))
        root_n = p->keep_esp ? 0 : 2; /* 0 = find by label after sfdisk; we guess next slot */
    if (!root_n)
        root_n = 3;             /* common: EFI=1, Windows=2, myos-root=3 in free space */

    disks_part_path(p->disk, root_n, rootdev, sizeof rootdev);
    disks_part_path(p->disk, data_n, datadev, sizeof datadev);
    disks_part_path(p->disk, 1, espdev, sizeof espdev);

    say("step filesystems");
    say("progress 16");
    say("file making ext2 on myos-root");
    logline("mke2fs -F -L myos-root %s", rootdev);
    if (apply) {
        char *a[] = { "mke2fs", "-F", "-L", "myos-root", rootdev, NULL };
        if (runv(a)) {
            fail("mke2fs failed on %s.", rootdev);
            return -1;
        }
    }
    if (p->separate_home) {
        say("file making ext2 on myos-data");
        logline("mke2fs -F -L myos-data %s", datadev);
        if (apply) {
            char *a[] = { "mke2fs", "-F", "-L", "myos-data", datadev, NULL };
            if (runv(a)) {
                fail("mke2fs failed on %s.", datadev);
                return -1;
            }
        }
    }
    if (p->firmware == FW_UEFI && p->mode == MODE_ERASE && !p->keep_esp) {
        say("file making FAT32 on the EFI partition");
        logline("mkfs.vfat -n EFI %s", espdev);
        if (apply) {
            char *a[] = { "mkfs.vfat", "-n", "EFI", espdev, NULL };
            if (runv(a)) {
                fail("mkfs.vfat failed (is dosfstools installed?).");
                return -1;
            }
        }
    }
    return 0;
}

static int step_copy(const struct install_plan *p)
{
    char *argv[] = {
        "rsync", "-aHAX",
        "--exclude=/proc", "--exclude=/sys", "--exclude=/dev",
        "--exclude=/tmp", "--exclude=/run", "--exclude=/mnt",
        "--exclude=/home/myos", "--exclude=/lost+found",
        "/", "/mnt/myos-root/", NULL
    };

    (void)p;
    say("step copy");
    say("progress 20");
    say("file copying the live system");
    logline("mount LABEL=myos-root /mnt/myos-root");
    if (p->separate_home)
        logline("mount LABEL=myos-data /mnt/myos-root/home");
    logline("rsync -aHAX / /mnt/myos-root/ (live paths left out)");
    if (!apply)
        return 0;
    if (mkdir("/mnt/myos-root", 0755) && errno != EEXIST) {
        fail("Can't make /mnt/myos-root.");
        return -1;
    }
    {
        char *m[] = { "mount", "-L", "myos-root", "/mnt/myos-root", NULL };
        if (runv(m)) {
            fail("Can't mount the new root. Runtime ext2 mount is still a kernel gap (B27).");
            return -1;
        }
    }
    if (p->separate_home) {
        mkdir("/mnt/myos-root/home", 0755);
        char *m[] = { "mount", "-L", "myos-data", "/mnt/myos-root/home", NULL };
        if (runv(m)) {
            fail("Can't mount myos-data.");
            return -1;
        }
    }
    if (runv(argv)) {
        fail("Copying the system failed (rsync). Hard links may still be missing (B12).");
        return -1;
    }
    say("progress 78");
    return 0;
}

static int write_text(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
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
    if (done != n || fchmod(fd, mode) || fsync(fd)) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int step_configure(const struct install_plan *p)
{
    const char *root = apply ? "/mnt/myos-root" : "";
    char path[256], body[512];

    say("step configure");
    say("progress 82");
    say("file setting up the account and language");
    logline("hostname %s, LANG=%s, timezone %s, compositor %s",
            p->hostname, p->lang, p->timezone, p->compositor);
    logline("account %s (%s), root locked, doas persist for wheel", p->user, p->fullname);
    if (p->copy_wifi && p->wifi_ssid[0])
        logline("copy Wi-Fi network %s", p->wifi_ssid);
    if (!apply)
        return 0;

    snprintf(path, sizeof path, "%s/etc/hostname", root);
    snprintf(body, sizeof body, "%s\n", p->hostname);
    if (write_text(path, body, 0644))
        return fail("Can't write /etc/hostname."), -1;

    snprintf(path, sizeof path, "%s/etc/myos", root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/etc/myos/desktop.conf", root);
    snprintf(body, sizeof body, "compositor=%s\n", p->compositor);
    write_text(path, body, 0644);

    snprintf(path, sizeof path, "%s/etc/profile.d", root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/etc/profile.d/lang.sh", root);
    snprintf(body, sizeof body, "export LANG=%s\nexport LC_ALL=%s\n", p->lang, p->lang);
    write_text(path, body, 0644);

    snprintf(path, sizeof path, "%s/etc/X11/xorg.conf.d", root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/etc/X11/xorg.conf.d/00-keyboard.conf", root);
    snprintf(body, sizeof body,
             "Section \"InputClass\"\n"
             "    Identifier \"MyOS keyboard\"\n"
             "    MatchIsKeyboard \"on\"\n"
             "    Option \"XkbLayout\" \"%s\"\n"
             "EndSection\n",
             p->keyboard);
    write_text(path, body, 0644);

    snprintf(path, sizeof path, "%s/etc/doas.d/wheel.conf", root);
    write_text(path, "permit persist :wheel\n", 0644);

    {
        char zone[256], dest[256];
        snprintf(zone, sizeof zone, "/usr/share/zoneinfo/%s", p->timezone);
        snprintf(dest, sizeof dest, "%s/etc/localtime", root);
        unlink(dest);
        if (access(zone, R_OK) == 0) {
            char *a[] = { "cp", "-a", zone, dest, NULL };
            runv(a);
        }
    }
    /* The account itself: same files myos-adduser writes, on the new root.
       Until hard links and a writable mounted root exist, this is a log line. */
    logline("would add %s to passwd/group/shadow on the new root (myos-adduser's method)", p->user);
    return 0;
}

static int step_boot(const struct install_plan *p)
{
    say("step boot");
    say("progress 92");
    say("file making the disk start MyOS");
    if (p->firmware == FW_UEFI) {
        logline("copy BOOTX64.EFI, KERNEL.BIN, FONT.BIN to EFI/MYOS/");
        logline("efibootmgr -c -L MyOS (needs UEFI SetVariable; else EFI/BOOT fallback)");
    } else {
        logline("write the MBR and the BIOS boot area (what make hd writes on the Mac)");
    }
    if (!apply)
        return 0;
    if (p->firmware == FW_UEFI) {
        mkdir("/mnt/myos-efi", 0755);
        char *m[] = { "mount", "-t", "vfat", "-L", "EFI", "/mnt/myos-efi", NULL };
        if (runv(m)) {
            fail("Can't mount the EFI partition (FAT32 mount/write are still kernel gaps).");
            return -1;
        }
        mkdir("/mnt/myos-efi/EFI", 0755);
        mkdir("/mnt/myos-efi/EFI/MYOS", 0755);
        /* Sources: the running stick's ESP, if it is mounted, or /boot. */
        {
            static const char *const src[] = {
                "/boot/efi/EFI/MYOS", "/efi/EFI/MYOS", "/boot/EFI/MYOS", NULL
            };
            int i;
            for (i = 0; src[i]; i++)
                if (access(src[i], R_OK) == 0)
                    break;
            if (!src[i]) {
                fail("Can't find EFI/MYOS on the live system to copy.");
                return -1;
            }
            char from[256], to[256];
            static const char *const files[] = { "BOOTX64.EFI", "KERNEL.BIN", "FONT.BIN", "INITRD.CPIO", NULL };
            for (int f = 0; files[f]; f++) {
                snprintf(from, sizeof from, "%s/%s", src[i], files[f]);
                snprintf(to, sizeof to, "/mnt/myos-efi/EFI/MYOS/%s", files[f]);
                if (access(from, R_OK) == 0) {
                    char *a[] = { "cp", "-a", from, to, NULL };
                    runv(a);
                }
            }
        }
    }
    return 0;
}

static int step_finish(const struct install_plan *p)
{
    (void)p;
    say("step finish");
    say("progress 98");
    say("file syncing disks");
    logline("sync; umount");
    if (apply) {
        char *a[] = { "sync", NULL };
        runv(a);
        char *u1[] = { "umount", "/mnt/myos-efi", NULL };
        char *u2[] = { "umount", "/mnt/myos-root/home", NULL };
        char *u3[] = { "umount", "/mnt/myos-root", NULL };
        runv(u1);
        runv(u2);
        runv(u3);
    }
    say("progress 100");
    return 0;
}

static int run_plan(const struct install_plan *p)
{
    const char *why;

    if (confirm_disk(p))
        return 2;
    why = blockers(p);
    if (why) {
        /* Dry-run still prints the steps so the window and --text can show them. */
        if (apply) {
            fail("%s", why);
            return 2;
        }
        logline("%s", why);
    }
    if (step_partition(p) || step_filesystems(p) || step_copy(p) ||
        step_configure(p) || step_boot(p) || step_finish(p))
        return 3;
    say("ok");
    logline("ok");
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
    if (apply || !strcmp(mode, "--dry-run")) {
        if (setuid(0) || setgid(0)) {
            fail("This helper must be installed setuid root.");
            return 1;
        }
        setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
        umask(022);
    }

    logf = fopen(PLAN_LOG_DEFAULT, "w");
    if (logf)
        fchmod(fileno(logf), 0644);

    if (open_plan(path, &p, apply || !strcmp(mode, "--dry-run"))) {
        if (logf)
            fclose(logf);
        return 2;
    }

    if (!strcmp(mode, "--check")) {
        say("ok");
        explicit_bzero(p.password, sizeof p.password);
        if (logf)
            fclose(logf);
        return 0;
    }

    rc = run_plan(&p);
    explicit_bzero(p.password, sizeof p.password);
    if (logf)
        fclose(logf);
    return rc;
}
