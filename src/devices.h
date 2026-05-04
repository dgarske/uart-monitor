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
    int         ambiguous;     /* 1 = force probe (chip-id / programmer CLI)
                                * to disambiguate; 0 = use boards[0] */
    const char *boards[MAX_BOARDS_PER_DEVICE];
} known_device_t;

typedef struct {
    const char *device_name;
    const char *functions[MAX_PORT_FUNCTIONS];
} port_function_t;

static const known_device_t KNOWN_DEVICES[] = {
    /* FTDI devices */
    { 0x0403, 0x6010, "FTDI FT2232H", 2, 0,
      { "VMK180", "ZCU102", "Various", NULL } },
    { 0x0403, 0x6011, "FTDI FT4232H", 4, 0,
      { "VMK180", "ZCU102", "SCU35", NULL } },
    { 0x0403, 0x6014, "FTDI FT232H",  1, 0,
      { "Generic", NULL, NULL, NULL } },
    { 0x0403, 0x6001, "FTDI FT232R",  1, 0,
      { "Generic", NULL, NULL, NULL } },

    /* Xilinx/AMD */
    { 0x04b4, 0x0008, "Cypress FX3",  4, 0,
      { "Versal VMK180", "ZCU102", NULL, NULL } },

    /* Silicon Labs CP210x bridges.
     * 0xea71 (CP2108, 4-port) is used on the Microchip PolarFire SoC
     * Icicle/Discovery kits. 0xea60 (CP2102/CP2103, single-port) is a
     * generic UART bridge shipped on many unrelated adapters (ZC702,
     * vendor dev kits, no-name USB-serial dongles); it should default
     * to "Generic" and be disambiguated via ~/.boards. */
    { 0x10c4, 0xea71, "Silicon Labs CP210x", 4, 0,
      { "PolarFire SoC", NULL, NULL, NULL } },
    { 0x10c4, 0xea60, "Silicon Labs CP210x", 1, 0,
      { "Generic", "ZC702", "PolarFire SoC", NULL } },

    /* NXP */
    { 0x1fc9, 0x0090, "NXP LPC-Link2 CMSIS-DAP", 1, 0,
      { "LPC54S018M-EVK", "NXP LPC boards", NULL, NULL } },

    /* STMicroelectronics: all three ST-LINK PIDs are shared by many
     * NUCLEO boards.  Force probe (STM32_Programmer_CLI / st-info) to
     * resolve the actual board instead of using boards[0]. */
    { 0x0483, 0x374b, "STM32 ST-LINK",         1, 1,
      { "STM32 boards", NULL, NULL, NULL } },
    { 0x0483, 0x374e, "STM32 Virtual COM Port", 1, 1,
      { "STM32 boards", NULL, NULL, NULL } },
    { 0x0483, 0x3754, "STM32 STLINK-V3",       1, 1,
      { "STM32 boards", NULL, NULL, NULL } },
    { 0x0483, 0x5740, "STM32 USB CDC",          1, 0,
      { "USB Relay Controller", NULL, NULL, NULL } },

    /* USB Relay / Generic */
    { 0x1a86, 0x7523, "CH340 USB-Serial",  1, 0,
      { "USB Relay", "Generic", NULL, NULL } },
    { 0x067b, 0x2303, "Prolific PL2303",   1, 0,
      { "Generic", NULL, NULL, NULL } },

    /* Debuggers */
    { 0x0897, 0x0002, "Lauterbach TRACE32", 1, 0,
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
    /* F0 / F1 / F2 / F3 / F4 / F7 series */
    { 0x408, "STM32F40x"   },   /* F405/F407 (RM0090, original) */
    { 0x413, "STM32F40x"   },   /* F405/F407 alt encoding */
    { 0x419, "STM32F4xx"   },   /* F42x/F43x (NUCLEO-F439ZI) */
    { 0x431, "STM32F411"   },
    { 0x441, "STM32F412"   },
    { 0x449, "STM32F74x"   },
    { 0x451, "STM32F76x"   },

    /* G0 / G4 series */
    { 0x460, "STM32G0"     },
    { 0x466, "STM32G0"     },
    { 0x467, "STM32G0"     },
    { 0x468, "STM32G4"     },   /* G431/G441 */
    { 0x469, "STM32G4"     },   /* G47x/G48x */
    { 0x479, "STM32G4"     },   /* G49x/G4Ax (NUCLEO-G491RE) */

    /* H5 / H7 series */
    { 0x450, "STM32H7"     },   /* H74x/H75x (NUCLEO-H753ZI) */
    { 0x480, "STM32H7Ax"   },
    { 0x483, "STM32H72x"   },
    { 0x484, "STM32H563"   },

    /* L4 / L5 series */
    { 0x461, "STM32L4"     },   /* L496/L4A6 */
    { 0x462, "STM32L4"     },   /* L45x/L46x */
    { 0x464, "STM32L4"     },   /* L412/L422 */
    { 0x470, "STM32L4Plus" },   /* L4Rx/L4Sx */
    { 0x471, "STM32L4Plus" },   /* L4P5/L4Q5 */
    { 0x472, "STM32L5"     },   /* L552/L562 */

    /* U / N series (newest) */
    { 0x454, "STM32U3"     },   /* confirmed on NUCLEO-U385RG-Q */
    { 0x455, "STM32U3"     },   /* alt ID reported by some variants */
    { 0x482, "STM32U5"     },
    { 0x505, "STM32N657"   },

    /* WB / WL series */
    { 0x495, "STM32WB"     },   /* WBx0/WBx5 (P-NUCLEO-WB55) */
    { 0x497, "STM32WL"     },
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

/* A known_device is "ambiguous" when its VID:PID is shared by multiple
 * boards or MCU families and boards[0] is not authoritative.  The
 * `ambiguous` field is the explicit signal; the heuristic below kept
 * for back-compat with entries that haven't been audited yet. When
 * ambiguous, the caller should probe (STM32_Programmer_CLI, st-info)
 * to resolve the actual board. */
static inline int
known_device_is_ambiguous(const known_device_t *dev)
{
    if (!dev)
        return 0;
    if (dev->ambiguous)
        return 1;
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
