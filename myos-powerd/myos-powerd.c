/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-powerd — closing the lid, the power button and the Start menu's
 * Sleep put the computer to sleep, as on a Mac (docs/SLEEP-PLAN.md).
 *
 * The kernel reports the lid on the "Lid Switch" input device (SW_LID) and
 * the power button on "Power Button" (KEY_POWER), as Linux names them
 * (drivers/acpi_button.c, /dev/input/event3 and event4). Anyone may ask for
 * sleep by writing "sleep" to the FIFO /run/myos/power (the Start menu
 * does). /etc/myos/power.conf says what the lid and the button do.
 *
 * Going to sleep:
 *   1. the desktop locks first, so the picture that comes back is the lock
 *      screen: SIGUSR1 to myos-idle, which starts myos-lock; myos-lock
 *      writes "locked" here once it covers the screen and holds the
 *      keyboard. We wait at most 1 s (no desktop running: no wait);
 *   2. "mem" to /sys/power/state. The kernel stops every other program
 *      and the clock tick, and the write returns when something wakes it:
 *      the lid opening, the power button, a key (kernel/suspend.c);
 *   3. SIGUSR2 to myos-idle: the screen comes on, with the lock up.
 * The press of the power button that wakes the computer comes as
 * KEY_WAKEUP, not KEY_POWER, so it doesn't put it back to sleep; whatever
 * else queued up while it slept is dropped too.
 *
 * Root, a runit service from boot: the login screen sleeps the same way.
 * Asleep in poll() all the time it isn't doing one of those.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "powerconf.h"

/* linux/input.h's few pieces we use (Alpine's sysroot has no linux-headers).
   The time is two longs: 16 bytes an event on i386, 24 on x86_64. */
struct input_event {
    unsigned long sec, usec;
    unsigned short type, code;
    int value;
};
#define EV_KEY       0x01
#define EV_SW        0x05
#define SW_LID       0x00
#define KEY_POWER    116
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)

#define STATE        "/sys/power/state"
#define BACKLIGHT    "/sys/class/backlight"
#define LOCK_WAIT_MS 1000

static int lid = -1, button = -1, fifo = -1;

static void say(const char *what)
{
    fprintf(stderr, "myos-powerd: %s\n", what);
}

/* The input device with this name, opened; -1 if there is none. */
static int find_input(const char *name)
{
    for (int i = 0; i < 32; i++) {
        char path[32], got[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGNAME(sizeof got), got) >= 0 && !strcmp(got, name))
            return fd;
        close(fd);
    }
    return -1;
}

/* Every process called `name` (its /proc/PID/comm), signalled. How many. */
static int signal_all(const char *name, int sig)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        char path[64], comm[32] = "";
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof path, "/proc/%.20s/comm", e->d_name);
        FILE *f = fopen(path, "re");
        if (f && fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (!strcmp(comm, name) && kill(atoi(e->d_name), sig) == 0)
                n++;
        }
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    return n;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* What came down the FIFO: 's' for "sleep", 'l' for "locked", 0 for
   nothing (or something else). One request a read: a burst of them
   (Sleep clicked twice) is one sleep. */
static int fifo_word(void)
{
    char buf[256];
    ssize_t n = read(fifo, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    return strstr(buf, "locked") ? 'l' : strstr(buf, "sleep") ? 's' : 0;
}

static void drain(int fd)
{
    char buf[512];
    while (fd >= 0 && read(fd, buf, sizeof buf) > 0)
        ;
}

/* --- the lid's "turn the display off" ------------------------------------------- */

static int read_int(const char *path)
{
    char t[16] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, t, sizeof t - 1);
    close(fd);
    return n > 0 ? atoi(t) : -1;
}

static void write_int(const char *path, int v)
{
    char t[16];
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    int n = snprintf(t, sizeof t, "%d\n", v);
    if (write(fd, t, (size_t)n) != n)
        say("could not set the brightness");
    close(fd);
}

static void display(int on)
{
    static int saved = -1;
    char path[300] = "";
    DIR *d = opendir(BACKLIGHT);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(path, sizeof path, BACKLIGHT "/%s/brightness", e->d_name);
            break;
        }
    if (d)
        closedir(d);
    if (!path[0])
        return;
    if (!on && saved < 0) {
        saved = read_int(path);
        write_int(path, 0);
    } else if (on && saved >= 0) {
        write_int(path, saved);
        saved = -1;
    }
}

/* --- sleep ------------------------------------------------------------------------ */

static void go_to_sleep(const char *why)
{
    char lock[8];
    if (strcmp(power_conf("lock_on_wake", "1", lock, sizeof lock), "0") != 0 &&
        signal_all("myos-idle", SIGUSR1) > 0) {
        long end = now_ms() + LOCK_WAIT_MS;
        int locked = 0;
        while (!locked && now_ms() < end) {
            struct pollfd p = { fifo, POLLIN, 0 };
            if (poll(&p, 1, (int)(end - now_ms())) > 0)
                locked = fifo_word() == 'l';
        }
        if (!locked)
            say("the desktop did not lock in 1 s; sleeping anyway");
    }
    fprintf(stderr, "myos-powerd: sleep (%s)\n", why);
    int fd = open(STATE, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, "mem\n", 4) != 4)
        fprintf(stderr, "myos-powerd: %s: %s\n", STATE, strerror(errno));
    if (fd >= 0)
        close(fd);
    /* Awake. Whatever queued up meanwhile is old news. */
    drain(lid);
    drain(button);
    drain(fifo);
    signal_all("myos-idle", SIGUSR2);
}

static void poweroff(void)
{
    say("the power button: turning off");
    if (fork() == 0) {
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        _exit(127);
    }
}

/* --- the events ------------------------------------------------------------------- */

static void lid_moved(int shut)
{
    char what[16];
    power_conf("lid_close", "sleep", what, sizeof what);
    if (!strcmp(what, "display_off"))
        display(!shut);
    else if (shut && !strcmp(what, "sleep"))
        go_to_sleep("the lid closed");
}

static void button_pressed(void)
{
    char what[16];
    power_conf("power_button", "sleep", what, sizeof what);
    if (!strcmp(what, "sleep"))
        go_to_sleep("the power button");
    else if (!strcmp(what, "poweroff"))
        poweroff();
}

static void input(int fd)
{
    struct input_event ev[16];
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) >= (ssize_t)sizeof ev[0])
        for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
            if (fd == lid && ev[i].type == EV_SW && ev[i].code == SW_LID)
                lid_moved(ev[i].value);
            else if (fd == button && ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1)
                button_pressed();
        }
}

int main(void)
{
    signal(SIGCHLD, SIG_IGN);           /* poweroff's child: nobody waits for it */
    lid = find_input("Lid Switch");
    button = find_input("Power Button");
    /* The FIFO: anyone may write, only we read. Opened read-write so it
       always has a writer (us): poll() then never sees an end of file. */
    mkdir("/run/myos", 0755);
    unlink(POWER_FIFO);
    if (mkfifo(POWER_FIFO, 0622) < 0 || chmod(POWER_FIFO, 0622) < 0 ||
        (fifo = open(POWER_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) {
        fprintf(stderr, "myos-powerd: %s: %s\n", POWER_FIFO, strerror(errno));
        return 1;
    }
    if (lid < 0)
        say("no lid");
    if (button < 0)
        say("no power button");

    for (;;) {
        struct pollfd p[3] = { { fifo, POLLIN, 0 }, { lid, POLLIN, 0 }, { button, POLLIN, 0 } };
        if (poll(p, 3, -1) < 0) {
            if (errno == EINTR)
                continue;
            say("poll failed");
            return 1;
        }
        if (p[0].revents & POLLIN && fifo_word() == 's')
            go_to_sleep("asked to");
        if (p[1].revents & POLLIN)
            input(lid);
        if (p[2].revents & POLLIN)
            input(button);
    }
}
