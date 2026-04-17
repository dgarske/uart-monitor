/* devices.h
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
      { "VMK180", "ZCU102", "SCU35", NULL } },
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

    /* NXP */
    { 0x1fc9, 0x0090, "NXP LPC-Link2 CMSIS-DAP", 1,
      { "LPC54S018M-EVK", "NXP LPC boards", NULL, NULL } },

    /* STMicroelectronics */
    { 0x0483, 0x374b, "STM32 ST-LINK",         1,
      { "STM32H563", "STM32 boards", NULL, NULL } },
    { 0x0483, 0x374e, "STM32 Virtual COM Port", 1,
      { "STM32H563", NULL, NULL, NULL } },
    { 0x0483, 0x3754, "STM32 STLINK-V3",       1,
      { "STM32U3", "STM32N657", "STM32C5A3", "STM32 boards" } },
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

/* Map USB product strings to board names.
 * When multiple boards share the same VID:PID (e.g. FTDI FT4232H),
 * the USB product string distinguishes them. */
typedef struct {
    const char *product;    /* USB product string (substring match) */
    const char *board;      /* Board name to use */
} product_board_map_t;

static const product_board_map_t PRODUCT_BOARD_MAP[] = {
    /* Xilinx/AMD FPGA boards using FTDI FT4232H */
    { "SCU35",  "SCU35"  },   /* Spartan UltraScale+ */
    { "VMK180", "VMK180" },   /* Versal VMK180 */
    { "ZCU102", "ZCU102" },   /* Zynq UltraScale+ ZCU102 */
};
#define PRODUCT_BOARD_MAP_COUNT \
    ((int)(sizeof(PRODUCT_BOARD_MAP) / sizeof(PRODUCT_BOARD_MAP[0])))

static inline const char *
lookup_board_by_product(const char *product)
{
    if (!product || product[0] == '\0')
        return NULL;
    for (int i = 0; i < PRODUCT_BOARD_MAP_COUNT; i++) {
        if (strstr(product, PRODUCT_BOARD_MAP[i].product) != NULL)
            return PRODUCT_BOARD_MAP[i].board;
    }
    return NULL;
}

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

/* Map DBGMCU IDCODE (DEV_ID, low 12 bits) to STM32 family name.
 * Used to differentiate STM32 variants sharing the STLINK-V3 PID
 * (0x0483:0x3754) by actively probing via `st-info --probe`. */
typedef struct {
    uint32_t    chipid;
    const char *board;
} stm32_chipid_map_t;

static const stm32_chipid_map_t STM32_CHIPID_MAP[] = {
    { 0x454, "STM32U3"   },   /* confirmed on STM32U3 NUCLEO */
    { 0x455, "STM32U3"   },   /* alt ID reported by some variants */
    { 0x482, "STM32U5"   },
    { 0x484, "STM32H563" },
    { 0x505, "STM32N657" },
};
#define STM32_CHIPID_MAP_COUNT \
    ((int)(sizeof(STM32_CHIPID_MAP) / sizeof(STM32_CHIPID_MAP[0])))

static inline const char *
lookup_board_by_chipid(uint32_t chipid)
{
    for (int i = 0; i < STM32_CHIPID_MAP_COUNT; i++) {
        if (STM32_CHIPID_MAP[i].chipid == chipid)
            return STM32_CHIPID_MAP[i].board;
    }
    return NULL;
}

/* A known_device is "ambiguous" when its boards[] array lists more than
 * one specific board name (excluding generic fallbacks). In that case,
 * picking boards[0] arbitrarily would mislabel other variants — the
 * caller should probe (e.g. via st-info) to resolve the actual board. */
static inline int
known_device_is_ambiguous(const known_device_t *dev)
{
    if (!dev)
        return 0;
    int specific = 0;
    for (int b = 0; b < MAX_BOARDS_PER_DEVICE && dev->boards[b]; b++) {
        const char *n = dev->boards[b];
        if (strcmp(n, "Generic") == 0)
            continue;
        if (strstr(n, " boards") != NULL)
            continue;
        if (strstr(n, "Various") != NULL)
            continue;
        specific++;
    }
    return specific >= 2;
}

#endif /* DEVICES_H */
