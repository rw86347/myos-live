/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * myos-console DEVICE — point /dev/console at DEVICE (ioctl TIOCCONS), as
 * BusyBox's setconsole does; the live disk has no BusyBox. /sbin/init runs
 * it with /dev/ttyS0 before runit, so the boot screen shows the kernel's
 * steps and "MyOS is up." and nothing else: runit's "enter stage" lines,
 * the services' output and the login messages go to the serial port.
 * `myos-console /dev/console` puts it back.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("usage: myos-console DEVICE\n", stderr);
        return 2;
    }
    int fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0 || ioctl(fd, TIOCCONS) < 0) {
        perror(argv[1]);
        return 1;
    }
    close(fd);
    return 0;
}
