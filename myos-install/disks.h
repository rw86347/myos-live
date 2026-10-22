/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Disks the kernel has published under /sys/block. Used by the window
 * (to list them) and by the helper (to check the plan's disk still exists
 * and is not the live stick).
 */
#ifndef MYOS_INSTALL_DISKS_H
#define MYOS_INSTALL_DISKS_H

#include <stddef.h>

#define DISKS_MAX 8
#define DISK_PARTS 16

struct disk_part {
    char name[32];              /* sda1, nvme0n1p1 */
    char label[64];             /* PARTNAME from sysfs, if any */
    unsigned long long bytes;
    unsigned long long start;   /* sectors */
    int is_esp;
    int is_myos;
};

struct disk_info {
    char path[64];              /* /dev/sda */
    char sysname[32];           /* sda */
    char model[80];
    char name[128];             /* "Samsung SSD 980 (NVMe)" */
    char summary[80];           /* "Windows likely" / "MyOS is running from this disk" */
    unsigned long long bytes;
    unsigned long long free_bytes;
    int removable;
    int in_use;                 /* something is mounted from it: never the target */
    int running;                /* ...and that is "/": the live system itself */
    int has_system;             /* an EFI partition and a large one: keep the ESP */
    int has_windows;            /* ...and Windows' own partition names */
    int npart;
    int upgradable;             /* myos-root + myos-data, same arch, our EFI */
    char system_ver[64];
    char kernel_ver[64];
    char why_not[160];          /* why it cannot be upgraded, if not */
    struct disk_part parts[DISK_PARTS];
};

int disks_scan(struct disk_info *out, int max);
int disks_firmware_uefi(void);
void disks_format_bytes(unsigned long long n, char *out, size_t len);
void disks_suggest_hostname(char *out, size_t n, const char *user);
/* The live stick's Wi-Fi networks, on its myos-data (/etc/myos/stick-data.sh). */
#define STICK_WIFI "/data/stick/wpa_supplicant.conf"
int disks_wifi_ssid(char *out, size_t n);
const struct disk_info *disks_find(const struct disk_info *list, int n, const char *path);
void disks_part_path(const char *disk, int partno, char *out, size_t n);
int disks_has_esp(const struct disk_info *d);
const char *disks_free_problem(const struct disk_info *d, unsigned long long need);

#endif
