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

    /* known device only -> its first listed board */
    port.known = lookup_known_device(0x0403, 0x6011);
    if (strcmp(get_board_name(&port), port.known->boards[0]) != 0) {
        FAIL("known device should fall back to boards[0]");
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
    test_group_ports();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
