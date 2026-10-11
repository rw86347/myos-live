/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kconfig.h — the kernel config (docs/KERNEL-CONFIG.md), for Settings ›
 * Kernel and its helper myos-kconfig: the text form, the safety checks, and
 * the two parts on the disk (the 512-byte boot block and the 5 KiB task
 * list, in copies A and B of the myos-config partition).
 *
 * The reference is tools/kconfig/kconfig.py: the same text form, the same
 * checks, the same bytes. The power features come from
 * tools/kconfig/powerfeatures.py (made into kconfig_features.h by the build).
 */
#ifndef MYOS_KCONFIG_H
#define MYOS_KCONFIG_H

#include <stddef.h>
#include <stdint.h>

#define KC_SLOTS      20
#define KC_MAX_TASKS  18
#define KC_NAME       16            /* 15 characters + NUL */
#define KC_PROGRAM    128
#define KC_ARGS       96            /* as stored: each NUL-ended, then an empty one */
#define KC_BOOT       512
#define KC_TASKS      5120
#define KC_SECTOR     512
#define KC_PART_USED  (42 * KC_SECTOR)

enum { KC_REALTIME, KC_SOFT, KC_POWER };
enum { KF_NO, KF_YES, KF_SOME, KF_MACHINE, KF_SMP };
#define KC_RT_LEFTOVER  (1u << 0)
#define KC_RT_DEFER_IRQ (1u << 1)

struct kfeature {
    int bit;
    const char *key, *label, *group, *what;
    unsigned char support[3];       /* per family: KF_* */
    int wake_us, slows_cpu, saves;
};

struct kcstate {
    int n;
    const char *name;
    int wake_us;
    unsigned families;              /* bit per family */
};

/* The run part of one task: what myos-taskd starts for it after boot. */
struct kc_task {
    char name[KC_NAME];
    char program[KC_PROGRAM];
    char args[KC_ARGS];             /* space-separated, as typed */
    unsigned uid, gid;
    int start_at_boot, restart;
    unsigned restart_ms;
};

struct kc_config {
    int mode;
    char arch[8];                   /* i386, x86_64, arm, arm64 */
    int big;                        /* a big-endian target */
    char slots[KC_SLOTS][KC_NAME];  /* "os", "" (free) or a task's name */
    unsigned rt_flags, latency_budget_us;
    uint32_t power_on, power_seen;
    int cpu_min_pct, cpu_max_pct, epp, cstate_max, spindown_min;
    uint32_t generation;
    int nprog;
    struct kc_task prog[KC_MAX_TASKS];  /* the programs, by task name */
};

/* This computer: its processor family, and the savings it has. Each
   saving is KC_HAS (seen), KC_MAYBE (only the kernel can tell: switchable,
   checked when MyOS starts) or KC_LACKS (with the reason in why[]). */
enum { KC_LACKS, KC_HAS, KC_MAYBE };
struct kc_machine {
    int family;                     /* index into the families, or -1: not one we know */
    int virt;                       /* a virtual machine (CPUID's hypervisor bit) */
    char cpu[96];                   /* the model name */
    unsigned char have[32];         /* per feature bit: KC_HAS, KC_MAYBE or KC_LACKS */
    const char *why[32];            /* why not */
};

#define KC_MSGS 24
#define KC_MSG  176
struct kc_msgs {
    int nerr, nwarn, nnote;
    char err[KC_MSGS][KC_MSG], warn[KC_MSGS][KC_MSG], note[KC_MSGS][KC_MSG];
};

int kc_nfeatures(void);
const struct kfeature *kc_feature(int i);
int kc_ncstates(void);
const struct kcstate *kc_cstate(int i);
const char *kc_family_name(int family);

void kc_default(struct kc_config *c);
int kc_parse(const char *text, struct kc_config *c, char *err, size_t n);
char *kc_format(const struct kc_config *c);          /* malloc'd; the text form */
int kc_tasks(const struct kc_config *c, char names[KC_MAX_TASKS][KC_NAME]);
struct kc_task *kc_prog(struct kc_config *c, const char *name, int create);
int kc_task_slots(const struct kc_config *c, const char *name, int slots[KC_SLOTS]);
void kc_cadence(const int *slots, int n, char *out, size_t size);
int kc_good_name(const char *s);

void kc_detect(struct kc_machine *m);
/* KC_HAS, KC_MAYBE (switchable; the kernel checks) or KC_LACKS (why says why). */
int kc_feature_ok(const struct kc_machine *m, const struct kfeature *f, const char **why);
const struct kcstate *kc_soft_cstate(const struct kc_machine *m, unsigned budget_us);
int kc_cstate_ok(const struct kc_machine *m, int n);
void kc_check(const struct kc_config *c, const struct kc_machine *m, int check_files, struct kc_msgs *out);

void kc_encode(const struct kc_config *c, uint32_t generation, uint8_t boot[KC_BOOT], uint8_t tasks[KC_TASKS]);
int kc_decode(const uint8_t *boot, const uint8_t *tasks, struct kc_config *c, char *err, size_t n);

/* The myos-config partition on the disk "/" is on. */
int kc_find_partition(char *path, size_t n);
int kc_read_partition(const char *path, struct kc_config *c, char *which, char *err, size_t n);
int kc_write_partition(const char *path, const struct kc_config *c, char *which, char *err, size_t n);

#endif
