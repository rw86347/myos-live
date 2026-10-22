/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * test-power-supply.c — power_supply.h on the build machine (make
 * test-power-supply): a made-up /sys/class/power_supply in a temporary
 * folder, read as the tray and the pop-up read the real one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "power_supply.h"

static char root[64];
static int fails;

static void put(const char *dev, const char *file, const char *text)
{
    char path[200];
    snprintf(path, sizeof path, "%s/%s", root, dev);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s", root, dev, file);
    FILE *f = fopen(path, "w");
    fprintf(f, "%s\n", text);
    fclose(f);
}

#define CHECK(what, got, want) do { \
        long long g_ = (got), w_ = (want); \
        if (g_ != w_) { printf("FAIL: %s: %lld, not %lld\n", what, g_, w_); fails++; } \
    } while (0)

int main(void)
{
    snprintf(root, sizeof root, "/tmp/ps-test-%d", (int)getpid());
    mkdir(root, 0755);
    struct power_supply p;

    CHECK("no battery", ps_read(root, &p), -1);

    /* tools/fakebat.py's battery: 30 of 45 Wh, 50 Wh new, 9 W out. */
    put("AC", "type", "Mains");
    put("AC", "online", "0");
    put("BAT0", "type", "Battery");
    put("BAT0", "present", "1");
    put("BAT0", "status", "Discharging");
    put("BAT0", "capacity", "66");
    put("BAT0", "energy_now", "30000000");
    put("BAT0", "energy_full", "45000000");
    put("BAT0", "energy_full_design", "50000000");
    put("BAT0", "power_now", "9000000");
    put("BAT0", "cycle_count", "127");
    CHECK("found", ps_read(root, &p), 0);
    CHECK("percent", p.percent, 66);
    CHECK("discharging", p.discharging, 1);
    CHECK("on the charger", p.on_ac, 0);
    CHECK("minutes left (30 Wh at 9 W)", ps_minutes(&p), 200);
    CHECK("health (45 of 50)", ps_health(&p), 90);
    CHECK("cycles", p.cycles, 127);

    /* Plugged in and charging: 15 Wh to go at 30 W. */
    put("AC", "online", "1");
    put("BAT0", "status", "Charging");
    put("BAT0", "power_now", "30000000");
    ps_read(root, &p);
    CHECK("on the charger", p.on_ac, 1);
    CHECK("minutes to full", ps_minutes(&p), 30);

    /* No rate measured yet: no time. */
    put("BAT0", "power_now", "0");
    ps_read(root, &p);
    CHECK("no rate", ps_minutes(&p), -1);

    /* A battery that counts in mAh, without a capacity file. */
    char cmd[100];
    snprintf(cmd, sizeof cmd, "rm -rf %s/BAT0", root);
    system(cmd);
    put("BAT0", "type", "Battery");
    put("BAT0", "status", "Discharging");
    put("BAT0", "charge_now", "1000000");
    put("BAT0", "charge_full", "4000000");
    put("BAT0", "charge_full_design", "5000000");
    put("BAT0", "current_now", "2000000");
    ps_read(root, &p);
    CHECK("mAh: counted in charge", p.unit_mah, 1);
    CHECK("mAh: percent worked out", p.percent, 25);
    CHECK("mAh: minutes left", ps_minutes(&p), 30);
    CHECK("mAh: health", ps_health(&p), 80);
    CHECK("mAh: cycles unknown", p.cycles, -1);

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    system(cmd);
    printf(fails ? "POWER SUPPLY TEST FAILED\n" : "POWER SUPPLY TEST OK\n");
    return fails != 0;
}
