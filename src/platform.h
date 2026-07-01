/* platform.h
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
#ifndef PLATFORM_H
#define PLATFORM_H

#include <glob.h>

#include "identify.h"

/* OS-specific USB serial-port backend.
 *
 * Linux resolves device identity through sysfs (identify_linux.c); macOS
 * uses the IOKit registry (identify_macos.c). Each platform implements the
 * three hooks below plus read_port_serial() (declared in identify.h); the
 * shared logic in identify.c calls through them so it stays OS-agnostic.
 */

/* Enumerate candidate serial-port device paths into g. The caller has
 * already memset g and is responsible for globfree(). Linux matches
 * /dev/ttyUSB*, ttyACM*, ttyUART*; macOS matches /dev/cu.usbserial*,
 * cu.usbmodem*. */
void plat_glob_serial_ports(glob_t *g);

/* Fill the USB descriptor fields of port (vid, pid, serial, manufacturer,
 * product, interface_num, usb_path) for the device at port->dev_path.
 * port->dev_path and port->tty_name are already set by the caller and must
 * not be modified. Returns 0 on success, -1 if the device has no USB
 * backing (gone, or a non-USB virtual tty). */
int plat_read_usb_info(tty_port_t *port);

#endif /* PLATFORM_H */
