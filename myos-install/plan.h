/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * The installer's plan: every answer, written as `key=value` lines.
 *
 * myos-install asks the questions and writes this file. The setuid helper
 * reads it, checks it again, and is the only process that may write a disk.
 * Nothing is written until the summary page's Install button (or
 * `myos-install-helper --apply`).
 *
 * A password line is allowed only if the file is mode 0600. The helper
 * never prints it.
 */
#ifndef MYOS_INSTALL_PLAN_H
#define MYOS_INSTALL_PLAN_H

#include <stddef.h>
#include <stdio.h>

#define PLAN_VERSION 1
#define PLAN_PATH_DEFAULT "/tmp/myos-install.plan"
#define PLAN_LOG_DEFAULT "/tmp/myos-install.log"

#define PLAN_NAME 64
#define PLAN_PATH 128
#define PLAN_LINE 256
#define PLAN_PASS 128
#define PLAN_USER 32

#define MIN_INSTALL_GB 8
#define DEFAULT_ROOT_GB 32
#define ESP_MIB 512                 /* the stick's start files (INITRD.CPIO ~240 MB) and room */
#define BOOT_AREA_MIB 1

enum install_mode {
    MODE_FREE = 0,              /* only unpartitioned space; keep what exists */
    MODE_ERASE,                 /* the whole disk becomes MyOS */
    MODE_CUSTOM                 /* sizes chosen on the partitions page */
};

enum install_firmware {
    FW_UEFI = 0,
    FW_BIOS
};

struct install_plan {
    char lang[32];              /* e.g. en_US.UTF-8 */
    char lang_name[PLAN_NAME];
    int cjk;
    char keyboard[32];          /* XKB layout, e.g. us */
    char keyboard_name[PLAN_NAME];
    char timezone[PLAN_NAME];   /* tzdata name, e.g. Asia/Shanghai */
    int ntp;
    int hwclock_local;          /* 1 = Windows dual-boot: RTC in local time */
    char disk[PLAN_PATH];       /* /dev/nvme0n1 */
    char disk_name[PLAN_NAME];
    unsigned long long disk_bytes;
    unsigned long long free_bytes;
    int mode;
    int keep_esp;
    int root_gb;
    int separate_home;
    int encrypt;                /* always 0 until encryption exists */
    char fullname[PLAN_NAME];
    char user[PLAN_USER];
    char password[PLAN_PASS];
    char hostname[PLAN_NAME];
    int ask_password;
    int guest;
    int copy_wifi;
    char wifi_ssid[PLAN_NAME];
    char compositor[16];        /* none, xrender, gl */
    double compositor_ms;
    int firmware;
};

void plan_init(struct install_plan *p);
int plan_write(const char *path, const struct install_plan *p);
int plan_read(const char *path, struct install_plan *p);
int plan_read_file(FILE *f, struct install_plan *p);
/* 0 if the plan is complete enough to run; otherwise a sentence in err. */
int plan_check(const struct install_plan *p, char *err, size_t errlen);
void plan_describe(const struct install_plan *p, char *out, size_t n);
const char *plan_mode_name(int mode);
const char *plan_firmware_name(int firmware);
int plan_good_user(const char *s);
int plan_good_fullname(const char *s);
int plan_good_zone(const char *s);      /* a tzdata name, safe after /usr/share/zoneinfo/ */
int plan_good_hostname(const char *s);
void plan_suggest_user(const char *full, char *out, size_t n);
int plan_password_score(const char *s); /* 0 empty .. 4 strong */

#endif
