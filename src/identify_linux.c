/* identify_linux.c
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

/* Linux USB serial-port backend: device identity comes from sysfs
 * (/sys/class/tty/<name>/device) and the netlink/inotify hot-plug layer.
 * The macOS equivalent lives in identify_macos.c. */

#include "platform.h"
#include "identify.h"
#include "util.h"

#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract the USB bus path (e.g. "1-6.2") from a sysfs device path.
 * Looks for pattern /usbN/<path>/ in the resolved sysfs path. */
static void
extract_usb_path(const char *sysfs_path, char *usb_path, size_t sz)
{
    usb_path[0] = '\0';
    /* Find /usbN/ in the path, then grab the next path component */
    const char *p = sysfs_path;
    while ((p = strstr(p, "/usb")) != NULL) {
        p += 4; /* skip "/usb" */
        /* skip the bus number digit(s) */
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '/') {
            p++;
            /* now p points to the USB device path like "1-6.2/..." */
            const char *end = p;
            /* USB path is digits, dashes, dots until next slash or colon */
            while (*end && *end != '/' && *end != ':')
                end++;
            size_t len = (size_t)(end - p);
            if (len > 0 && len < sz) {
                memcpy(usb_path, p, len);
                usb_path[len] = '\0';
            }
            return;
        }
        /* keep searching */
    }
}

void
plat_glob_serial_ports(glob_t *g)
{
    int flags = 0;

    glob("/dev/ttyUSB*",  flags, NULL, g);
    flags |= GLOB_APPEND;
    glob("/dev/ttyACM*",  flags, NULL, g);
    glob("/dev/ttyUART*", flags, NULL, g);
}

int
plat_read_usb_info(tty_port_t *port)
{
    /* resolve /sys/class/tty/<name>/device
     * Use PATH_MAX-sized buffers since sysfs paths can be very long. */
    char syslink[512];
    char resolved[PATH_MAX];
    snprintf(syslink, sizeof(syslink),
             "/sys/class/tty/%s/device", port->tty_name);

    if (realpath(syslink, resolved) == NULL) {
        /* no sysfs entry -- might be a virtual tty */
        return -1;
    }

    /* Walk up the directory tree looking for USB device properties.
     * For ttyUSB: resolved = .../1-6.2:1.0/ttyUSB0/ttyUSB0
     *   interface dir has bInterfaceNumber
     *   USB device dir (parent of interface) has idVendor
     * For ttyACM: resolved = .../1-5.3:1.2
     *   this IS the interface dir */
    char path[PATH_MAX];
    strlcpy_safe(path, resolved, sizeof(path));
    int found_iface = 0;

    /* Helper to build sysfs attribute paths safely.
     * attr_buf must be PATH_MAX + 32 to guarantee no truncation. */
    #define SYSFS_ATTR_BUFSZ (PATH_MAX + 32)
    char attr[SYSFS_ATTR_BUFSZ];
    char val[128];

    for (int depth = 0; depth < 12; depth++) {
        /* Check for bInterfaceNumber (interface directory) */
        if (!found_iface) {
            snprintf(attr, sizeof(attr), "%s/bInterfaceNumber", path);
            if (sysfs_read_attr(attr, val, sizeof(val)) >= 0) {
                port->interface_num = (int)strtol(val, NULL, 10);
                found_iface = 1;
            }
        }

        /* Check for idVendor (USB device directory) */
        snprintf(attr, sizeof(attr), "%s/idVendor", path);
        if (sysfs_read_attr(attr, val, sizeof(val)) >= 0) {
            /* Found the USB device directory */
            sysfs_read_hex(attr, &port->vid);

            snprintf(attr, sizeof(attr), "%s/idProduct", path);
            sysfs_read_hex(attr, &port->pid);

            snprintf(attr, sizeof(attr), "%s/serial", path);
            sysfs_read_attr(attr, port->serial, sizeof(port->serial));

            snprintf(attr, sizeof(attr), "%s/manufacturer", path);
            sysfs_read_attr(attr, port->manufacturer,
                           sizeof(port->manufacturer));

            snprintf(attr, sizeof(attr), "%s/product", path);
            sysfs_read_attr(attr, port->product, sizeof(port->product));

            /* Extract USB path from this sysfs path */
            extract_usb_path(path, port->usb_path, sizeof(port->usb_path));
            return 0;
        }

        /* go up one directory */
        char *sl = strrchr(path, '/');
        if (!sl || sl == path)
            break;
        *sl = '\0';
    }

    /* tty exists but has no USB device parent -- treat as no USB backing */
    return -1;
}

int
read_port_serial(const char *dev_path, char *out, size_t out_sz)
{
    const char *slash;
    char tty_name[32];
    char syslink[512];
    char resolved[PATH_MAX];
    char path[PATH_MAX];
    char attr[PATH_MAX + 32];
    char val[128];
    char *sl;
    int depth;

    if (out == NULL || out_sz == 0)
        return -1;
    out[0] = '\0';

    slash = strrchr(dev_path, '/');
    strlcpy_safe(tty_name, slash != NULL ? slash + 1 : dev_path,
                 sizeof(tty_name));

    snprintf(syslink, sizeof(syslink),
             "/sys/class/tty/%s/device", tty_name);
    if (realpath(syslink, resolved) == NULL)
        return -1; /* no sysfs entry -- device gone */

    /* Walk up to the USB device directory (the one with idVendor) and
     * read its serial. This is the cheap prefix of identify_port() with
     * no SWD / STM32_Programmer_CLI probe, so it is safe to call often. */
    strlcpy_safe(path, resolved, sizeof(path));
    for (depth = 0; depth < 12; depth++) {
        snprintf(attr, sizeof(attr), "%s/idVendor", path);
        if (sysfs_read_attr(attr, val, sizeof(val)) >= 0) {
            snprintf(attr, sizeof(attr), "%s/serial", path);
            sysfs_read_attr(attr, out, out_sz);
            return 0; /* serial may legitimately be empty */
        }
        sl = strrchr(path, '/');
        if (sl == NULL || sl == path)
            break;
        *sl = '\0';
    }
    return 0;
}
