/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-kconfig — what Settings › Kernel runs to read and save the kernel
 * config (docs/KERNEL-CONFIG.md) in the myos-config partition.
 *
 * Installed setuid root, like myos-adduser: Settings runs as you, and only
 * root may write a partition. Settings writes the request to our stdin:
 *
 *   read   detect            save
 *                            admin name
 *                            admin password
 *                            the config, in its text form, to the end
 *
 * and reads the answer:
 *   "ok A 7" (the copy and generation in use) and, after "read", the text
 *   "none <why>"   nothing saved yet, or no myos-config partition
 *   anything else  what went wrong, in words a person can read
 *
 * Save trusts nothing Settings checked: the administrator's password is
 * checked again (as myos-adduser checks it), and so is the whole config,
 * programs' files included, before anything is written.
 */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kconfig.h"

#define FIELD 256
#define TEXT_MAX 65536

static int read_field(char *buf)
{
    if (!fgets(buf, FIELD, stdin))
        return -1;
    size_t n = strcspn(buf, "\n");
    if (buf[n] != '\n')
        return -1;
    buf[n] = 0;
    return 0;
}

/* --- the administrator: the same check myos-adduser makes ---------------------- */

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

/* pam_unix's "nullok" lets an account with an empty password in whatever
   is typed (the live disk's myos has one), so for those only an empty
   password counts. */
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

/* --- read and save ------------------------------------------------------------- */

static int do_read(void)
{
    char path[64], err[256], which;
    struct kc_config c;
    if (kc_find_partition(path, sizeof path)) {
        printf("none This disk has no myos-config partition (a newer installer makes one).\n");
        return 0;
    }
    if (kc_read_partition(path, &c, &which, err, sizeof err)) {
        printf("none %s\n", err);
        return 0;
    }
    char *text = kc_format(&c);
    printf("ok %c %u %s\n%s", which, c.generation, path, text);
    free(text);
    return 0;
}

static int do_save(void)
{
    char admin[FIELD], pass[FIELD], path[64], err[256], which;
    if (read_field(admin) || read_field(pass)) {
        printf("Settings sent a request this helper can't read.\n");
        return 1;
    }
    int ok = admin_ok(admin, pass);
    explicit_bzero(pass, sizeof pass);
    if (!ok) {
        printf("That administrator name or password is wrong.\n");
        return 2;
    }
    static char text[TEXT_MAX + 1];
    size_t n = fread(text, 1, TEXT_MAX + 1, stdin);
    if (n > TEXT_MAX) {
        printf("The config is too long.\n");
        return 3;
    }
    text[n] = 0;
    struct kc_config c;
    struct kc_machine m;
    struct kc_msgs msgs;
    if (kc_parse(text, &c, err, sizeof err)) {
        printf("The config can't be read: %s\n", err);
        return 3;
    }
    /* For this computer: its own architecture and byte order. */
#ifdef __x86_64__
    snprintf(c.arch, sizeof c.arch, "x86_64");
#else
    snprintf(c.arch, sizeof c.arch, "i386");
#endif
    c.big = 0;
    kc_detect(&m);
    kc_check(&c, &m, 1, &msgs);
    if (msgs.nerr) {
        printf("%s\n", msgs.err[0]);
        return 3;
    }
    if (kc_find_partition(path, sizeof path)) {
        printf("This disk has no myos-config partition, so there is nowhere to save.\n");
        return 4;
    }
    if (kc_write_partition(path, &c, &which, err, sizeof err)) {
        printf("%s\n", err);
        return 4;
    }
    struct kc_config back;
    char w2;
    if (kc_read_partition(path, &back, &w2, err, sizeof err) || w2 != which) {
        printf("Saved, but reading it back failed: %s\n", err);
        return 4;
    }
    printf("ok %c %u %s\n", which, back.generation, path);
    return 0;
}

/* What this computer has, as Settings sees it (kc_detect): nothing a user
   can't see, so it runs as the caller, before we become root. */
static int do_detect(void)
{
    struct kc_machine m;
    kc_detect(&m);
    printf("ok %s%s, %s\n", m.cpu, m.virt ? " (virtual machine)" : "",
           m.family >= 0 ? kc_family_name(m.family) : "known by its CPUID bits");
    for (int i = 0; i < kc_nfeatures(); i++) {
        const char *why;
        int has = kc_feature_ok(&m, kc_feature(i), &why);
        printf("%-28s %-5s %s\n", kc_feature(i)->label, has == KC_HAS ? "yes" : has == KC_MAYBE ? "maybe" : "no",
               why ? why : "");
    }
    return 0;
}

int main(void)
{
    char mode[FIELD];
    if (read_field(mode)) {
        printf("Settings sent a request this helper can't read.\n");
        return 1;
    }
    if (!strcmp(mode, "detect"))        /* as the caller: root for good gone first */
        return setgid(getgid()) || setuid(getuid()) ? 1 : do_detect();
    if (setuid(0) || setgid(0)) {
        printf("This helper must be installed setuid root.\n");
        return 1;
    }
    clearenv();
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    umask(022);
    if (!strcmp(mode, "read"))
        return do_read();
    if (!strcmp(mode, "save"))
        return do_save();
    printf("Settings sent a request this helper can't read.\n");
    return 1;
}
