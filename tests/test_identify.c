/* test_identify.c -- Tests for device identification and labeling. */
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
    port.board_override = "ZynqMP ZCU102";

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
    test_get_device_label_known();
    test_get_device_label_override();
    test_get_device_label_fallback();
    test_group_ports();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
