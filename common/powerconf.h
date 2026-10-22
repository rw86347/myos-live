/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * powerconf.h — /etc/myos/power.conf, shared by myos-powerd (the lid, the
 * power button) and myos-idle (locking, sleeping when nobody is there).
 * Lines are key=value, '#' starts a comment; the keys and their values are
 * docs/power-performance-settings.md's. A missing file or key gives the
 * default. Read when it matters, not cached: an edit needs no restart.
 */
#ifndef POWERCONF_H
#define POWERCONF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWER_CONF "/etc/myos/power.conf"
#define POWER_FIFO "/run/myos/power"    /* "sleep" and "locked" go to myos-powerd */

/* key's value (one word) into out; def if it isn't there. */
static inline const char *power_conf(const char *key, const char *def, char *out, size_t size)
{
    char line[256];
    size_t klen = strlen(key);
    FILE *f = fopen(POWER_CONF, "re");
    snprintf(out, size, "%s", def);
    while (f && fgets(line, sizeof line, f)) {
        char *p = line + strspn(line, " \t");
        if (strncmp(p, key, klen) != 0)
            continue;
        p += klen;
        p += strspn(p, " \t");
        if (*p++ != '=')
            continue;
        p += strspn(p, " \t");
        snprintf(out, size, "%.*s", (int)strcspn(p, " \t#\r\n"), p);
        break;
    }
    if (f)
        fclose(f);
    return out;
}

static inline int power_conf_int(const char *key, int def)
{
    char d[16], v[16];
    snprintf(d, sizeof d, "%d", def);
    return atoi(power_conf(key, d, v, sizeof v));
}

#endif
