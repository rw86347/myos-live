/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-rm — rm for MyOS's users: files go to ~/Trash, not away.
 *
 * Two rm's live on the disk:
 *
 *   /usr/bin/myos-rm   this program. /bin/rm and /usr/bin/rm are symlinks
 *                      to it, so every way of running rm — by name, by
 *                      full path, from a script — arrives here.
 *   GNU rm             inside Alpine's /usr/bin/coreutils: the real unlink.
 *
 * Who gets which (decided by the effective uid, first thing):
 *
 *   root, and system accounts (uid < 1000, or nobody) → GNU rm, exactly:
 *     same flags, messages and exit codes. Root holds the dangerous gun on
 *     purpose; daemons have no Trash, and their scripts expect Linux.
 *   people (uid 1000-59999, Alpine's UID_MIN..UID_MAX) → this program.
 *     A file is renamed into ~/Trash, and a hidden sidecar next to it,
 *     ._NAME.where, records where it lived, its owner, mode and times, so
 *     `rm --putBack NAME` (or My Files) can put it back exactly.
 *
 * For people, three things are a real delete:
 *   - files in the scratch folders (/tmp, /var/tmp, /dev/shm, /run): every
 *     script cleans up there, they are emptied at boot anyway, and keeping
 *     them would fill the Trash with other programs' leftovers;
 *   - files already in ~/Trash (that is what the Trash is for);
 *   - rm --reallyDelete ..., asked for by name (My Files' "Delete
 *     Permanently"); it runs GNU rm.
 *
 * This is a seat belt, not a lock. It stops accidents; it does not stop a
 * person deleting their own files on purpose (any program can call
 * unlink()). Other people's files are protected by Unix permissions, as
 * always.
 *
 *   rm [-fiIrRdv] FILE...      move to ~/Trash (GNU rm's options)
 *   rm --reallyDelete ...      GNU rm: gone for good
 *   rm --putBack NAME          put NAME (a name in ~/Trash) back
 *   rm --emptyTrash            empty ~/Trash (a real delete)
 *
 * Installed by the Makefile (RM_BIN). apk re-creates /bin/rm when
 * coreutils is upgraded, so /etc/apk/commit_hooks.d/myos-rm.post-commit
 * points it back here after every apk run.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define GNU_RM      "/usr/bin/coreutils"    /* GNU's one binary; argv[0] picks rm */
#define BUSYBOX     "/bin/busybox"          /* if coreutils is missing */
#ifndef PEOPLE_MIN                          /* (-DPEOPLE_MIN=500 tests it on a Mac) */
#define PEOPLE_MIN  1000                    /* Alpine's /etc/login.defs UID_MIN */
#endif
#define PEOPLE_MAX  60000                   /* ... and UID_MAX (+1) */

static const char *const scratch[] = { "/tmp", "/var/tmp", "/dev/shm", "/run" };

/* The options, GNU rm's meanings. */
static int force, recursive, empty_dir, verbose;
static enum { ASK_NEVER, ASK_ONCE, ASK_ALWAYS } interactive;
static int status;                          /* the exit code: 1 if anything failed */
static char bin[PATH_MAX];                  /* ~/Trash, resolved */

/* "rm: cannot remove 'x': No such file or directory", as GNU says it. */
static void cannot(const char *arg, const char *why)
{
    fprintf(stderr, "rm: cannot remove '%s': %s\n", arg, why);
    status = 1;
}

static int is_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/')
        return 1;
    return !strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

/* path is a thing in the Trash itself (bin/NAME), not the Trash or deeper. */
static int trash_item(const char *path)
{
    size_t n = strlen(bin);
    return !strncmp(path, bin, n) && path[n] == '/' && !strchr(path + n + 1, '/');
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* --- GNU rm, for root, system accounts and --reallyDelete ---------------- */

/* Run GNU rm with our own options taken out. Never returns. The check
   against /proc/self/exe is the guard against the bug of 2026-10-07, when
   an image build overwrote coreutils with this program and root's rm
   called itself forever. */
static void exec_gnu(int argc, char **argv)
{
    char *nargv[argc + 1];
    int n = 0, i;
    nargv[n++] = "rm";
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--"))
            break;                      /* everything after is a file name */
        if (!strcmp(a, "--reallyDelete") || !strcmp(a, "--really-delete"))
            continue;
        nargv[n++] = argv[i];
    }
    for (; i < argc; i++)
        nargv[n++] = argv[i];
    nargv[n] = NULL;

    struct stat me, gnu;
    if (!stat("/proc/self/exe", &me) && !stat(GNU_RM, &gnu) &&
        me.st_dev == gnu.st_dev && me.st_ino == gnu.st_ino) {
        fputs("rm: " GNU_RM " is this program, not GNU coreutils: the disk image is "
              "broken (reinstall coreutils)\n", stderr);
        exit(1);
    }
    execv(GNU_RM, nargv);
    execv(BUSYBOX, nargv);              /* busybox picks its rm from argv[0] too */
    fprintf(stderr, "rm: cannot run " GNU_RM " or " BUSYBOX ": %s\n", strerror(errno));
    exit(1);
}

/* --- the Trash -------------------------------------------------------------- */

static int find_trash(void)
{
    const char *h = getenv("HOME");
    if (!h || !h[0]) {
        struct passwd *pw = getpwuid(geteuid());
        h = pw ? pw->pw_dir : NULL;
    }
    char home[PATH_MAX];
    if (!h || !realpath(h, home))
        return -1;
    snprintf(bin, sizeof bin, "%s/Trash", strcmp(home, "/") ? home : "");
    return 0;
}

/* The sidecar that says where a trashed file came from, and the picture
   thumbnail sidecar (._NAME) that My Files keeps next to a picture. */
static void where_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/._%s.where", bin, name);
}

static void thumb_of(const char *dir, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/._%s", dir, name);
}

static int write_where(const char *name, const char *from, const struct stat *st)
{
    char path[PATH_MAX], when[32];
    where_of(name, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    time_t now = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    fprintf(f, "MyOS-Trash 1\npath=%s\nuid=%u\ngid=%u\nmode=%04o\nmtime=%ld\ndeleted=%s\n",
            from, (unsigned)st->st_uid, (unsigned)st->st_gid,
            (unsigned)(st->st_mode & 07777), (long)st->st_mtime, when);
    int bad = ferror(f);
    if (fclose(f) || bad) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int read_where(const char *side, char *from, size_t fromn,
                      uid_t *uid, gid_t *gid, mode_t *mode, time_t *mtime)
{
    FILE *f = fopen(side, "r");
    if (!f)
        return -1;
    char line[PATH_MAX + 32];
    *uid = (uid_t)-1;
    *gid = (gid_t)-1;
    *mode = 0;
    *mtime = 0;
    from[0] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "path=", 5))
            snprintf(from, fromn, "%s", line + 5);
        else if (!strncmp(line, "uid=", 4))
            *uid = (uid_t)strtoul(line + 4, NULL, 10);
        else if (!strncmp(line, "gid=", 4))
            *gid = (gid_t)strtoul(line + 4, NULL, 10);
        else if (!strncmp(line, "mode=", 5))
            *mode = (mode_t)strtoul(line + 5, NULL, 8);
        else if (!strncmp(line, "mtime=", 6))
            *mtime = (time_t)strtol(line + 6, NULL, 10);
    }
    fclose(f);
    return from[0] ? 0 : -1;
}

/* --- moving and deleting -------------------------------------------------- */

/* Unlink a file, or a folder and everything in it, without following
   symlinks. For the scratch folders, the Trash itself, and Empty Trash. */
static int delete_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        int bad = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            bad |= delete_tree(child);
        }
        closedir(d);
        return rmdir(path) || bad ? -1 : 0;
    }
    return unlink(path);
}

static int copy_file(const char *from, const char *to, const struct stat *st)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 07777);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while (ok && (n = read(in, buf, sizeof buf)) > 0)
        for (char *p = buf; n > 0; ) {
            ssize_t w = write(out, p, (size_t)n);
            if (w < 0) {
                ok = 0;
                break;
            }
            p += w;
            n -= w;
        }
    if (n < 0)
        ok = 0;
    close(in);
    if (close(out))
        ok = 0;
    if (!ok) {
        unlink(to);
        return -1;
    }
    struct utimbuf ut = { .actime = st->st_atime, .modtime = st->st_mtime };
    utime(to, &ut);
    return 0;
}

static int copy_tree(const char *from, const char *to)
{
    struct stat st;
    if (lstat(from, &st))
        return -1;
    if (S_ISLNK(st.st_mode)) {
        char buf[PATH_MAX];
        ssize_t n = readlink(from, buf, sizeof buf - 1);
        if (n < 0)
            return -1;
        buf[n] = 0;
        return symlink(buf, to);
    }
    if (S_ISREG(st.st_mode))
        return copy_file(from, to, &st);
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTSUP;                /* a device or a socket: leave it be */
        return -1;
    }
    if (mkdir(to, (st.st_mode & 07777) | 0700))
        return -1;
    DIR *d = opendir(from);
    if (!d)
        return -1;
    int bad = 0;
    struct dirent *e;
    while (!bad && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char a[PATH_MAX], b[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        bad = copy_tree(a, b);
    }
    closedir(d);
    chmod(to, st.st_mode & 07777);
    return bad ? -1 : 0;
}

/* A rename, which is instant and copies nothing. Only when the file is on
   another disk (a USB stick) is it copied into ~/Trash, then deleted. */
static int move(const char *from, const char *to)
{
    if (!rename(from, to))
        return 0;
    if (errno != EXDEV)
        return -1;
    if (copy_tree(from, to)) {
        int e = errno;
        delete_tree(to);
        errno = e;
        return -1;
    }
    return delete_tree(from);
}

/* A free name in the Trash: "name", then "name 2", "name 3"... */
static int pick_name(const char *base, char *name, size_t n)
{
    char dest[PATH_MAX];
    for (int i = 1; i < 10000; i++) {
        if (i == 1)
            snprintf(name, n, "%s", base);
        else
            snprintf(name, n, "%s %d", base, i);
        snprintf(dest, sizeof dest, "%s/%s", bin, name);
        struct stat st;
        if (lstat(dest, &st) && errno == ENOENT)
            return 0;
    }
    return -1;
}

static int yes(void)
{
    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == 'y' || *p == 'Y';
}

static const char *kind(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? "directory" : S_ISLNK(st->st_mode) ? "symbolic link"
         : st->st_size ? "regular file" : "regular empty file";
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    int empty = 1;
    while (empty && (e = readdir(d)))
        empty = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
    closedir(d);
    return empty;
}

/* One operand. Its folder is resolved (so "..", and a symlinked folder,
   can't hide where it really is); its last name is not (so a symlink is
   removed as the link, as Unix rm does). */
static void remove_one(const char *arg)
{
    char tmp[PATH_MAX], dir[PATH_MAX], path[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", arg);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;                 /* "dir/" is "dir" */
    char *slash = strrchr(tmp, '/');
    const char *name = slash ? slash + 1 : tmp;
    if (!strcmp(tmp, "/")) {
        fputs(recursive ? "rm: it is dangerous to operate recursively on '/'\n"
                        : "rm: cannot remove '/': Is a directory\n", stderr);
        status = 1;
        return;
    }
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        fprintf(stderr, "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", arg);
        status = 1;
        return;
    }
    if (slash)
        *slash = 0;
    if (!realpath(slash ? (tmp[0] ? tmp : "/") : ".", dir)) {
        if (!(force && errno == ENOENT))
            cannot(arg, strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", name);

    struct stat st;
    if (lstat(path, &st)) {
        if (!(force && errno == ENOENT))
            cannot(arg, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode) && !recursive) {
        if (!empty_dir) {
            cannot(arg, "Is a directory");
            return;
        }
        if (!dir_is_empty(path)) {
            cannot(arg, "Directory not empty");
            return;
        }
    }

    /* The scratch folders, and the Trash itself: a real delete. */
    int real = is_under(path, bin);
    for (size_t i = 0; !real && i < sizeof scratch / sizeof *scratch; i++)
        real = is_under(path, scratch[i]);
    if (real) {
        if (interactive == ASK_ALWAYS) {
            fprintf(stderr, "rm: remove %s '%s'? ", kind(&st), arg);
            if (!yes())
                return;
        }
        if (delete_tree(path)) {
            cannot(arg, strerror(errno));
            return;
        }
        if (trash_item(path)) {
            char side[PATH_MAX];        /* an item of the Trash: its sidecars go too */
            where_of(name, side, sizeof side);
            unlink(side);
            thumb_of(bin, name, side, sizeof side);
            unlink(side);
        }
        if (verbose)
            printf("removed %s'%s'\n", S_ISDIR(st.st_mode) ? "directory " : "", arg);
        return;
    }

    if (is_under(bin, path)) {
        cannot(arg, "the Trash is inside it");
        return;
    }
    if (interactive == ASK_ALWAYS) {
        fprintf(stderr, "rm: move %s '%s' to the Trash? ", kind(&st), arg);
        if (!yes())
            return;
    }
    if (mkdir(bin, 0700) && errno != EEXIST) {
        cannot(arg, "can't make ~/Trash");
        return;
    }
    char tname[NAME_MAX + 16], dest[PATH_MAX];
    if (pick_name(name, tname, sizeof tname)) {
        cannot(arg, "the Trash has 10000 things of this name");
        return;
    }
    snprintf(dest, sizeof dest, "%s/%s", bin, tname);
    /* The record first: if it can't be written, the file stays where it is. */
    if (write_where(tname, path, &st)) {
        cannot(arg, "can't write in ~/Trash");
        return;
    }
    if (move(path, dest)) {
        char side[PATH_MAX];
        int e = errno;
        where_of(tname, side, sizeof side);
        unlink(side);
        cannot(arg, strerror(e));
        return;
    }
    char thumb[PATH_MAX], thumb_to[PATH_MAX];
    thumb_of(dir, name, thumb, sizeof thumb);
    thumb_of(bin, tname, thumb_to, sizeof thumb_to);
    if (!lstat(thumb, &st))
        move(thumb, thumb_to);
    if (verbose)
        printf("moved '%s' to the Trash\n", arg);
}

/* --- put back, empty -------------------------------------------------------- */

static int put_back(const char *name)
{
    char item[PATH_MAX], where[PATH_MAX], from[PATH_MAX];
    if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        fprintf(stderr, "rm: --putBack wants a name in the Trash, not a path: '%s'\n", name);
        return 1;
    }
    snprintf(item, sizeof item, "%s/%s", bin, name);
    where_of(name, where, sizeof where);
    uid_t uid;
    gid_t gid;
    mode_t mode;
    time_t mtime;
    if (read_where(where, from, sizeof from, &uid, &gid, &mode, &mtime) || from[0] != '/') {
        fprintf(stderr, "rm: no record of where '%s' came from\n", name);
        return 1;
    }
    struct stat st;
    if (!lstat(from, &st)) {
        fprintf(stderr, "rm: cannot put back '%s': '%s' is there already\n", name, from);
        return 1;
    }
    if (move(item, from)) {
        fprintf(stderr, "rm: cannot put back '%s': %s\n", name, strerror(errno));
        return 1;
    }
    if (uid != (uid_t)-1)
        lchown(from, uid, gid);         /* works for root; a person owns it already */
    if (mode && !lstat(from, &st) && !S_ISLNK(st.st_mode))
        chmod(from, mode);
    if (mtime) {
        struct utimbuf ut = { .actime = mtime, .modtime = mtime };
        utime(from, &ut);
    }
    unlink(where);
    char thumb[PATH_MAX], thumb_to[PATH_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", from);
    *strrchr(dir, '/') = 0;             /* from is absolute: there is a '/' */
    thumb_of(bin, name, thumb, sizeof thumb);
    thumb_of(dir, base_of(from), thumb_to, sizeof thumb_to);
    if (!lstat(thumb, &st))
        move(thumb, thumb_to);
    return 0;
}

static int empty_trash(void)
{
    DIR *d = opendir(bin);
    if (!d)
        return errno == ENOENT ? 0 : 1;
    struct dirent *e;
    int bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", bin, e->d_name);
        if (delete_tree(path)) {
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
            bad = 1;
        }
    }
    closedir(d);
    return bad;
}

/* --- the command line ------------------------------------------------------ */

static void usage(FILE *f)
{
    fputs("Usage: rm [OPTION]... FILE...\n"
          "MyOS: moves each FILE to ~/Trash, where it can be put back.\n"
          "Files in /tmp, /var/tmp, /dev/shm and /run, and in the Trash, are deleted.\n"
          "\n"
          "  -f, --force           ignore missing files, never ask\n"
          "  -i                    ask before each one\n"
          "  -I                    ask once, before more than three, or -r\n"
          "      --interactive[=WHEN]  never, once (-I) or always (-i)\n"
          "  -r, -R, --recursive   folders and what is in them\n"
          "  -d, --dir             empty folders\n"
          "  -v, --verbose         say what is done\n"
          "      --reallyDelete    GNU rm: delete for good\n"
          "      --putBack NAME    put NAME back where it came from\n"
          "      --emptyTrash      delete everything in ~/Trash\n"
          "      --help, --version\n", f);
}

static void bad_usage(const char *fmt, const char *what)
{
    fputs("rm: ", stderr);
    fprintf(stderr, fmt, what);
    fputs("\nTry 'rm --help' for more information.\n", stderr);
    exit(1);
}

int main(int argc, char **argv)
{
    uid_t uid = geteuid();
    int person = uid >= PEOPLE_MIN && uid < PEOPLE_MAX;
    int really = 0, empty = 0, endopts = 0, nfiles = 0;
    const char *putname = NULL;
    char *files[argc];

    /* Our three verbs first: they decide whether GNU rm runs at all. */
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--reallyDelete") || !strcmp(argv[i], "--really-delete"))
            really = 1;
        else if (!strcmp(argv[i], "--emptyTrash") || !strcmp(argv[i], "--empty-trash"))
            empty = 1;
        else if (!strcmp(argv[i], "--putBack") || !strcmp(argv[i], "--put-back")) {
            if (i + 1 >= argc)
                bad_usage("%s needs the name of something in the Trash", argv[i]);
            putname = argv[++i];
        }
    }
    if (empty || putname) {
        if (find_trash()) {
            fputs("rm: no home folder, so no Trash\n", stderr);
            return 1;
        }
        return empty ? empty_trash() : put_back(putname);
    }
    if (really || !person)
        exec_gnu(argc, argv);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (endopts || a[0] != '-' || !a[1]) {
            files[nfiles++] = argv[i];
            continue;
        }
        if (a[1] == '-') {
            if (!a[2])
                endopts = 1;
            else if (!strcmp(a, "--force"))
                force = 1, interactive = ASK_NEVER;
            else if (!strcmp(a, "--recursive"))
                recursive = 1;
            else if (!strcmp(a, "--dir"))
                empty_dir = 1;
            else if (!strcmp(a, "--verbose"))
                verbose = 1;
            else if (!strcmp(a, "--interactive") || !strcmp(a, "--interactive=always"))
                interactive = ASK_ALWAYS, force = 0;
            else if (!strcmp(a, "--interactive=once"))
                interactive = ASK_ONCE, force = 0;
            else if (!strcmp(a, "--interactive=never"))
                interactive = ASK_NEVER;
            else if (!strcmp(a, "--one-file-system") || !strcmp(a, "--no-preserve-root") ||
                     !strncmp(a, "--preserve-root", 15))
                ;                       /* a move never crosses into other disks' trees */
            else if (!strcmp(a, "--help")) {
                usage(stdout);
                return 0;
            } else if (!strcmp(a, "--version")) {
                puts("rm (MyOS): moves to ~/Trash; GNU rm is rm --reallyDelete");
                return 0;
            } else
                bad_usage("unrecognized option '%s'", a);
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'f': force = 1; interactive = ASK_NEVER; break;
            case 'i': interactive = ASK_ALWAYS; force = 0; break;
            case 'I': interactive = ASK_ONCE; force = 0; break;
            case 'r': case 'R': recursive = 1; break;
            case 'd': empty_dir = 1; break;
            case 'v': verbose = 1; break;
            default: {
                char c[2] = { *p, 0 };
                bad_usage("invalid option -- '%s'", c);
            }
            }
        }
    }

    if (!nfiles) {
        if (force)
            return 0;
        bad_usage("missing operand%s", "");
    }
    if (find_trash()) {
        fputs("rm: no home folder, so no Trash: nothing was moved\n", stderr);
        return 1;
    }
    if (interactive == ASK_ONCE && (nfiles > 3 || recursive)) {
        fprintf(stderr, "rm: move %d argument%s%s to the Trash? ", nfiles,
                nfiles == 1 ? "" : "s", recursive ? " recursively" : "");
        if (!yes())
            return 0;
    }
    for (int i = 0; i < nfiles; i++)
        remove_one(files[i]);
    return status;
}
