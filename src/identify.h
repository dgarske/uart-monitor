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
    const char *board_override;
    const char *board_match;   /* from USB product string match */
    char label[64];      /* filesystem-safe name, e.g. "VMK180_UART1" */
    int  baud;           /* per-device baud rate override (0 = use global) */
} tty_port_t;

typedef struct {
    char group_key[256];
    tty_port_t *ports[MAX_PORTS_PER_GROUP];
    int port_count;
} device_group_t;

/* Board config entry loaded from ~/.boards */
typedef struct {
    char serial[64];
    char board_name[128];
    int  baud;           /* per-board baud rate (0 = use global default) */
    char dev_path[256];  /* optional: match by device path instead of S/N */
} board_id_t;

#define MAX_BOARD_IDS 32

/* Scan all /dev/ttyUSB*, ttyACM*, ttyUART* ports. Returns count. */
int scan_all_ports(tty_port_t *ports, int max_ports);

/* Identify a single port by reading sysfs. Returns 0 on success. */
int identify_port(const char *dev_path, tty_port_t *port);

/* Drop cached results from st-info / STM32_Programmer_CLI probes so the
 * next identify_port() call shells out fresh. Use this after a hot-plug
 * event where the cache may have been populated before the new device
 * fully enumerated on the USB bus. */
void identify_reset_probe_caches(void);

/* Group ports by parent USB device. Returns number of groups. */
int group_ports(tty_port_t *ports, int nports,
                device_group_t *groups, int max_groups);

/* Generate a filesystem-safe label for a port's log directory. */
void get_device_label(tty_port_t *port);

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
