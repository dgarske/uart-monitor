/* identify.h
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
#ifndef IDENTIFY_H
#define IDENTIFY_H

#include "devices.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_PORTS       64
#define MAX_GROUPS      32
#define MAX_PORTS_PER_GROUP 8

typedef struct {
    char dev_path[256];
    char tty_name[32];
    uint16_t vid;
    uint16_t pid;
    int interface_num;
    char serial[64];
    char manufacturer[128];
    char product[128];
    char usb_path[128];
    const known_device_t *known;
    const char *function_name;
    /* Owned copy of the ~/.boards board name. This must NOT be a
     * pointer: every caller of apply_board_config() passes a stack-local
     * board_id_t array, and the populated tty_port_t is then copied into
     * daemon-lifetime state, so a pointer into that array would dangle. */
    char board_override[128];
    const char *board_match;   /* from USB product string match */
    char label[64];      /* filesystem-safe name, e.g. "VMK180_UART1" */
    int  baud;           /* per-device baud rate override (0 = use global) */
} tty_port_t;

typedef struct {
    char group_key[256];
    tty_port_t *ports[MAX_PORTS_PER_GROUP];
    int port_count;
} device_group_t;

/* Board config entry loaded from ~/.boards.
 *
 * Three possible match keys, in descending order of trustworthiness:
 *   serial   - globally unique, survives re-enumeration. Best.
 *   usb_path - USB topology, e.g. "1-6.1". Stable as long as the cable
 *              stays in the same hub port. The only stable key available
 *              for adapters that report no serial (FT4232H strapped with
 *              SerialNumber=0), whose /dev path floats on every replug.
 *   dev_path - /dev/ttyUSBn. Volatile; the kernel reuses freed minor
 *              numbers, so a stale pin can land on an unrelated board.
 */
typedef struct {
    char serial[64];
    char board_name[128];
    int  baud;           /* per-board baud rate (0 = use global default) */
    char usb_path[128];  /* optional: match by USB topology path */
    char dev_path[256];  /* optional: match by device path instead of S/N */
} board_id_t;

#define MAX_BOARD_IDS 64

/* Scan all /dev/ttyUSB*, ttyACM*, ttyUART* ports. Returns count.
 * Runs the full identify (may shell out to st-info / STM32_Programmer_CLI);
 * inside the daemon use only off the main thread. */
int scan_all_ports(tty_port_t *ports, int max_ports);

/* Like scan_all_ports() but identifies each port with identify_port_cheap()
 * -- sysfs only, no external probe, no probe-cache reset. Safe on the
 * daemon's main epoll thread. */
int scan_all_ports_cheap(tty_port_t *ports, int max_ports);

/* Identify a single port by reading sysfs. Returns 0 on success.
 * The full path may run an external SWD/CLI probe for ambiguous devices
 * and mutates probe-cache statics -- in the daemon call this ONLY from
 * the identify worker thread. */
int identify_port(const char *dev_path, tty_port_t *port);

/* Cheap identification: sysfs only, no external probe, no probe-cache
 * access. Safe to call from the daemon's main thread. Ambiguous ST-LINK
 * devices receive the generic fallback label. Returns 0 on success. */
int identify_port_cheap(const char *dev_path, tty_port_t *port);

/* Cheap sysfs-only read of a port's USB serial number. Runs no SWD /
 * STM32_Programmer_CLI probe, so it is safe to call frequently. Writes the
 * serial (possibly an empty string) to out. Returns 0 on success, or -1 if
 * the tty has no sysfs device entry (i.e. the device has gone away). */
int read_port_serial(const char *dev_path, char *out, size_t out_sz);

/* Drop cached results from st-info / STM32_Programmer_CLI probes so the
 * next identify_port() call shells out fresh. Use this after a hot-plug
 * event where the cache may have been populated before the new device
 * fully enumerated on the USB bus. */
void identify_reset_probe_caches(void);

/* Group ports by parent USB device. Returns number of groups. */
int group_ports(tty_port_t *ports, int nports,
                device_group_t *groups, int max_groups);

/* Intern a board name into process-lifetime storage and return a pointer
 * to it. tty_port_t.board_match is a `const char *` that outlives the
 * caller's stack frame, so every probe-derived name passes through here.
 * Interning, not appending: calling this repeatedly with the same name
 * returns the same pointer and consumes no extra space, which is what
 * keeps a port that re-enumerates every few seconds from exhausting the
 * arena and dragging every other port's label down with it.
 * Returns NULL if the arena is full. */
const char *intern_board_name(const char *name);

/* Bytes of the board-name arena currently in use. Diagnostic: once this
 * reaches the arena size, further probe results fall back to generic
 * labels. */
size_t intern_board_name_used(void);

/* Generate a filesystem-safe label for a port's log directory. */
void get_device_label(tty_port_t *port);

/* Human-readable board name: ~/.boards pin, else product/probe match,
 * else the USB device's single known board, else "Unknown". Never NULL.
 * A device whose VID:PID is shared by several boards reports "Unknown"
 * rather than naming the first candidate -- a guess presented as fact is
 * worse than admitting the board is unresolved. */
const char *get_board_name(const tty_port_t *port);

/* Print formatted table of ports grouped by device. */
void print_port_table(device_group_t *groups, int ngroups, int verbose);

/* Load board identifications from ~/.boards. Returns count. */
int load_board_config(board_id_t *ids, int max_ids);

/* Apply board overrides from config to scanned ports. */
void apply_board_config(tty_port_t *ports, int nports,
                        board_id_t *ids, int nids);

/* The identify subcommand. */
int cmd_identify(int argc, char *argv[]);

#endif /* IDENTIFY_H */
