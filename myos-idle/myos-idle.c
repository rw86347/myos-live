/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-idle — dim the screen, then turn it off, when nobody uses it.
 *
 * How long depends on the battery slider (myos-battery, the platform
 * profile) and on the charger:
 *
 *   slider                 on battery   plugged in
 *   Best battery life        1 min        2 min
 *   Balanced                 5 min       10 min
 *   Best performance        15 min       30 min
 *
 * 20 seconds before the screen goes off it dims, as a warning: touch
 * anything and it comes back. A computer without a battery counts as
 * plugged in.
 *
 * Dimming is the panel's backlight (/sys/class/backlight, the firmware's
 * _BCM: drivers/backlight.c), down to 30%; off is its lowest level, X's
 * blank screen, and DPMS "off" where the display driver has it. Without a
 * backlight the dim is a half-dark window over everything, when the
 * compositor is on (without it a window can't be half transparent: then
 * there is no dim, only off).
 *
 * Nothing here polls. The X server counts how long since the last key or
 * pointer move (the SYNC extension's IDLETIME counter) and sends an event
 * when it crosses a value we set (an alarm). The first alarm is always at
 * the shortest dim time there is (40 s); when it fires, the slider and the
 * charger are read and, if this setting waits longer, the alarm moves
 * there. So a change of the slider or the charger needs no message: it is
 * read when it matters. A second alarm fires when the idle time drops
 * again (someone touched something) and brings the screen back.
 *
 * Locking and sleep (docs/SLEEP-PLAN.md), as a Mac does:
 *   - when the screen goes off it locks (myos-lock), unless power.conf's
 *     lock_on_display_off=0;
 *   - on battery, sleep_after_display_off_minutes (1) after that it asks
 *     myos-powerd for sleep. Plugged in it doesn't, but looks again each
 *     minute, so pulling the plug counts;
 *   - myos-powerd, about to sleep, sends SIGUSR1: start myos-lock, which
 *     tells powerd once it covers the screen; and SIGUSR2 when the
 *     computer woke: the screen comes on, the lock is what it shows.
 * The signals reach the X event loop through a pipe, so it can poll both.
 *
 * Started by /etc/myos/xsession; one sleeping process.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/sync.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "powerconf.h"

#define PROFILE "/sys/firmware/acpi/platform_profile"
#define SUPPLY  "/sys/class/power_supply"
#define BACKLIGHT "/sys/class/backlight"
#define DIM_MS  20000
#define DIM_PCT 30

static const int off_s[3] = { 60, 300, 900 };   /* low-power, balanced, performance */
#define FIRST_MS (off_s[0] * 1000 - DIM_MS)

static Display *dpy;
static XSyncCounter idle;
static XSyncAlarm wait_alarm, back_alarm;
static Window shade;                    /* the half-dark window, or None */
static char bl_path[300];               /* .../brightness, or "" */
static int saved = -1;                  /* the brightness before dimming */
static int dpms;
static int sig_pipe[2];
enum { ACTIVE, DIMMED, OFF } state;

/* --- what the settings say -------------------------------------------------- */

static int read_text(const char *path, char *out, int size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int n = (int)read(fd, out, (size_t)size - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    return n;
}

static int profile(void)
{
    char t[32];
    if (read_text(PROFILE, t, sizeof t) < 0)
        return 1;
    return !strncmp(t, "low-power", 9) ? 0 : !strncmp(t, "performance", 11) ? 2 : 1;
}

/* On the charger, or no battery at all. */
static int plugged_in(void)
{
    DIR *d = opendir(SUPPLY);
    int mains = 0, battery = 0;
    struct dirent *e;
    char path[300], t[32];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, SUPPLY "/%s/type", e->d_name);
        if (read_text(path, t, sizeof t) < 0)
            continue;
        if (!strncmp(t, "Battery", 7))
            battery = 1;
        snprintf(path, sizeof path, SUPPLY "/%s/online", e->d_name);
        if (!strncmp(t, "Mains", 5) && read_text(path, t, sizeof t) > 0 && t[0] == '1')
            mains = 1;
    }
    if (d)
        closedir(d);
    return mains || !battery;
}

static int off_ms(void)
{
    return off_s[profile()] * 1000 * (plugged_in() ? 2 : 1);
}

/* --- the screen ---------------------------------------------------------------- */

static void find_backlight(void)
{
    DIR *d = opendir(BACKLIGHT);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(bl_path, sizeof bl_path, BACKLIGHT "/%s/brightness", e->d_name);
            break;
        }
    if (d)
        closedir(d);
}

static int brightness(void)
{
    char t[16];
    return bl_path[0] && read_text(bl_path, t, sizeof t) > 0 ? atoi(t) : -1;
}

static int set_brightness(int v)
{
    char t[16];
    int fd = bl_path[0] ? open(bl_path, O_WRONLY | O_CLOEXEC) : -1;
    if (fd < 0)
        return -1;
    int n = snprintf(t, sizeof t, "%d\n", v);
    int ok = write(fd, t, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}

/* A screen-sized window, black at half strength, that clicks go through.
   Only with a compositor: it needs a 32-bit (ARGB) window. */
static void make_shade(void)
{
    int scr = DefaultScreen(dpy);
    char sel[32];
    snprintf(sel, sizeof sel, "_NET_WM_CM_S%d", scr);
    XVisualInfo vi;
    if (XGetSelectionOwner(dpy, XInternAtom(dpy, sel, False)) == None ||
        !XMatchVisualInfo(dpy, scr, 32, TrueColor, &vi))
        return;
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = 0x80000000,     /* alpha 0x80, black (premultiplied) */
        .border_pixel = 0,
        .colormap = XCreateColormap(dpy, RootWindow(dpy, scr), vi.visual, AllocNone),
    };
    shade = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)DisplayWidth(dpy, scr),
                          (unsigned)DisplayHeight(dpy, scr), 0, 32, InputOutput, vi.visual,
                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap, &a);
    XShapeCombineRectangles(dpy, shade, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
}

static void dim(void)
{
    saved = brightness();
    if (saved > 0 && set_brightness(saved * DIM_PCT / 100) == 0)
        return;
    saved = -1;
    if (!shade)
        make_shade();
    if (shade)
        XMapRaised(dpy, shade);
}

/* --- locking and sleep --------------------------------------------------------- */

/* myos-lock, in the background. With --ack it writes "locked" to
   myos-powerd once it holds the screen; one already up just says so. */
static void lock(int ack)
{
    if (fork() == 0) {
        execl("/usr/bin/myos-lock", "myos-lock", ack ? "--ack" : (char *)NULL, (char *)NULL);
        _exit(127);
    }
}

static void ask_sleep(void)
{
    int fd = open(POWER_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);  /* ENXIO: no powerd */
    if (fd >= 0) {
        if (write(fd, "sleep\n", 6) != 6)
            fprintf(stderr, "myos-idle: myos-powerd did not take the request\n");
        close(fd);
    }
}

static void screen_off(void)
{
    if (saved >= 0)
        set_brightness(0);
    if (shade)
        XUnmapWindow(dpy, shade);
    XForceScreenSaver(dpy, ScreenSaverActive);      /* black */
    if (dpms)
        DPMSForceLevel(dpy, DPMSModeOff);           /* and off, where the driver can */
    if (power_conf_int("lock_on_display_off", 1))
        lock(0);
}

static void screen_back(void)
{
    if (saved >= 0)
        set_brightness(saved);
    saved = -1;
    if (shade)
        XUnmapWindow(dpy, shade);
    if (dpms)
        DPMSForceLevel(dpy, DPMSModeOn);
    XForceScreenSaver(dpy, ScreenSaverReset);
}

/* --- the alarms ------------------------------------------------------------------ */

/* Fires when the idle time is at least ms (positive), or below it (not):
   at once if it already is. Once, then it waits to be set again. */
static XSyncAlarm alarm_at(XSyncAlarm a, int ms, int positive)
{
    XSyncAlarmAttributes at;
    at.trigger.counter = idle;
    at.trigger.value_type = XSyncAbsolute;
    at.trigger.test_type = positive ? XSyncPositiveComparison : XSyncNegativeComparison;
    XSyncIntToValue(&at.trigger.wait_value, ms);
    XSyncIntToValue(&at.delta, 0);
    at.events = True;
    unsigned long flags = XSyncCACounter | XSyncCAValueType | XSyncCATestType | XSyncCAValue |
                          XSyncCADelta | XSyncCAEvents;
    if (a == None)
        return XSyncCreateAlarm(dpy, flags, &at);
    XSyncChangeAlarm(dpy, a, flags, &at);
    return a;
}

/* The idle time reached `at` ms. */
static void waited(int at)
{
    int sleep_ms = power_conf_int("sleep_after_display_off_minutes", 1) * 60000;
    if (state == DIMMED) {
        state = OFF;
        screen_off();
        if (sleep_ms > 0)
            wait_alarm = alarm_at(wait_alarm, at + sleep_ms, 1);
        return;
    }
    if (state == OFF) {                 /* the screen has been off a while */
        if (sleep_ms > 0 && !plugged_in())
            ask_sleep();
        else if (sleep_ms > 0)
            wait_alarm = alarm_at(wait_alarm, at + 60000, 1);  /* look again in a minute */
        return;
    }
    int off = off_ms();
    if (off - DIM_MS > at)
        wait_alarm = alarm_at(wait_alarm, off - DIM_MS, 1);     /* this setting waits longer */
    else {
        state = DIMMED;
        dim();
        wait_alarm = alarm_at(wait_alarm, at + DIM_MS, 1);
    }
    back_alarm = alarm_at(back_alarm, at, 0);   /* any key or move: below `at` again */
}

static void touched(void)
{
    if (state != ACTIVE)
        screen_back();
    state = ACTIVE;
    wait_alarm = alarm_at(wait_alarm, FIRST_MS, 1);
}

/* Awake after sleep: the screen on, the idle time from zero (X's clock
   left the sleep out, so it would carry on where it stopped), and the
   lock screen told, so its clock shows the time now, not at sleep. */
static void woke(void)
{
    XForceScreenSaver(dpy, ScreenSaverReset);
    touched();
    Window lock_win = XGetSelectionOwner(dpy, XInternAtom(dpy, "_MYOS_LOCK", False));
    if (lock_win != None) {
        XEvent m = { .xclient = { .type = ClientMessage, .window = lock_win, .format = 32,
                                  .message_type = XInternAtom(dpy, "_MYOS_LOCK", False) } };
        XSendEvent(dpy, lock_win, False, NoEventMask, &m);
    }
}

static void on_signal(int sig)
{
    int e = errno;
    ssize_t r = write(sig_pipe[1], sig == SIGUSR1 ? "l" : "w", 1);    /* full: one waits already */
    (void)r;
    errno = e;
}

int main(void)
{
    int ev, err, major, minor, n;
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "myos-idle: no X display\n");
        return 1;
    }
    if (!XSyncQueryExtension(dpy, &ev, &err) || !XSyncInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "myos-idle: the X server has no SYNC extension\n");
        return 1;
    }
    XSyncSystemCounter *c = XSyncListSystemCounters(dpy, &n);
    for (int i = 0; i < n; i++)
        if (!strcmp(c[i].name, "IDLETIME"))
            idle = c[i].counter;
    if (c)
        XSyncFreeSystemCounterList(c);
    if (!idle) {
        fprintf(stderr, "myos-idle: the X server keeps no idle time\n");
        return 1;
    }
    find_backlight();
    /* We decide when: X's own screen saver (10 minutes by default) and
       DPMS timers off; DPMS stays enabled so it can be forced. */
    XSetScreenSaver(dpy, 0, 0, PreferBlanking, AllowExposures);
    int dev, derr;
    if (DPMSQueryExtension(dpy, &dev, &derr) && DPMSCapable(dpy)) {
        dpms = 1;
        DPMSSetTimeouts(dpy, 0, 0, 0);
        DPMSEnable(dpy);
    }
    wait_alarm = alarm_at(None, FIRST_MS, 1);
    back_alarm = None;
    XSync(dpy, False);

    if (fork() > 0)
        _exit(0);
    setsid();
    if (pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        fprintf(stderr, "myos-idle: pipe: %s\n", strerror(errno));
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);           /* myos-lock: nobody waits for it */

    for (;;) {
        while (XPending(dpy)) {         /* (also sends what we drew) */
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type != ev + XSyncAlarmNotify)
                continue;
            XSyncAlarmNotifyEvent *a = (XSyncAlarmNotifyEvent *)&e;
            if (a->state == XSyncAlarmDestroyed)
                continue;
            if (a->alarm == wait_alarm)
                waited(XSyncValueLow32(a->counter_value));
            else if (a->alarm == back_alarm)
                touched();
        }
        struct pollfd p[2] = { { ConnectionNumber(dpy), POLLIN, 0 }, { sig_pipe[0], POLLIN, 0 } };
        if (poll(p, 2, -1) < 0 && errno != EINTR)
            return 1;
        char c[8];
        ssize_t n = p[1].revents & POLLIN ? read(sig_pipe[0], c, sizeof c) : 0;
        for (ssize_t i = 0; i < n; i++)
            if (c[i] == 'l')
                lock(1);
            else
                woke();
    }
}
