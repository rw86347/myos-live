/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * power_supply.h — the battery and the charger, as Linux's /sys shows them
 *
 * Shared by the tray (myos-menu draws the battery icon) and the pop-up
 * (myos-battery). The kernel (drivers/battery.c) keeps these files
 * up to date every 10 seconds; reading them never waits for the hardware.
 *
 *   /sys/class/power_supply/<name>/type      "Battery" or "Mains"
 *     status          Charging, Discharging, Full, Not charging
 *     capacity        percent left
 *     energy_now      µWh left      } or charge_now / charge_full /
 *     energy_full     µWh when full } charge_full_design (µAh) and
 *     energy_full_design            } current_now (µA): a battery
 *     power_now       µW in or out  } that counts in mAh
 *     cycle_count
 *   /sys/class/power_supply/<name>/online    the charger: 1 plugged in
 *
 * The same files on any Linux laptop, so this works there too. Times are
 * worked out here, as upower does: what is left (or missing) over the rate.
 */
#ifndef POWER_SUPPLY_H
#define POWER_SUPPLY_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS_DIR "/sys/class/power_supply"

struct power_supply {
    int present;                    /* a battery was found */
    int on_ac;                      /* the charger is plugged in */
    int percent;                    /* -1: unknown */
    int charging, discharging, full;
    long long now, last_full, design, rate;  /* µWh and µW, or µAh and µA */
    int unit_mah;
    int cycles;                     /* -1: unknown */
    char name[16];
};

/* One number from dir/file, or -1. */
static inline long long ps_num(const char *dir, const char *file)
{
    char path[300], buf[32];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long long v = -1;
    if (fgets(buf, sizeof buf, f))
        v = strtoll(buf, NULL, 10);
    fclose(f);
    return v;
}

/* One word from dir/file ("Charging"), "" if none. */
static inline void ps_word(const char *dir, const char *file, char *out, int size)
{
    char path[300];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(out, size, f))
        out[0] = 0;
    out[strcspn(out, "\n")] = 0;
    fclose(f);
}

/* The first battery and the first charger under root (PS_DIR; a test
   passes its own). 0 if there is a battery, -1 if not. */
static inline int ps_read(const char *root, struct power_supply *p)
{
    memset(p, 0, sizeof *p);
    p->percent = p->cycles = -1;
    DIR *d = opendir(root);
    if (!d)
        return -1;
    struct dirent *e;
    char dir[280], word[32];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        ps_word(dir, "type", word, sizeof word);
        if (!strcmp(word, "Mains")) {
            if (ps_num(dir, "online") == 1)
                p->on_ac = 1;
            continue;
        }
        if (strcmp(word, "Battery") || p->present || ps_num(dir, "present") == 0)
            continue;
        p->present = 1;
        snprintf(p->name, sizeof p->name, "%.15s", e->d_name);
        p->percent = (int)ps_num(dir, "capacity");
        ps_word(dir, "status", word, sizeof word);
        p->charging = !strcmp(word, "Charging");
        p->discharging = !strcmp(word, "Discharging");
        p->full = !strcmp(word, "Full");
        p->now = ps_num(dir, "energy_now");
        if (p->now >= 0) {
            p->last_full = ps_num(dir, "energy_full");
            p->design = ps_num(dir, "energy_full_design");
            p->rate = ps_num(dir, "power_now");
        } else {
            p->unit_mah = 1;
            p->now = ps_num(dir, "charge_now");
            p->last_full = ps_num(dir, "charge_full");
            p->design = ps_num(dir, "charge_full_design");
            p->rate = ps_num(dir, "current_now");
        }
        p->cycles = (int)ps_num(dir, "cycle_count");
        if (p->percent < 0 && p->now >= 0 && p->last_full > 0)
            p->percent = (int)(p->now >= p->last_full ? 100 : p->now * 100 / p->last_full);
    }
    closedir(d);
    if (p->percent > 100)
        p->percent = 100;
    return p->present ? 0 : -1;
}

/* Minutes until empty (discharging) or full (charging); -1 if it can't be
   told yet (no rate: the firmware hasn't measured one). */
static inline int ps_minutes(const struct power_supply *p)
{
    if (p->rate <= 0 || p->now < 0)
        return -1;
    long long left;
    if (p->discharging)
        left = p->now;
    else if (p->charging && p->last_full > 0)
        left = p->last_full > p->now ? p->last_full - p->now : 0;
    else
        return -1;
    return (int)(left * 60 / p->rate);
}

/* Health: what it holds when full now, as a percent of what it held new;
   -1 if the firmware doesn't say. */
static inline int ps_health(const struct power_supply *p)
{
    if (p->last_full <= 0 || p->design <= 0)
        return -1;
    long long h = p->last_full * 100 / p->design;
    return h > 100 ? 100 : (int)h;
}

#endif
