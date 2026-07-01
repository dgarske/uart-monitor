/* identify_macos.c
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

/* macOS USB serial-port backend: device identity comes from the IOKit
 * registry (the IOSerialBSDClient node plus its USB device / interface
 * ancestors). The Linux equivalent (sysfs) lives in identify_linux.c. */

#include "platform.h"
#include "identify.h"
#include "util.h"

#include <glob.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

/* Search the entry and its parents for a CFString property; copy into out. */
static int
search_str(io_registry_entry_t entry, CFStringRef key,
           char *out, size_t sz)
{
    CFTypeRef v = IORegistryEntrySearchCFProperty(
        entry, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    int rc = -1;

    if (v == NULL)
        return -1;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        if (CFStringGetCString((CFStringRef)v, out, (CFIndex)sz,
                               kCFStringEncodingUTF8))
            rc = 0;
    }
    CFRelease(v);
    return rc;
}

/* Search the entry and its parents for a CFNumber property. */
static int
search_num(io_registry_entry_t entry, CFStringRef key, long long *out)
{
    CFTypeRef v = IORegistryEntrySearchCFProperty(
        entry, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    int rc = -1;

    if (v == NULL)
        return -1;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, out))
            rc = 0;
    }
    CFRelease(v);
    return rc;
}

/* Find the IOSerialBSDClient service whose call-out device node equals
 * dev_path (e.g. "/dev/cu.usbserial-AL00KKC6"). Returns a retained service
 * (caller releases with IOObjectRelease) or IO_OBJECT_NULL. */
static io_service_t
find_serial_service(const char *dev_path)
{
    CFMutableDictionaryRef match;
    io_iterator_t it;
    io_service_t svc;
    io_service_t found = IO_OBJECT_NULL;

    match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (match == NULL)
        return IO_OBJECT_NULL;

    /* IOServiceGetMatchingServices consumes the match reference. */
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it)
        != KERN_SUCCESS)
        return IO_OBJECT_NULL;

    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        CFTypeRef c = IORegistryEntryCreateCFProperty(
            svc, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (c != NULL && CFGetTypeID(c) == CFStringGetTypeID()) {
            char path[256];
            if (CFStringGetCString((CFStringRef)c, path, sizeof(path),
                                   kCFStringEncodingUTF8) &&
                strcmp(path, dev_path) == 0) {
                found = svc; /* keep -- do not release */
                CFRelease(c);
                break;
            }
        }
        if (c != NULL)
            CFRelease(c);
        IOObjectRelease(svc);
    }

    IOObjectRelease(it);
    return found;
}

void
plat_glob_serial_ports(glob_t *g)
{
    int flags = 0;

    /* USB-serial adapters appear as /dev/cu.usbserial-* (FTDI, CP210x,
     * CH340, ...) and CDC-ACM devices as /dev/cu.usbmodem* (ST-LINK,
     * many MCU VCPs). Non-USB call-out nodes (cu.Bluetooth-*, cu.debug-
     * console, ...) are excluded by pattern, and any that slip through
     * are dropped later because plat_read_usb_info() finds no USB info. */
    glob("/dev/cu.usbserial*", flags, NULL, g);
    flags |= GLOB_APPEND;
    glob("/dev/cu.usbmodem*", flags, NULL, g);
}

int
plat_read_usb_info(tty_port_t *port)
{
    io_service_t svc;
    long long n;

    svc = find_serial_service(port->dev_path);
    if (svc == IO_OBJECT_NULL)
        return -1;

    /* idVendor is the marker for a real USB device backing this node.
     * Without it, treat as a non-USB virtual tty (skip it). */
    if (search_num(svc, CFSTR("idVendor"), &n) != 0) {
        IOObjectRelease(svc);
        return -1;
    }
    port->vid = (uint16_t)n;

    if (search_num(svc, CFSTR("idProduct"), &n) == 0)
        port->pid = (uint16_t)n;

    search_str(svc, CFSTR("USB Serial Number"),
               port->serial, sizeof(port->serial));
    search_str(svc, CFSTR("USB Vendor Name"),
               port->manufacturer, sizeof(port->manufacturer));
    search_str(svc, CFSTR("USB Product Name"),
               port->product, sizeof(port->product));

    if (search_num(svc, CFSTR("bInterfaceNumber"), &n) == 0)
        port->interface_num = (int)n;

    /* locationID is the stable USB topology key (physical hub port),
     * the macOS analogue of the Linux "1-6.2" sysfs path. */
    if (search_num(svc, CFSTR("locationID"), &n) == 0)
        snprintf(port->usb_path, sizeof(port->usb_path),
                 "%08llx", (unsigned long long)(n & 0xffffffffLL));

    IOObjectRelease(svc);
    return 0;
}

int
read_port_serial(const char *dev_path, char *out, size_t out_sz)
{
    tty_port_t tmp;

    if (out == NULL || out_sz == 0)
        return -1;
    out[0] = '\0';

    memset(&tmp, 0, sizeof(tmp));
    strlcpy_safe(tmp.dev_path, dev_path, sizeof(tmp.dev_path));
    if (plat_read_usb_info(&tmp) != 0)
        return -1; /* device gone */

    strlcpy_safe(out, tmp.serial, out_sz);
    return 0; /* serial may legitimately be empty */
}
