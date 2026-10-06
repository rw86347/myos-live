/*
 * myos-adduser — what Settings' "Add User…" runs to make an account.
 *
 * Installed setuid root (like passwd), so a standard user can add one too,
 * as on the Mac, if an administrator types their name and password.
 * Settings writes the request to our stdin, one field per line:
 *
 *   check            add
 *   admin name       admin name
 *   admin password   admin password
 *                    full name
 *                    account name
 *                    password
 *                    "admin" or "standard"
 *
 * and reads one line back: "ok", or what went wrong in words a person can
 * read. "check" only asks whether the administrator's password is right
 * (Settings asks before it shows the form); "add" asks again, because
 * every run stands alone.
 *
 * Why not shadow's useradd: it locks /etc/passwd by making a hard link, and
 * MyOS has none yet (docs/gaps.md). So this writes the three files itself,
 * the way useradd would: under an flock on /etc/.pwd.lock, each file written
 * whole to a new name, synced, then renamed over the old one -- a reader
 * (login, ls) sees the old file or the new one, never half of one. passwd
 * goes last, so the account only appears once its group and password exist.
 */
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FIELD 256
#define UID_MIN 1000            /* what the distribution's useradd hands out */
#define UID_MAX 59999

static void say(const char *msg)
{
    printf("%s\n", msg);
    fflush(stdout);
}

static int read_field(char *buf)
{
    if (!fgets(buf, FIELD, stdin))
        return -1;
    size_t n = strcspn(buf, "\n");
    if (buf[n] != '\n')
        return -1;              /* too long, or no end: refuse rather than cut */
    buf[n] = 0;
    return 0;
}

/* --- the administrator ---------------------------------------------------------- */

static int conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    struct pam_response *r = calloc(n, sizeof *r);
    if (!r)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; i++)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF && !(r[i].resp = strdup(data))) {
            free(r);
            return PAM_BUF_ERR;
        }
    *resp = r;
    return PAM_SUCCESS;
}

static int in_wheel(const char *user)
{
    struct group *g = getgrnam("wheel");
    if (!g)
        return 0;
    for (char **m = g->gr_mem; *m; m++)
        if (!strcmp(*m, user))
            return 1;
    return 0;
}

/* The same check the login screen makes (pam_unix against /etc/shadow),
   plus one: pam_unix's "nullok" lets an account with an empty password in
   whatever is typed (the live disk's root and myos have one), so for those
   only an empty password counts. */
static int admin_ok(const char *name, const char *password)
{
    struct spwd *sp = getspnam(name);
    if (sp && sp->sp_pwdp && !sp->sp_pwdp[0] && password[0]) {
        sleep(1);
        return 0;
    }
    struct pam_conv conv = {conversation, (void *)password};
    pam_handle_t *pamh = NULL;
    int err = pam_start("myos-admin", name, &conv, &pamh);
    if (err == PAM_SUCCESS)
        err = pam_authenticate(pamh, 0);
    if (err == PAM_SUCCESS)
        err = pam_acct_mgmt(pamh, 0);
    pam_end(pamh, err);
    if (err != PAM_SUCCESS || !in_wheel(name)) {
        sleep(1);               /* slows down guessing */
        return 0;
    }
    return 1;
}

/* --- checking the new account ------------------------------------------------------ */

/* useradd's rule: a lower-case letter or _, then letters, digits, _ or -. */
static int good_account(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 31 || !(islower((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (size_t i = 1; i < n; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

/* The full name goes in passwd's comment field: no ':' (field separator),
   no ',' (the comment's own separator), nothing unprintable. UTF-8 is fine. */
static int good_full_name(const char *s)
{
    if (!*s || strlen(s) > 64)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p == 127 || *p == ':' || *p == ',')
            return 0;
    return 1;
}

/* --- the files ------------------------------------------------------------------------ */

static char *slurp(const char *path, struct stat *st)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (fstat(fileno(f), st) || st->st_size > 1 << 20) {
        fclose(f);
        return NULL;
    }
    char *s = malloc(st->st_size + 2);
    size_t n = s ? fread(s, 1, st->st_size, f) : 0;
    fclose(f);
    if (!s)
        return NULL;
    if (n && s[n - 1] != '\n')
        s[n++] = '\n';
    s[n] = 0;
    return s;
}

/* Write `text` to path+ with the old file's owner and mode, sync, rename. */
static int replace(const char *path, const char *text, const struct stat *old)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s+", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    size_t n = strlen(text), done = 0;
    while (done < n) {
        ssize_t w = write(fd, text + done, n - done);
        if (w <= 0)
            break;
        done += w;
    }
    int ok = done == n && fchown(fd, old->st_uid, old->st_gid) == 0 &&
             fchmod(fd, old->st_mode & 07777) == 0 && fsync(fd) == 0;
    ok &= close(fd) == 0;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Does any line of `text` start with "name:"? */
static int has_entry(const char *text, const char *name)
{
    size_t n = strlen(name);
    for (const char *l = text; *l; l = strchr(l, '\n') + 1) {
        if (!strncmp(l, name, n) && l[n] == ':')
            return 1;
        if (!strchr(l, '\n'))
            break;
    }
    return 0;
}

/* The group file with `user` added to each group in `groups` and its own
   group appended at the end. */
static char *add_to_groups(const char *text, const char *user, gid_t gid, const char *const *groups)
{
    size_t cap = strlen(text) + 64 * 8 + 2 * strlen(user) + 64;
    char *out = malloc(cap), *o = out;
    if (!out)
        return NULL;
    for (const char *l = text; *l;) {
        const char *end = strchr(l, '\n');
        size_t len = end ? (size_t)(end - l) : strlen(l);
        memcpy(o, l, len);
        o += len;
        int member = 0;
        for (const char *const *g = groups; *g; g++)
            if (!strncmp(l, *g, strlen(*g)) && l[strlen(*g)] == ':')
                member = 1;
        if (member) {
            /* name:x:gid:a,b -- add after the last ':' */
            int colons = 0;
            for (size_t i = 0; i < len; i++)
                colons += l[i] == ':';
            if (colons == 3)
                o += sprintf(o, "%s%s", l[len - 1] == ':' ? "" : ",", user);
        }
        *o++ = '\n';
        if (!end)
            break;
        l = end + 1;
    }
    o += sprintf(o, "%s:x:%u:\n", user, (unsigned)gid);
    *o = 0;
    return out;
}

/* The first id from UID_MIN up that no user and no group has. */
static uid_t free_id(void)
{
    for (uid_t id = UID_MIN; id <= UID_MAX; id++)
        if (!getpwuid(id) && !getgrgid(id))
            return id;
    return 0;
}

/* sha512-crypt ("$6$"), what Alpine's passwd uses; 16 random salt characters. */
static char *hash(const char *password)
{
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[16];
    char salt[3 + 16 + 2] = "$6$";
    if (getrandom(r, sizeof r, 0) != sizeof r)
        return NULL;
    for (int i = 0; i < 16; i++)
        salt[3 + i] = abc[r[i] % 64];
    strcpy(salt + 19, "$");
    return crypt(password, salt);
}

/* Copy /etc/skel into the new home (files, folders and links), owned by the user. */
static void copy_skel(const char *from, const char *to, uid_t uid, gid_t gid, int depth)
{
    DIR *d = depth <= 8 ? opendir(from) : NULL;
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char src[PATH_MAX], dst[PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        if (lstat(src, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(dst, st.st_mode & 0777) == 0) {
                lchown(dst, uid, gid);
                copy_skel(src, dst, uid, gid, depth + 1);
            }
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlink(src, target, sizeof target - 1);
            if (n > 0) {
                target[n] = 0;
                if (symlink(target, dst) == 0)
                    lchown(dst, uid, gid);
            }
        } else if (S_ISREG(st.st_mode)) {
            int in = open(src, O_RDONLY | O_CLOEXEC);
            int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
            char buf[4096];
            ssize_t n;
            while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0)
                if (write(out, buf, n) != n)
                    break;
            if (out >= 0) {
                fchown(out, uid, gid);
                close(out);
            }
            if (in >= 0)
                close(in);
        }
    }
    closedir(d);
}

static const char *add(const char *full, const char *user, const char *password, int admin)
{
    struct stat pst, gst, sst;
    char *passwd = slurp("/etc/passwd", &pst), *group = slurp("/etc/group", &gst),
         *shadow = slurp("/etc/shadow", &sst);
    if (!passwd || !group || !shadow)
        return "Can't read the account files.";
    if (has_entry(passwd, user) || has_entry(group, user) || has_entry(shadow, user))
        return "There is already an account with that name.";
    uid_t id = free_id();
    if (!id)
        return "There are no free user numbers left.";
    char *h = hash(password);
    if (!h || h[0] != '$')
        return "Can't make the password's hash.";

    /* netdev: wpa_supplicant's control socket, so anyone can join Wi-Fi. */
    static const char *const standard[] = {"audio", "video", "input", "netdev", NULL};
    static const char *const administrator[] = {"wheel", "audio", "video", "input", "netdev", NULL};
    char *newgroup = add_to_groups(group, user, id, admin ? administrator : standard);
    size_t sn = strlen(shadow), pn = strlen(passwd);
    char *newshadow = malloc(sn + strlen(user) + strlen(h) + 64);
    char *newpasswd = malloc(pn + strlen(user) * 2 + strlen(full) + 64);
    if (!newgroup || !newshadow || !newpasswd)
        return "Out of memory.";
    sprintf(newshadow, "%s%s:%s:%ld:0:99999:7:::\n", shadow, user, h, (long)(time(NULL) / 86400));
    sprintf(newpasswd, "%s%s:x:%u:%u:%s:/home/%s:/bin/bash\n", passwd, user, (unsigned)id, (unsigned)id, full,
            user);

    if (replace("/etc/shadow", newshadow, &sst) || replace("/etc/group", newgroup, &gst))
        return "Can't write the account files.";

    char home[PATH_MAX];
    snprintf(home, sizeof home, "/home/%s", user);
    if (mkdir(home, 0700) && errno != EEXIST)
        return "Can't make the home folder.";
    chown(home, id, id);
    copy_skel("/etc/skel", home, id, id, 0);

    if (replace("/etc/passwd", newpasswd, &pst))
        return "Can't write the account files.";
    return NULL;
}

int main(void)
{
    char mode[FIELD], admin[FIELD], apass[FIELD];
    if (setuid(0) || setgid(0)) {
        say("This helper must be installed setuid root.");
        return 1;
    }
    umask(022);
    if (read_field(mode) || read_field(admin) || read_field(apass)) {
        say("Settings sent a request this helper can't read.");
        return 1;
    }
    int ok = admin_ok(admin, apass);
    explicit_bzero(apass, sizeof apass);
    if (!ok) {
        say("That administrator name or password is wrong.");
        return 2;
    }
    if (!strcmp(mode, "check")) {
        say("ok");
        return 0;
    }

    char full[FIELD], user[FIELD], pass[FIELD], kind[FIELD];
    if (strcmp(mode, "add") || read_field(full) || read_field(user) || read_field(pass) || read_field(kind)) {
        say("Settings sent a request this helper can't read.");
        return 1;
    }
    if (!good_full_name(full)) {
        explicit_bzero(pass, sizeof pass);
        say("The full name can't be empty or contain : or ,");
        return 3;
    }
    if (!good_account(user)) {
        explicit_bzero(pass, sizeof pass);
        say("The account name must start with a small letter and use only small letters, digits, - and _.");
        return 3;
    }
    if (!*pass) {
        say("The new account needs a password.");
        return 3;
    }

    int lock = open("/etc/.pwd.lock", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX)) {
        explicit_bzero(pass, sizeof pass);
        say("Can't lock the account files.");
        return 4;
    }
    const char *err = add(full, user, pass, !strcmp(kind, "admin"));
    explicit_bzero(pass, sizeof pass);
    close(lock);
    say(err ? err : "ok");
    return err ? 4 : 0;
}
