/* test_serial.c
 *
 * Copyright (C) 2025 wolfSSL Inc.
 *
 * This file is part of uart-monitor.
 *
 * uart-monitor is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * uart-monitor is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#ifndef __APPLE__
#include <poll.h>
#include <sys/inotify.h>
#endif

#include "../src/serial.h"
#include "../src/util.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-40s ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static int
create_pty_pair(int *master_fd, char *slave_path, size_t sz)
{
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0)
        return -1;

    char *name = ttyname(slave);
    if (!name) {
        close(master);
        close(slave);
        return -1;
    }

    strlcpy_safe(slave_path, name, sz);
    close(slave); /* monitor will open by path */
    *master_fd = master;
    return 0;
}

static void
test_open_close(void)
{
    TEST("serial_open/close with PTY");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    int ret = serial_open(&sp, slave_path, B115200);
    if (ret < 0) {
        FAIL("serial_open failed");
        close(master);
        return;
    }

    if (sp.fd < 0) {
        FAIL("fd is negative after open");
        close(master);
        return;
    }

    serial_close(&sp);
    if (sp.fd != -1) {
        FAIL("fd not -1 after close");
        close(master);
        return;
    }

    close(master);
    PASS();
}

static void
test_read_data(void)
{
    TEST("read data through PTY");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    if (serial_open(&sp, slave_path, B115200) < 0) {
        FAIL("serial_open failed");
        close(master);
        return;
    }

    /* write test data through master side */
    const char *test_msg = "Hello UART\r\n";
    ssize_t nw = write(master, test_msg, strlen(test_msg));
    if (nw < 0) {
        FAIL("write to master failed");
        serial_close(&sp);
        close(master);
        return;
    }

    /* wait for data to be readable */
    fd_set rfds;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(sp.fd, &rfds);
    int ready = select(sp.fd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) {
        FAIL("select timeout, no data");
        serial_close(&sp);
        close(master);
        return;
    }

    char buf[256];
    ssize_t nr = read(sp.fd, buf, sizeof(buf) - 1);
    if (nr <= 0) {
        FAIL("read returned no data");
        serial_close(&sp);
        close(master);
        return;
    }

    buf[nr] = '\0';

    /* data should contain our test message (PTY may add/strip chars) */
    if (strstr(buf, "Hello UART") == NULL) {
        FAIL("data mismatch");
        serial_close(&sp);
        close(master);
        return;
    }

    serial_close(&sp);
    close(master);
    PASS();
}

static void
test_readonly(void)
{
    TEST("O_RDONLY prevents write()");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    if (serial_open(&sp, slave_path, B115200) < 0) {
        FAIL("serial_open failed");
        close(master);
        return;
    }

    /* verify we opened read-only by checking flags */
    int flags = fcntl(sp.fd, F_GETFL);
    int accmode = flags & O_ACCMODE;
    if (accmode != O_RDONLY) {
        FAIL("not opened O_RDONLY");
        serial_close(&sp);
        close(master);
        return;
    }

    /* attempt write should fail with EBADF */
    ssize_t nw = write(sp.fd, "x", 1);
    if (nw >= 0) {
        FAIL("write() succeeded on read-only fd");
        serial_close(&sp);
        close(master);
        return;
    }

    serial_close(&sp);
    close(master);
    PASS();
}

static void
test_double_close(void)
{
    TEST("serial_close is safe to call twice");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    serial_open(&sp, slave_path, B115200);

    serial_close(&sp);
    serial_close(&sp); /* should not crash */

    close(master);
    PASS();
}

static void
test_proxy_open_close(void)
{
    TEST("serial_open_proxy creates PTY pair");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    int ret = serial_open_proxy(&sp, slave_path, B115200);
    if (ret < 0) {
        FAIL("serial_open_proxy failed");
        close(master);
        return;
    }

    if (sp.fd < 0) {
        FAIL("fd is negative");
        close(master);
        return;
    }
    if (sp.pty_master < 0) {
        FAIL("pty_master is negative");
        serial_close(&sp);
        close(master);
        return;
    }
    if (sp.pty_path[0] == '\0') {
        FAIL("pty_path is empty");
        serial_close(&sp);
        close(master);
        return;
    }

    serial_close(&sp);
    if (sp.fd != -1 || sp.pty_master != -1) {
        FAIL("fds not -1 after close");
        close(master);
        return;
    }

    close(master);
    PASS();
}

static void
test_proxy_bidirectional(void)
{
    TEST("proxy forwards data bidirectionally");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    if (serial_open_proxy(&sp, slave_path, B115200) < 0) {
        FAIL("serial_open_proxy failed");
        close(master);
        return;
    }

    /* 1. Write through the "real port" master -> read from serial fd */
    const char *msg = "from_board\n";
    ssize_t nw = write(master, msg, strlen(msg));
    (void)nw;
    usleep(50000);

    char buf[256];
    fd_set rfds;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(sp.fd, &rfds);
    if (select(sp.fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        FAIL("no data from serial fd");
        serial_close(&sp);
        close(master);
        return;
    }

    ssize_t nr = read(sp.fd, buf, sizeof(buf) - 1);
    if (nr <= 0 || strstr(buf, "from_board") == NULL) {
        FAIL("serial fd read mismatch");
        serial_close(&sp);
        close(master);
        return;
    }

    /* 2. Write to PTY slave (user side) -> should be readable from pty_master */
    int pty_slave = open(sp.pty_path, O_RDWR | O_NOCTTY);
    if (pty_slave < 0) {
        FAIL("cannot open PTY slave");
        serial_close(&sp);
        close(master);
        return;
    }

    const char *cmd = "user_cmd\n";
    nw = write(pty_slave, cmd, strlen(cmd));
    (void)nw;
    usleep(50000);

    tv.tv_sec = 1; tv.tv_usec = 0;
    FD_ZERO(&rfds);
    FD_SET(sp.pty_master, &rfds);
    if (select(sp.pty_master + 1, &rfds, NULL, NULL, &tv) <= 0) {
        FAIL("no data from pty_master");
        close(pty_slave);
        serial_close(&sp);
        close(master);
        return;
    }

    nr = read(sp.pty_master, buf, sizeof(buf) - 1);
    if (nr <= 0) {
        FAIL("pty_master read failed");
        close(pty_slave);
        serial_close(&sp);
        close(master);
        return;
    }
    buf[nr] = '\0';
    if (strstr(buf, "user_cmd") == NULL) {
        FAIL("pty_master read mismatch");
        close(pty_slave);
        serial_close(&sp);
        close(master);
        return;
    }

    close(pty_slave);
    serial_close(&sp);
    close(master);
    PASS();
}

/* Regression: a client (screen) that sets TIOCEXCL on the proxy PTY
 * slave and exits must not leave the pty permanently EBUSY. The daemon
 * holds the pty pair open forever, so the kernel never clears the lock
 * on the client's last close; serial_pty_clear_excl() must heal it. */
static void
test_pty_excl_heal(void)
{
    TEST("stale TIOCEXCL heal on PTY slave");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    if (serial_open_proxy(&sp, slave_path, B115200) < 0) {
        FAIL("serial_open_proxy failed");
        close(master);
        return;
    }

    /* client attaches, sets exclusive mode (screen does), exits */
    int client = open(sp.pty_path, O_RDWR | O_NOCTTY);
    if (client < 0) {
        FAIL("cannot open PTY slave as client");
        serial_close(&sp);
        close(master);
        return;
    }
    if (ioctl(client, TIOCEXCL) < 0) {
        FAIL("TIOCEXCL failed");
        close(client);
        serial_close(&sp);
        close(master);
        return;
    }
    close(client);

    /* the bug: reconnect fails EBUSY (Linux kernel contract; BSD ptys
     * may behave differently, tolerate either there) */
    int reopen = open(sp.pty_path, O_RDWR | O_NOCTTY);
#ifndef __APPLE__
    if (reopen >= 0 || errno != EBUSY) {
        FAIL("expected EBUSY reopen after client TIOCEXCL");
        if (reopen >= 0)
            close(reopen);
        serial_close(&sp);
        close(master);
        return;
    }
#else
    if (reopen >= 0)
        close(reopen);
#endif

    /* the fix: clear via the keeper slave fd */
    int rc = serial_pty_clear_excl(&sp);
#ifndef __APPLE__
    if (rc != 1) {
        FAIL("serial_pty_clear_excl did not report a clear");
        serial_close(&sp);
        close(master);
        return;
    }
#else
    if (rc < 0) {
        FAIL("serial_pty_clear_excl failed");
        serial_close(&sp);
        close(master);
        return;
    }
#endif

    reopen = open(sp.pty_path, O_RDWR | O_NOCTTY);
    if (reopen < 0) {
        FAIL("reopen still fails after heal");
        serial_close(&sp);
        close(master);
        return;
    }

    close(reopen);
    serial_close(&sp);
    close(master);
    PASS();
}

#ifndef __APPLE__
/* Kernel-contract guard for the monitor wiring: inotify on a /dev/pts/N
 * node must deliver a close event when a client exits, so the daemon can
 * clear a stale TIOCEXCL the moment it happens. */
static void
test_pty_close_inotify(void)
{
    TEST("inotify close event on PTY slave");
    int master;
    char slave_path[256];
    if (create_pty_pair(&master, slave_path, sizeof(slave_path)) < 0) {
        FAIL("cannot create PTY pair");
        return;
    }

    serial_port_t sp;
    if (serial_open_proxy(&sp, slave_path, B115200) < 0) {
        FAIL("serial_open_proxy failed");
        close(master);
        return;
    }

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        FAIL("inotify_init1 failed");
        serial_close(&sp);
        close(master);
        return;
    }
    int wd = inotify_add_watch(ifd, sp.pty_path,
                               IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
    if (wd < 0) {
        FAIL("inotify_add_watch failed");
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }

    /* client attach, TIOCEXCL, exit -- must produce a close event */
    int client = open(sp.pty_path, O_RDWR | O_NOCTTY);
    if (client < 0 || ioctl(client, TIOCEXCL) < 0) {
        FAIL("client open/TIOCEXCL failed");
        if (client >= 0)
            close(client);
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }
    close(client);

    struct pollfd pfd;
    pfd.fd = ifd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 1000) <= 0) {
        FAIL("no inotify event within 1s");
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }

    char buf[4096];
    ssize_t n = read(ifd, buf, sizeof(buf));
    int got_close = 0;
    ssize_t off = 0;
    while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
        const struct inotify_event *ie =
            (const struct inotify_event *)(buf + off);
        if (ie->wd == wd &&
            (ie->mask & (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)))
            got_close = 1;
        off += (ssize_t)(sizeof(struct inotify_event) + ie->len);
    }
    if (!got_close) {
        FAIL("no close event for watched pty");
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }

    /* end-to-end: what the daemon's handler does on that event */
    if (serial_pty_clear_excl(&sp) != 1) {
        FAIL("clear after event did not report a clear");
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }
    int reopen = open(sp.pty_path, O_RDWR | O_NOCTTY);
    if (reopen < 0) {
        FAIL("reopen after event-driven heal failed");
        close(ifd);
        serial_close(&sp);
        close(master);
        return;
    }

    close(reopen);
    close(ifd);
    serial_close(&sp);
    close(master);
    PASS();
}
#endif /* !__APPLE__ */

int main(void)
{
    printf("=== test_serial ===\n");

    test_open_close();
    test_read_data();
    test_readonly();
    test_double_close();
    test_proxy_open_close();
    test_proxy_bidirectional();
    test_pty_excl_heal();
#ifndef __APPLE__
    test_pty_close_inotify();
#endif

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
