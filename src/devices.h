/* devices.h -- Known USB serial device table for embedded development boards.
 *
 * Ported from identify_tty_ports.py KNOWN_DEVICES and PORT_FUNCTIONS tables.
 * This tool NEVER writes to serial ports.
 */
#ifndef DEVICES_H
#define DEVICES_H

#include <stdint.h>
#include <string.h>

#define MAX_BOARDS_PER_DEVICE 4
#define MAX_PORT_FUNCTIONS    4

typedef struct {
    uint16_t    vid;
    uint16_t    pid;
    const char *name;
    int         expected_ports;
    const char *boards[MAX_BOARDS_PER_DEVICE];
} known_device_t;

typedef struct {
    const char *device_name;
    const char *functions[MAX_PORT_FUNCTIONS];
} port_function_t;

static const known_device_t KNOWN_DEVICES[] = {
    /* FTDI devices */
    { 0x0403, 0x6010, "FTDI FT2232H", 2,
      { "VMK180", "ZCU102", "Various", NULL } },
    { 0x0403, 0x6011, "FTDI FT4232H", 4,
      { "VMK180", "ZCU102", NULL, NULL } },
    { 0x0403, 0x6014, "FTDI FT232H",  1,
      { "Generic", NULL, NULL, NULL } },
    { 0x0403, 0x6001, "FTDI FT232R",  1,
      { "Generic", NULL, NULL, NULL } },

    /* Xilinx/AMD */
    { 0x04b4, 0x0008, "Cypress FX3",  4,
      { "Versal VMK180", "ZCU102", NULL, NULL } },

    /* Microchip PolarFire SoC */
    { 0x10c4, 0xea71, "Silicon Labs CP210x", 4,
      { "PolarFire SoC", NULL, NULL, NULL } },
    { 0x10c4, 0xea60, "Silicon Labs CP210x", 1,
      { "PolarFire SoC", "Generic", NULL, NULL } },

    /* STMicroelectronics */
    { 0x0483, 0x374b, "STM32 ST-LINK",         1,
      { "STM32H563", "STM32 boards", NULL, NULL } },
    { 0x0483, 0x374e, "STM32 Virtual COM Port", 1,
      { "STM32H563", NULL, NULL, NULL } },
    { 0x0483, 0x5740, "STM32 USB CDC",          1,
      { "USB Relay Controller", NULL, NULL, NULL } },

    /* USB Relay / Generic */
    { 0x1a86, 0x7523, "CH340 USB-Serial",  1,
      { "USB Relay", "Generic", NULL, NULL } },
    { 0x067b, 0x2303, "Prolific PL2303",   1,
      { "Generic", NULL, NULL, NULL } },

    /* Debuggers */
    { 0x0897, 0x0002, "Lauterbach TRACE32", 1,
      { "Debugger", NULL, NULL, NULL } },
};
#define KNOWN_DEVICES_COUNT \
    ((int)(sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0])))

static const port_function_t PORT_FUNCTIONS[] = {
    { "FTDI FT2232H",
      { "UART/JTAG Port A", "UART/JTAG Port B", NULL, NULL } },
    { "FTDI FT4232H",
      { "UART0/JTAG", "UART1", "UART2", "UART3" } },
    { "Cypress FX3",
      { "UART0 (Console)", "UART1 (PMC)", "UART2 (Debug)", "UART3" } },
    { "Silicon Labs CP210x",
      { "UART0", "UART1", "UART2", "UART3" } },
};
#define PORT_FUNCTIONS_COUNT \
    ((int)(sizeof(PORT_FUNCTIONS) / sizeof(PORT_FUNCTIONS[0])))

static inline const known_device_t *
lookup_known_device(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < KNOWN_DEVICES_COUNT; i++) {
        if (KNOWN_DEVICES[i].vid == vid && KNOWN_DEVICES[i].pid == pid)
            return &KNOWN_DEVICES[i];
    }
    return NULL;
}

static inline const char *
lookup_port_function(const char *device_name, int interface_num)
{
    if (!device_name || interface_num < 0)
        return NULL;
    for (int i = 0; i < PORT_FUNCTIONS_COUNT; i++) {
        if (strcmp(PORT_FUNCTIONS[i].device_name, device_name) == 0) {
            if (interface_num < MAX_PORT_FUNCTIONS)
                return PORT_FUNCTIONS[i].functions[interface_num];
        }
    }
    return NULL;
}

#endif /* DEVICES_H */
