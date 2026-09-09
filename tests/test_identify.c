/* test_identify.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/identify.h"
#include "../src/util.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-40s ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static void
test_lookup_known_device(void)
{
    TEST("lookup_known_device CP210x");
    const known_device_t *dev = lookup_known_device(0x10c4, 0xea71);
    if (!dev) { FAIL("not found"); return; }
    if (strcmp(dev->name, "Silicon Labs CP210x") != 0) {
        FAIL("wrong name"); return;
    }
    if (dev->expected_ports != 4) { FAIL("wrong port count"); return; }
    PASS();
}

static void
test_lookup_unknown_device(void)
{
    TEST("lookup_known_device unknown VID:PID");
    const known_device_t *dev = lookup_known_device(0xffff, 0xffff);
    if (dev != NULL) { FAIL("should be NULL"); return; }
    PASS();
}

static void
test_lookup_port_function(void)
{
    TEST("lookup_port_function CP210x iface 2");
    const char *fn = lookup_port_function("Silicon Labs CP210x", 2);
    if (!fn) { FAIL("not found"); return; }
    if (strcmp(fn, "UART2") != 0) { FAIL("wrong function"); return; }
    PASS();
}

static void
test_get_device_label_known(void)
{
    TEST("get_device_label for known device");
    tty_port_t port;
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyUSB0", sizeof(port.tty_name));
    port.known = lookup_known_device(0x10c4, 0xea71);
    port.interface_num = 1;

    get_device_label(&port);

    if (strcmp(port.label, "POLARFIRE_SOC_UART1") != 0) {
        printf("\n    got: '%s' expected: 'POLARFIRE_SOC_UART1'\n    ",
               port.label);
        FAIL("wrong label");
        return;
    }
    PASS();
}

static void
test_get_device_label_override(void)
{
    TEST("get_device_label with board override");
    tty_port_t port;
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyUSB4", sizeof(port.tty_name));
    port.known = lookup_known_device(0x10c4, 0xea71);
    port.interface_num = 0;
    strlcpy_safe(port.board_override, "ZynqMP ZCU102",
                 sizeof(port.board_override));

    get_device_label(&port);

    if (strcmp(port.label, "ZYNQMP_ZCU102_UART0") != 0) {
        printf("\n    got: '%s' expected: 'ZYNQMP_ZCU102_UART0'\n    ",
               port.label);
        FAIL("wrong label");
        return;
    }
    PASS();
}

static void
test_lookup_board_by_product(void)
{
    TEST("lookup_board_by_product SCU35");
    const char *board = lookup_board_by_product("SCU35");
    if (!board) { FAIL("not found"); return; }
    if (strcmp(board, "SCU35") != 0) { FAIL("wrong board"); return; }
    PASS();
}

static void
test_get_device_label_product_match(void)
{
    TEST("get_device_label with product match");
    tty_port_t port;
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyUSB4", sizeof(port.tty_name));
    port.known = lookup_known_device(0x0403, 0x6011); /* FT4232H */
    port.interface_num = 1;
    port.board_match = lookup_board_by_product("SCU35");

    get_device_label(&port);

    if (strcmp(port.label, "SCU35_UART1") != 0) {
        printf("\n    got: '%s' expected: 'SCU35_UART1'\n    ", port.label);
        FAIL("wrong label");
        return;
    }
    PASS();
}

static void
test_get_device_label_fallback(void)
{
    TEST("get_device_label fallback to tty_name");
    tty_port_t port;
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyUSB99", sizeof(port.tty_name));
    port.known = NULL;
    port.interface_num = 0;

    get_device_label(&port);

    if (strcmp(port.label, "ttyUSB99") != 0) {
        printf("\n    got: '%s' expected: 'ttyUSB99'\n    ", port.label);
        FAIL("wrong label");
        return;
    }
    PASS();
}

static void
test_apply_board_config_path_mismatch(void)
{
    TEST("apply_board_config rejects stale path");
    tty_port_t ports[1];
    memset(ports, 0, sizeof(ports));

    /* NXP LPC-Link2 device on /dev/ttyACM0 */
    ports[0].vid = 0x1fc9;
    ports[0].pid = 0x0090;
    ports[0].known = lookup_known_device(0x1fc9, 0x0090);
    strlcpy_safe(ports[0].dev_path, "/dev/ttyACM0", sizeof(ports[0].dev_path));
    strlcpy_safe(ports[0].tty_name, "ttyACM0", sizeof(ports[0].tty_name));
    ports[0].interface_num = 1;

    /* Config says /dev/ttyACM0 is STM32H563 (stale entry) */
    board_id_t ids[1];
    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].dev_path, "/dev/ttyACM0", sizeof(ids[0].dev_path));
    strlcpy_safe(ids[0].board_name, "STM32H563", sizeof(ids[0].board_name));

    apply_board_config(ports, 1, ids, 1);

    if (ports[0].board_override[0] != '\0') {
        printf("\n    got override: '%s', expected none\n    ",
               ports[0].board_override);
        FAIL("should not apply mismatched board override");
        return;
    }
    PASS();
}

static void
test_apply_board_config_serial_match(void)
{
    TEST("apply_board_config allows serial match");
    tty_port_t ports[1];
    memset(ports, 0, sizeof(ports));

    /* NXP LPC-Link2 device with a serial number */
    ports[0].vid = 0x1fc9;
    ports[0].pid = 0x0090;
    ports[0].known = lookup_known_device(0x1fc9, 0x0090);
    strlcpy_safe(ports[0].serial, "EQAQBQLQ", sizeof(ports[0].serial));
    strlcpy_safe(ports[0].dev_path, "/dev/ttyACM0", sizeof(ports[0].dev_path));
    strlcpy_safe(ports[0].tty_name, "ttyACM0", sizeof(ports[0].tty_name));
    ports[0].interface_num = 1;

    /* Config matches by serial -- should override even if board differs */
    board_id_t ids[1];
    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].serial, "EQAQBQLQ", sizeof(ids[0].serial));
    strlcpy_safe(ids[0].board_name, "CustomBoard", sizeof(ids[0].board_name));

    apply_board_config(ports, 1, ids, 1);

    if (strcmp(ports[0].board_override, "CustomBoard") != 0) {
        FAIL("serial match should allow any board override");
        return;
    }
    PASS();
}

/* apply_board_config() must COPY the board name, not alias it. Every
 * daemon caller passes a stack-local board_id_t array and then copies the
 * populated tty_port_t into long-lived state, so an aliasing pointer would
 * dangle for the life of the daemon. Overwriting the config array here
 * while it is still in scope makes the check fully defined: it fails on an
 * aliasing implementation without relying on stack reuse. */
static void
test_apply_board_config_owns_name(void)
{
    TEST("apply_board_config copies the board name");
    tty_port_t port;
    board_id_t ids[1];

    memset(&port, 0, sizeof(port));
    port.vid = 0x1fc9;
    port.pid = 0x0090;
    port.known = lookup_known_device(0x1fc9, 0x0090);
    strlcpy_safe(port.serial, "EQAQBQLQ", sizeof(port.serial));
    strlcpy_safe(port.dev_path, "/dev/ttyACM0", sizeof(port.dev_path));
    strlcpy_safe(port.tty_name, "ttyACM0", sizeof(port.tty_name));
    port.interface_num = 1;

    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].serial, "EQAQBQLQ", sizeof(ids[0].serial));
    strlcpy_safe(ids[0].board_name, "CustomBoard", sizeof(ids[0].board_name));

    apply_board_config(&port, 1, ids, 1);

    /* the caller's config array goes away */
    memset(ids, 0xAA, sizeof(ids));

    if (strcmp(port.board_override, "CustomBoard") != 0) {
        printf("\n    got: '%s' expected: 'CustomBoard'\n    ",
               port.board_override);
        FAIL("board name not copied -- aliases caller storage");
        return;
    }
    PASS();
}

/* get_board_name() is the single source of truth for the reported board
 * name. status.json and the log header used to open-code the chain and
 * disagreed: the monitored-ports loop skipped board_match, so a device
 * resolved by USB product string reported its USB device's first known
 * board instead (SCU35 logged as "VMK180"). */
/* get_device_label() must never leave the label empty. An empty label is
 * destructive downstream: the log path becomes "<session>/.log" and the PTY
 * symlink becomes the pty directory itself, so every affected port shares
 * one file. Every branch emits at least "_UART" and the last falls back to
 * the tty name -- pin that so the invariant monitor.c's relabel guard
 * relies on cannot regress. */
static void
test_get_device_label_never_empty(void)
{
    TEST("get_device_label never empty");
    tty_port_t port;

    /* no board information at all: tty_name fallback */
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.dev_path, "/dev/ttyACM9", sizeof(port.dev_path));
    strlcpy_safe(port.tty_name, "ttyACM9", sizeof(port.tty_name));
    get_device_label(&port);
    if (port.label[0] == '\0') {
        FAIL("bare port produced an empty label");
        return;
    }

    /* a ~/.boards name that sanitizes away to nothing must still label */
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyACM9", sizeof(port.tty_name));
    strlcpy_safe(port.board_override, "---", sizeof(port.board_override));
    get_device_label(&port);
    if (port.label[0] == '\0') {
        FAIL("board_override produced an empty label");
        return;
    }

    /* resolved by probe / product string */
    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.tty_name, "ttyACM9", sizeof(port.tty_name));
    port.board_match = "NUCLEO-V873XJ";
    get_device_label(&port);
    if (strcmp(port.label, "NUCLEO_V873XJ_UART") != 0) {
        FAIL("board_match label mismatch");
        return;
    }

    PASS();
}

static void
test_get_board_name_precedence(void)
{
    TEST("get_board_name precedence chain");
    tty_port_t port;

    /* no information at all */
    memset(&port, 0, sizeof(port));
    if (strcmp(get_board_name(&port), "Unknown") != 0) {
        FAIL("bare port should be Unknown");
        return;
    }

    /* An unambiguous known device -> its single listed board. */
    port.known = lookup_known_device(0x110a, 0x1150);
    if (port.known == NULL) {
        FAIL("Moxa UPort 1150 missing from device table");
        return;
    }
    if (strcmp(get_board_name(&port), port.known->boards[0]) != 0) {
        FAIL("unambiguous device should use boards[0]");
        return;
    }

    /* An AMBIGUOUS known device must NOT name a board. boards[0] there is
     * only the first of several candidates sharing the VID:PID, and
     * reporting it as fact put "VMK180" on every unpinned FT4232H cable
     * on the bench. */
    port.known = lookup_known_device(0x0403, 0x6011);
    if (!known_device_is_ambiguous(port.known)) {
        FAIL("FT4232H should be ambiguous");
        return;
    }
    if (strcmp(get_board_name(&port), "Unknown") != 0) {
        printf("\n    got: '%s' expected: 'Unknown'\n    ",
               get_board_name(&port));
        FAIL("ambiguous device must not report a guessed board");
        return;
    }

    /* product-string match beats the known device's board list */
    port.board_match = lookup_board_by_product("SCU35");
    if (port.board_match == NULL) {
        FAIL("lookup_board_by_product(SCU35) returned NULL");
        return;
    }
    if (strcmp(get_board_name(&port), port.board_match) != 0) {
        printf("\n    got: '%s' expected: '%s'\n    ",
               get_board_name(&port), port.board_match);
        FAIL("board_match should outrank known->boards[0]");
        return;
    }

    /* a ~/.boards pin outranks everything */
    strlcpy_safe(port.board_override, "PinnedBoard",
                 sizeof(port.board_override));
    if (strcmp(get_board_name(&port), "PinnedBoard") != 0) {
        FAIL("board_override should outrank board_match");
        return;
    }
    PASS();
}

static void
test_group_ports(void)
{
    TEST("group_ports groups by VID:PID:serial");
    tty_port_t ports[4];
    memset(ports, 0, sizeof(ports));

    /* Two ports from same device */
    ports[0].vid = 0x10c4; ports[0].pid = 0xea71;
    strlcpy_safe(ports[0].serial, "ABC123", sizeof(ports[0].serial));
    strlcpy_safe(ports[0].usb_path, "1-6", sizeof(ports[0].usb_path));
    ports[0].interface_num = 0;

    ports[1].vid = 0x10c4; ports[1].pid = 0xea71;
    strlcpy_safe(ports[1].serial, "ABC123", sizeof(ports[1].serial));
    strlcpy_safe(ports[1].usb_path, "1-6", sizeof(ports[1].usb_path));
    ports[1].interface_num = 1;

    /* One port from different device */
    ports[2].vid = 0x0403; ports[2].pid = 0x6001;
    strlcpy_safe(ports[2].serial, "XYZ789", sizeof(ports[2].serial));
    strlcpy_safe(ports[2].usb_path, "1-4", sizeof(ports[2].usb_path));
    ports[2].interface_num = 0;

    device_group_t groups[MAX_GROUPS];
    memset(groups, 0, sizeof(groups));
    int ngroups = group_ports(ports, 3, groups, MAX_GROUPS);

    if (ngroups != 2) {
        printf("\n    got %d groups, expected 2\n    ", ngroups);
        FAIL("wrong group count");
        return;
    }

    /* find the group with 2 ports */
    int found2 = 0;
    for (int i = 0; i < ngroups; i++) {
        if (groups[i].port_count == 2) found2++;
    }
    if (found2 != 1) { FAIL("expected one group with 2 ports"); return; }
    PASS();
}


/* ------------------------------------------------------------------ */
/*  ~/.boards parsing and matching                                     */
/* ------------------------------------------------------------------ */

/* load_board_config() reads $HOME/.boards, so point HOME at a temp dir. */
static int
write_boards_file(const char *body)
{
    static char dir[] = "/tmp/umtest_boardsXXXXXX";
    static int made = 0;
    char path[512];
    FILE *fp;

    if (!made) {
        if (mkdtemp(dir) == NULL)
            return -1;
        made = 1;
    }
    setenv("HOME", dir, 1);
    snprintf(path, sizeof(path), "%s/.boards", dir);
    fp = fopen(path, "w");
    if (fp == NULL)
        return -1;
    fputs(body, fp);
    fclose(fp);
    return 0;
}

static const board_id_t *
find_board_id(const board_id_t *ids, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i].board_name, name) == 0)
            return &ids[i];
    }
    return NULL;
}

/* Regression: two path-pinned sections in a row. The old parser only
 * emitted a path pin when the PREVIOUS entry had no dev_path, so the
 * second of two consecutive path-pinned sections was silently dropped --
 * which is why the i.MX8QM MEK pin never took effect. */
static void
test_load_board_config_consecutive_path_pins(void)
{
    TEST("~/.boards two path pins in a row");
    board_id_t ids[MAX_BOARD_IDS];
    int n;

    if (write_boards_file(
            "# === BoardA ===\n"
            "# Baud: 115200\n"
            "BOARD_A=/dev/ttyUSB14\n"
            "\n"
            "# === BoardB ===\n"
            "# Baud: 115200\n"
            "BOARD_B=/dev/ttyUSB1\n") != 0) {
        FAIL("could not write temp ~/.boards");
        return;
    }

    n = load_board_config(ids, MAX_BOARD_IDS);
    if (find_board_id(ids, n, "BoardA") == NULL) {
        FAIL("BoardA pin missing");
        return;
    }
    if (find_board_id(ids, n, "BoardB") == NULL) {
        FAIL("BoardB pin dropped (consecutive path-pin regression)");
        return;
    }
    PASS();
}

/* A section may pin by USB topology alone. That is the only stable key
 * for an adapter that reports no serial. */
static void
test_load_board_config_usb_path_pin(void)
{
    TEST("~/.boards pin by USB topology");
    board_id_t ids[MAX_BOARD_IDS];
    const board_id_t *e;
    int n;

    if (write_boards_file(
            "# === SerialLessRig ===\n"
            "# USB: 1-6.1\n") != 0) {
        FAIL("could not write temp ~/.boards");
        return;
    }

    n = load_board_config(ids, MAX_BOARD_IDS);
    e = find_board_id(ids, n, "SerialLessRig");
    if (e == NULL) { FAIL("topology-only pin not loaded"); return; }
    if (strcmp(e->usb_path, "1-6.1") != 0) {
        FAIL("wrong usb_path parsed");
        return;
    }
    if (e->serial[0] != '\0') { FAIL("serial should be empty"); return; }
    PASS();
}

/* A serial-less device is matched on its hub port. */
static void
test_apply_board_config_usb_path_match(void)
{
    TEST("apply_board_config matches by topology");
    tty_port_t port;
    board_id_t ids[1];

    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.dev_path, "/dev/ttyUSB1", sizeof(port.dev_path));
    strlcpy_safe(port.tty_name, "ttyUSB1", sizeof(port.tty_name));
    strlcpy_safe(port.usb_path, "1-6.1", sizeof(port.usb_path));
    port.vid = 0x0403; port.pid = 0x6011;
    port.known = lookup_known_device(0x0403, 0x6011);
    port.interface_num = 0;

    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].usb_path, "1-6.1", sizeof(ids[0].usb_path));
    strlcpy_safe(ids[0].board_name, "IMX8QM_MEK",
                 sizeof(ids[0].board_name));

    apply_board_config(&port, 1, ids, 1);
    if (strcmp(port.board_override, "IMX8QM_MEK") != 0) {
        FAIL("topology pin not applied");
        return;
    }
    PASS();
}

/* Regression: when an entry carries a serial, its "# USB:" value is a
 * human note about where the board was last seen and is frequently
 * stale. Using it as a match key put NUCLEO-H563ZI's name on whichever
 * ST-LINK happened to occupy its old hub port. */
static void
test_apply_board_config_usb_path_ignored_with_serial(void)
{
    TEST("topology ignored when entry has a serial");
    tty_port_t port;
    board_id_t ids[1];

    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.dev_path, "/dev/ttyACM0", sizeof(port.dev_path));
    strlcpy_safe(port.tty_name, "ttyACM0", sizeof(port.tty_name));
    strlcpy_safe(port.usb_path, "1-1.2.1", sizeof(port.usb_path));
    strlcpy_safe(port.serial, "OTHERSTLINK", sizeof(port.serial));

    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].serial, "THEREALONE", sizeof(ids[0].serial));
    strlcpy_safe(ids[0].usb_path, "1-1.2.1", sizeof(ids[0].usb_path));
    strlcpy_safe(ids[0].board_name, "NUCLEO-H563ZI",
                 sizeof(ids[0].board_name));

    apply_board_config(&port, 1, ids, 1);
    if (port.board_override[0] != '\0') {
        printf("\n    got override: '%s'\n    ", port.board_override);
        FAIL("stale topology note must not match a different board");
        return;
    }
    PASS();
}

/* A serial pin must win even when some other entry path-matches. */
static void
test_apply_board_config_serial_outranks_path(void)
{
    TEST("serial pin outranks a /dev path pin");
    tty_port_t port;
    board_id_t ids[2];

    memset(&port, 0, sizeof(port));
    strlcpy_safe(port.dev_path, "/dev/ttyUSB4", sizeof(port.dev_path));
    strlcpy_safe(port.tty_name, "ttyUSB4", sizeof(port.tty_name));
    strlcpy_safe(port.serial, "REALSERIAL", sizeof(port.serial));

    memset(ids, 0, sizeof(ids));
    strlcpy_safe(ids[0].dev_path, "/dev/ttyUSB4", sizeof(ids[0].dev_path));
    strlcpy_safe(ids[0].board_name, "StalePathBoard",
                 sizeof(ids[0].board_name));
    strlcpy_safe(ids[1].serial, "REALSERIAL", sizeof(ids[1].serial));
    strlcpy_safe(ids[1].board_name, "RealBoard",
                 sizeof(ids[1].board_name));

    apply_board_config(&port, 1, ids, 2);
    if (strcmp(port.board_override, "RealBoard") != 0) {
        printf("\n    got: '%s'\n    ", port.board_override);
        FAIL("serial pin should win regardless of entry order");
        return;
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/*  Label stability                                                    */
/* ------------------------------------------------------------------ */

/* The four interfaces of a serial-less quad bridge must not collapse
 * onto one label: they would share a single log file and a single PTY
 * symlink, interleaving four consoles and losing three of them. */
static void
test_label_serialless_multiport_distinct(void)
{
    TEST("serial-less quad bridge gets 4 labels");
    tty_port_t ports[4];
    int i, j;

    for (i = 0; i < 4; i++) {
        memset(&ports[i], 0, sizeof(ports[i]));
        snprintf(ports[i].dev_path, sizeof(ports[i].dev_path),
                 "/dev/ttyUSB%d", 20 + i);
        snprintf(ports[i].tty_name, sizeof(ports[i].tty_name),
                 "ttyUSB%d", 20 + i);
        strlcpy_safe(ports[i].usb_path, "1-6.1",
                     sizeof(ports[i].usb_path));
        ports[i].vid = 0x0403;
        ports[i].pid = 0x6011;
        ports[i].interface_num = i;
        ports[i].known = lookup_known_device(0x0403, 0x6011);
        get_device_label(&ports[i]);
        if (ports[i].label[0] == '\0') {
            FAIL("empty label");
            return;
        }
    }

    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            if (strcmp(ports[i].label, ports[j].label) == 0) {
                printf("\n    iface %d and %d both '%s'\n    ",
                       i, j, ports[i].label);
                FAIL("labels collide");
                return;
            }
        }
    }
    PASS();
}

/* The label must not move when the kernel hands out a different ttyUSB
 * number, or the log file renames itself on every replug. */
static void
test_label_stable_across_renumber(void)
{
    TEST("label survives a tty renumber");
    tty_port_t a, b;

    memset(&a, 0, sizeof(a));
    strlcpy_safe(a.dev_path, "/dev/ttyUSB3", sizeof(a.dev_path));
    strlcpy_safe(a.tty_name, "ttyUSB3", sizeof(a.tty_name));
    strlcpy_safe(a.serial, "AL00KKC6", sizeof(a.serial));
    strlcpy_safe(a.usb_path, "1-4.3", sizeof(a.usb_path));
    a.vid = 0x0403; a.pid = 0x6001;
    a.known = lookup_known_device(0x0403, 0x6001);
    get_device_label(&a);

    /* same hardware, new tty number after a re-enumeration */
    b = a;
    strlcpy_safe(b.dev_path, "/dev/ttyUSB18", sizeof(b.dev_path));
    strlcpy_safe(b.tty_name, "ttyUSB18", sizeof(b.tty_name));
    get_device_label(&b);

    if (strcmp(a.label, b.label) != 0) {
        printf("\n    '%s' -> '%s'\n    ", a.label, b.label);
        FAIL("label changed with the tty number");
        return;
    }
    if (strstr(a.label, "TTYUSB") != NULL) {
        printf("\n    label '%s'\n    ", a.label);
        FAIL("label should not embed the tty name when a serial exists");
        return;
    }
    PASS();
}

/* With no serial, fall back to topology rather than the tty number. */
static void
test_label_serialless_prefers_topology(void)
{
    TEST("serial-less label prefers topology");
    tty_port_t p;

    memset(&p, 0, sizeof(p));
    strlcpy_safe(p.dev_path, "/dev/ttyUSB24", sizeof(p.dev_path));
    strlcpy_safe(p.tty_name, "ttyUSB24", sizeof(p.tty_name));
    strlcpy_safe(p.usb_path, "1-6.4", sizeof(p.usb_path));
    p.vid = 0x067b; p.pid = 0x2303;
    p.known = lookup_known_device(0x067b, 0x2303);
    get_device_label(&p);

    if (strstr(p.label, "1_6_4") == NULL) {
        printf("\n    got '%s'\n    ", p.label);
        FAIL("expected sanitised topology in the label");
        return;
    }
    if (strstr(p.label, "TTYUSB24") != NULL) {
        FAIL("tty name should not be used when topology is available");
        return;
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/*  Board-name arena                                                   */
/* ------------------------------------------------------------------ */

/* A flapping port re-probes every couple of seconds, and each probe
 * result is interned. Before interning, every repeat appended another
 * copy until the 4 KB arena filled -- after which intern_board_name()
 * returned NULL and EVERY port's probe result silently degraded to a
 * generic label. One dead board could take the whole bench's labels
 * with it. */
static void
test_intern_board_name_dedupes(void)
{
    TEST("repeated probe names do not grow arena");
    const char *first;
    size_t used_after_first;
    int i;

    first = intern_board_name("NUCLEO-H563ZI");
    if (first == NULL) { FAIL("first intern returned NULL"); return; }
    used_after_first = intern_board_name_used();

    /* simulate thousands of re-enumerations of the same board */
    for (i = 0; i < 5000; i++) {
        const char *again = intern_board_name("NUCLEO-H563ZI");
        if (again == NULL) {
            printf("\n    arena exhausted after %d repeats\n    ", i);
            FAIL("repeated interning exhausted the arena");
            return;
        }
        if (again != first) {
            FAIL("same name should return the same pointer");
            return;
        }
    }

    if (intern_board_name_used() != used_after_first) {
        printf("\n    grew from %zu to %zu bytes\n    ",
               used_after_first, intern_board_name_used());
        FAIL("arena grew while interning a duplicate");
        return;
    }
    PASS();
}

/* Distinct names must still each get their own storage. */
static void
test_intern_board_name_distinct(void)
{
    TEST("distinct probe names stay distinct");
    const char *a = intern_board_name("BoardOne");
    const char *b = intern_board_name("BoardTwo");

    if (a == NULL || b == NULL) { FAIL("intern returned NULL"); return; }
    if (a == b) { FAIL("different names share storage"); return; }
    if (strcmp(a, "BoardOne") != 0 || strcmp(b, "BoardTwo") != 0) {
        FAIL("interned content wrong");
        return;
    }
    PASS();
}

int main(void)
{
    printf("=== test_identify ===\n");

    test_lookup_known_device();
    test_lookup_unknown_device();
    test_lookup_port_function();
    test_lookup_board_by_product();
    test_get_device_label_known();
    test_get_device_label_product_match();
    test_get_device_label_override();
    test_get_device_label_fallback();
    test_apply_board_config_path_mismatch();
    test_apply_board_config_serial_match();
    test_apply_board_config_owns_name();
    test_get_board_name_precedence();
    test_get_device_label_never_empty();
    test_group_ports();
    test_load_board_config_consecutive_path_pins();
    test_load_board_config_usb_path_pin();
    test_apply_board_config_usb_path_match();
    test_apply_board_config_usb_path_ignored_with_serial();
    test_apply_board_config_serial_outranks_path();
    test_label_serialless_multiport_distinct();
    test_label_stable_across_renumber();
    test_label_serialless_prefers_topology();
    test_intern_board_name_dedupes();
    test_intern_board_name_distinct();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
