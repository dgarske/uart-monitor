/* test_monitor.c -- Integration tests using PTY pairs.
 *
 * Tests log file creation, timestamped output, session management,
 * and multi-port logging.
 */
#include <assert.h>
#include <errno.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/log.h"
#include "../src/serial.h"
#include "../src/util.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-40s ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static void
test_log_create_session(void)
{
    TEST("log_create_session");
    char session_path[512];
    if (log_create_session(session_path, sizeof(session_path)) < 0) {
        FAIL("log_create_session failed");
        return;
    }

    struct stat st;
    if (stat(session_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        FAIL("session directory not created");
        return;
    }

    /* check that latest symlink exists */
    char latest[512];
    snprintf(latest, sizeof(latest), "%s/latest", LOG_BASE_DIR);
    char target[512];
    ssize_t n = readlink(latest, target, sizeof(target) - 1);
    if (n < 0) {
        FAIL("latest symlink missing");
        return;
    }
    target[n] = '\0';
    if (strncmp(target, "session-", 8) != 0) {
        FAIL("latest symlink doesn't point to session");
        return;
    }
    PASS();
}

static void
test_log_write_timestamps(void)
{
    TEST("log_write adds timestamps");
    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    if (log_open(&lf, session_path, "test_port", "Test header\n") < 0) {
        FAIL("log_open failed");
        return;
    }

    /* write some data */
    log_write(&lf, "Hello world\n", 12);
    log_write(&lf, "Second line\n", 12);
    log_close(&lf);

    /* read back and verify timestamps */
    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) {
        FAIL("cannot read log file");
        return;
    }

    char line[512];
    int found_hello = 0;
    int found_timestamp = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Hello world"))
            found_hello = 1;
        if (line[0] == '[' && line[5] == '-')
            found_timestamp = 1;
    }
    fclose(fp);

    if (!found_hello) { FAIL("data not in log"); return; }
    if (!found_timestamp) { FAIL("no timestamps in log"); return; }
    PASS();
}

static void
test_log_marker(void)
{
    TEST("log_marker writes separator");
    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    log_open(&lf, session_path, "test_marker", NULL);

    log_write(&lf, "before\n", 7);
    log_marker(&lf, "PORT YIELDED");
    log_write(&lf, "after\n", 6);
    log_close(&lf);

    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) { FAIL("cannot read log"); return; }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "PORT YIELDED"))
            found = 1;
    }
    fclose(fp);

    if (!found) { FAIL("marker not in log"); return; }
    PASS();
}

static void
test_log_crlf_handling(void)
{
    TEST("log_write handles \\r\\n correctly");
    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    log_open(&lf, session_path, "test_crlf", NULL);

    log_write(&lf, "line1\r\nline2\r\n", 14);
    log_close(&lf);

    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) { FAIL("cannot read log"); return; }

    char line[512];
    int linecount = 0;
    int found_line1 = 0;
    int found_line2 = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "line1")) { found_line1 = 1; linecount++; }
        if (strstr(line, "line2")) { found_line2 = 1; linecount++; }
    }
    fclose(fp);

    if (!found_line1 || !found_line2) {
        FAIL("missing lines");
        return;
    }
    PASS();
}

static void
test_log_prune(void)
{
    TEST("log_prune_sessions keeps N newest");

    /* use far-future timestamps to ensure these sort AFTER any real sessions */
    /* create several session dirs */
    for (int i = 0; i < 5; i++) {
        char path[512];
        snprintf(path, sizeof(path),
                 "%s/session-20991231-00000%d", LOG_BASE_DIR, i);
        mkdirp(path);
        /* create a dummy file so dir isn't empty */
        char fpath[600];
        snprintf(fpath, sizeof(fpath), "%s/dummy.log", path);
        FILE *fp = fopen(fpath, "w");
        if (fp) { fprintf(fp, "test\n"); fclose(fp); }
    }

    /* prune to keep only 3 total (test sessions are the newest) */
    log_prune_sessions(3);

    /* the 3 newest test sessions should survive */
    struct stat st;
    char p4[512];
    snprintf(p4, sizeof(p4), "%s/session-20991231-000004", LOG_BASE_DIR);

    if (stat(p4, &st) < 0) {
        FAIL("newest session was pruned");
        /* cleanup anyway */
        for (int i = 0; i < 5; i++) {
            char path[512], fpath[600];
            snprintf(path, sizeof(path),
                     "%s/session-20991231-00000%d", LOG_BASE_DIR, i);
            snprintf(fpath, sizeof(fpath), "%s/dummy.log", path);
            unlink(fpath);
            rmdir(path);
        }
        return;
    }
    PASS();

    /* cleanup test sessions */
    for (int i = 0; i < 5; i++) {
        char path[512], fpath[600];
        snprintf(path, sizeof(path),
                 "%s/session-20991231-00000%d", LOG_BASE_DIR, i);
        snprintf(fpath, sizeof(fpath), "%s/dummy.log", path);
        unlink(fpath);
        rmdir(path);
    }
}

static void
test_pty_to_log(void)
{
    TEST("PTY -> serial_read -> log_write");

    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        FAIL("openpty failed");
        return;
    }
    char *slave_name = ttyname(slave);
    close(slave);

    serial_port_t sp;
    if (serial_open(&sp, slave_name, B115200) < 0) {
        FAIL("serial_open failed");
        close(master);
        return;
    }

    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    log_open(&lf, session_path, "pty_test", "PTY Integration Test\n");

    /* simulate board output */
    const char *msg = "U-Boot 2024.01\r\nDRAM: 2 GiB\r\n";
    ssize_t nw = write(master, msg, strlen(msg));
    (void)nw;
    usleep(100000); /* 100ms settle */

    /* read from serial and write to log */
    char buf[4096];
    fd_set rfds;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(sp.fd, &rfds);

    if (select(sp.fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        ssize_t nr = read(sp.fd, buf, sizeof(buf));
        if (nr > 0)
            log_write(&lf, buf, (size_t)nr);
    }

    log_close(&lf);
    serial_close(&sp);
    close(master);

    /* verify log contents */
    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) { FAIL("cannot read log"); return; }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "U-Boot"))
            found = 1;
    }
    fclose(fp);

    if (!found) { FAIL("U-Boot not in log"); return; }
    PASS();
}

static void
test_label_log_filename(void)
{
    TEST("log file uses label as filename");
    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    if (log_open(&lf, session_path, "POLARFIRE_SOC_UART0",
                 "Test label\n") < 0) {
        FAIL("log_open with label failed");
        return;
    }

    /* verify the filepath uses the label */
    if (strstr(lf.filepath, "POLARFIRE_SOC_UART0.log") == NULL) {
        FAIL("filepath doesn't contain label");
        log_close(&lf);
        return;
    }

    log_write(&lf, "label test data\n", 16);
    log_close(&lf);

    /* verify file exists and has content */
    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) { FAIL("cannot read label log"); return; }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "label test data"))
            found = 1;
    }
    fclose(fp);

    if (!found) { FAIL("data not in label log"); return; }
    PASS();
}

static void
test_proxy_log_and_forward(void)
{
    TEST("proxy: serial -> log + PTY forward");

    /* create a simulated "real port" via PTY */
    int real_master, real_slave;
    if (openpty(&real_master, &real_slave, NULL, NULL, NULL) < 0) {
        FAIL("openpty failed");
        return;
    }
    char *real_slave_name = ttyname(real_slave);
    close(real_slave);

    /* open in proxy mode */
    serial_port_t sp;
    if (serial_open_proxy(&sp, real_slave_name, B115200) < 0) {
        FAIL("serial_open_proxy failed");
        close(real_master);
        return;
    }

    char session_path[512];
    log_create_session(session_path, sizeof(session_path));

    log_file_t lf;
    log_open(&lf, session_path, "PROXY_TEST", "Proxy Test\n");

    /* simulate board output on the "real port" */
    const char *msg = "Board booting...\n";
    ssize_t nw = write(real_master, msg, strlen(msg));
    (void)nw;
    usleep(100000);

    /* read from serial fd (real port) */
    char buf[4096];
    fd_set rfds;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(sp.fd, &rfds);

    if (select(sp.fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        ssize_t nr = read(sp.fd, buf, sizeof(buf));
        if (nr > 0) {
            /* log the data */
            log_write(&lf, buf, (size_t)nr);
            /* forward to PTY master (like monitor does) */
            nw = write(sp.pty_master, buf, (size_t)nr);
            (void)nw;
        }
    }

    log_close(&lf);

    /* verify log file has the data */
    FILE *fp = fopen(lf.filepath, "r");
    if (!fp) { FAIL("cannot read log"); serial_close(&sp); close(real_master); return; }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Board booting"))
            found = 1;
    }
    fclose(fp);

    serial_close(&sp);
    close(real_master);

    if (!found) { FAIL("data not in proxy log"); return; }
    PASS();
}

int main(void)
{
    printf("=== test_monitor ===\n");

    test_log_create_session();
    test_log_write_timestamps();
    test_log_marker();
    test_log_crlf_handling();
    test_log_prune();
    test_pty_to_log();
    test_label_log_filename();
    test_proxy_log_and_forward();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
