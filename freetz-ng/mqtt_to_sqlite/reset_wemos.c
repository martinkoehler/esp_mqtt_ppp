// reset_wemos.c
// Hard reset a WeMos D1/ESP8266 by toggling DTR/RTS on a USB-serial adapter.
// Usage: ./reset_wemos [/dev/ttyUSB0] [pulse_ms]
// Default device: /dev/ttyUSB0, default pulse: 120 ms

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/serial.h>

#ifndef TIOCMGET
#  include <sys/ttycom.h>
#endif

#ifndef TIOCM_DTR
#  define TIOCM_DTR 0x002
#endif
#ifndef TIOCM_RTS
#  define TIOCM_RTS 0x004
#endif
#ifndef TIOCMGET
#  define TIOCMGET 0x5415
#endif
#ifndef TIOCMSET
#  define TIOCMSET 0x5418
#endif

static void msleep(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000UL;
    nanosleep(&ts, NULL);
}

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    const char *dev = (argc >= 2) ? argv[1] : "/dev/ttyUSB0";
    unsigned pulse_ms = (argc >= 3) ? (unsigned)strtoul(argv[2], NULL, 10) : 120;

    // Open without becoming controlling TTY; read/write not strictly needed but harmless.
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) die("open");

    // Clear O_NONBLOCK after open so ioctls behave normally.
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    // Get current modem control state
    int mstate = 0;
    if (ioctl(fd, TIOCMGET, &mstate) < 0) die("ioctl(TIOCMGET)");

    // Step 1: Deassert DTR to keep GPIO0 HIGH (normal run mode)
    mstate &= ~TIOCM_DTR;
    if (ioctl(fd, TIOCMSET, &mstate) < 0) die("ioctl(TIOCMSET DTR off)");
    msleep(50);

    // Step 2: Pulse RTS to reset (assert -> delay -> deassert)
    // Asserting RTS (through the inverter) pulls RESET low.
    mstate |= TIOCM_RTS; // assert
    if (ioctl(fd, TIOCMSET, &mstate) < 0) die("ioctl(TIOCMSET RTS on)");
    msleep(pulse_ms);

    mstate &= ~TIOCM_RTS; // deassert
    if (ioctl(fd, TIOCMSET, &mstate) < 0) die("ioctl(TIOCMSET RTS off)");

    // Small settle delay (optional)
    msleep(50);

    close(fd);
    return 0;
}

