/* test_serial.c -- PTY-based tests for the serial reader.
 *
 * Creates PTY pairs to simulate serial ports, verifies:
 *   - ReadOnlySerial opens and reads data
 *   - O_RDONLY prevents writes
 *   - Non-blocking reads work with select/poll
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

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

int main(void)
{
    printf("=== test_serial ===\n");

    test_open_close();
    test_read_data();
    test_readonly();
    test_double_close();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
