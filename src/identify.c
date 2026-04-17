/* identify.c
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
#include "identify.h"
#include "log.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------- */
/* ST-LINK chipid probe                                                 */
/*                                                                      */
/* The STLINK-V3 USB VID:PID (0x0483:0x3754) is shared by many STM32    */
/* families (U3, N6, C5, H5, ...). USB descriptors do not differentiate */
/* them. When we encounter an "ambiguous" known_device, shell out to    */
/* `st-info --probe` (stlink-tools) which reads the DBGMCU IDCODE and   */
/* returns a chipid that uniquely identifies the MCU family. Results    */
/* are cached per scan so multiple ttys / multiple STLINK units don't   */
/* spawn multiple subprocesses.                                         */
/* -------------------------------------------------------------------- */

typedef struct {
    char serial[64];
    uint32_t chipid;
} stlink_probe_entry_t;

#define STLINK_PROBE_CACHE_SZ 8
#define STLINK_PROBE_CACHE_TTL_SEC 2
static stlink_probe_entry_t stlink_probe_cache[STLINK_PROBE_CACHE_SZ];
static int stlink_probe_cache_count = 0;
static int stlink_probe_cache_populated = 0;
static time_t stlink_probe_cache_time = 0;

/* Arena for probe-derived board names. The tty_port_t->board_match field
 * is `const char *` and expects pointers that live for the process, so
 * we can't hand out stack buffers. */
#define PROBE_NAME_ARENA_SZ 256
static char probe_name_arena[PROBE_NAME_ARENA_SZ];
static size_t probe_name_arena_used = 0;

static const char *
intern_board_name(const char *name)
{
    if (!name)
        return NULL;
    size_t need = strlen(name) + 1;
    if (probe_name_arena_used + need > sizeof(probe_name_arena))
        return NULL;
    char *slot = probe_name_arena + probe_name_arena_used;
    memcpy(slot, name, need);
    probe_name_arena_used += need;
    return slot;
}

/* Reset cache at the start of each full scan (see scan_all_ports). */
static void
stlink_probe_cache_reset(void)
{
    stlink_probe_cache_count = 0;
    stlink_probe_cache_populated = 0;
    stlink_probe_cache_time = 0;
}

/* Populate the cache by running `st-info --probe` once. Returns 0 on
 * success (including the case of zero programmers found), -1 if the
 * subprocess failed (tool missing, exec error, non-zero exit). */
static int
stlink_probe_cache_populate(void)
{
    time_t now = time(NULL);
    if (stlink_probe_cache_populated &&
        now - stlink_probe_cache_time < STLINK_PROBE_CACHE_TTL_SEC)
        return 0;

    /* (re)populate: clear prior state first */
    stlink_probe_cache_count = 0;
    stlink_probe_cache_populated = 1;
    stlink_probe_cache_time = now;

    FILE *fp = popen("st-info --probe 2>/dev/null", "r");
    if (!fp)
        return -1;

    char line[256];
    char cur_serial[64] = {0};
    uint32_t cur_chipid = 0;
    int have_chipid = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* trim leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "serial:", 7) == 0) {
            /* flush previous block if complete */
            if (cur_serial[0] && have_chipid &&
                stlink_probe_cache_count < STLINK_PROBE_CACHE_SZ) {
                strlcpy_safe(stlink_probe_cache[stlink_probe_cache_count]
                                 .serial, cur_serial, 64);
                stlink_probe_cache[stlink_probe_cache_count].chipid =
                    cur_chipid;
                stlink_probe_cache_count++;
            }
            cur_serial[0] = '\0';
            have_chipid = 0;
            cur_chipid = 0;

            const char *v = p + 7;
            while (*v == ' ' || *v == '\t') v++;
            int i = 0;
            while (*v && *v != '\n' && *v != '\r' && *v != ' ' &&
                   i < (int)sizeof(cur_serial) - 1) {
                cur_serial[i++] = *v++;
            }
            cur_serial[i] = '\0';
        }
        else if (strncmp(p, "chipid:", 7) == 0) {
            const char *v = p + 7;
            while (*v == ' ' || *v == '\t') v++;
            cur_chipid = (uint32_t)strtoul(v, NULL, 0);
            have_chipid = 1;
        }
    }

    /* flush trailing block */
    if (cur_serial[0] && have_chipid &&
        stlink_probe_cache_count < STLINK_PROBE_CACHE_SZ) {
        strlcpy_safe(stlink_probe_cache[stlink_probe_cache_count].serial,
                     cur_serial, 64);
        stlink_probe_cache[stlink_probe_cache_count].chipid = cur_chipid;
        stlink_probe_cache_count++;
    }

    int rc = pclose(fp);
    if (rc == -1)
        return -1;
    /* st-info returns 0 on success; tolerate non-zero only if we parsed
     * at least one entry (some older builds exit non-zero on partial). */
    if (rc != 0 && stlink_probe_cache_count == 0)
        return -1;
    return 0;
}

/* Look up the board name for a given STLINK serial by probing.
 * Returns 0 + fills board_out on success, -1 if not found / probe failed. */
static int
probe_stlink_chipid(const char *serial, char *board_out, size_t board_sz)
{
    if (!serial || !serial[0])
        return -1;
    if (stlink_probe_cache_populate() < 0)
        return -1;
    for (int i = 0; i < stlink_probe_cache_count; i++) {
        if (strcmp(stlink_probe_cache[i].serial, serial) != 0)
            continue;
        const char *b = lookup_board_by_chipid(stlink_probe_cache[i].chipid);
        if (!b)
            return -1;
        strlcpy_safe(board_out, b, board_sz);
        return 0;
    }
    return -1;
}

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

int
identify_port(const char *dev_path, tty_port_t *port)
{
    memset(port, 0, sizeof(*port));
    strlcpy_safe(port->dev_path, dev_path, sizeof(port->dev_path));

    /* extract tty name from path */
    const char *slash = strrchr(dev_path, '/');
    strlcpy_safe(port->tty_name, slash ? slash + 1 : dev_path,
                 sizeof(port->tty_name));

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
            break;
        }

        /* go up one directory */
        char *sl = strrchr(path, '/');
        if (!sl || sl == path)
            break;
        *sl = '\0';
    }

    /* fallback names */
    if (port->manufacturer[0] == '\0')
        strlcpy_safe(port->manufacturer, "Unknown",
                     sizeof(port->manufacturer));
    if (port->product[0] == '\0')
        strlcpy_safe(port->product, "Unknown", sizeof(port->product));

    /* look up in known device table */
    port->known = lookup_known_device(port->vid, port->pid);

    /* try to match board by USB product string */
    port->board_match = lookup_board_by_product(port->product);

    /* For ambiguous devices (VID:PID shared by multiple MCU families
     * like the STLINK-V3), probe the debug interface to resolve the
     * actual chip. Only runs if the user hasn't already matched by
     * product string. Best-effort: silently falls through if st-info
     * is missing or the probe fails. */
    if (port->known && !port->board_match && port->serial[0] &&
        known_device_is_ambiguous(port->known)) {
        char probed[48];
        if (probe_stlink_chipid(port->serial, probed, sizeof(probed)) == 0)
            port->board_match = intern_board_name(probed);
    }

    /* determine function name */
    if (port->known) {
        port->function_name =
            lookup_port_function(port->known->name, port->interface_num);
    }
    if (!port->function_name) {
        if (strstr(port->tty_name, "ACM"))
            port->function_name = "Main UART";
        else
            port->function_name = "Main UART";
    }

    /* generate label */
    get_device_label(port);

    return 0;
}

int
scan_all_ports(tty_port_t *ports, int max_ports)
{
    glob_t g;
    int flags = 0;
    int n = 0;

    memset(&g, 0, sizeof(g));

    /* Reset per-scan probe cache so stale hotplug state doesn't leak. */
    stlink_probe_cache_reset();

    glob("/dev/ttyUSB*",  flags, NULL, &g);
    flags |= GLOB_APPEND;
    glob("/dev/ttyACM*",  flags, NULL, &g);
    glob("/dev/ttyUART*", flags, NULL, &g);

    for (size_t i = 0; i < g.gl_pathc && n < max_ports; i++) {
        if (identify_port(g.gl_pathv[i], &ports[n]) == 0)
            n++;
    }

    globfree(&g);
    return n;
}

void
get_device_label(tty_port_t *port)
{
    /* If we have a board override and known device, use board + UART + iface */
    if (port->board_override && port->board_override[0]) {
        char board[48];
        strlcpy_safe(board, port->board_override, sizeof(board));
        for (char *p = board; *p; p++) {
            if (*p == ' ') *p = '_';
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        snprintf(port->label, sizeof(port->label),
                 "%.48s_UART%d", board, port->interface_num);
        return;
    }

    /* If board matched by USB product string */
    if (port->board_match) {
        char clean[48];
        strlcpy_safe(clean, port->board_match, sizeof(clean));
        for (char *p = clean; *p; p++) {
            if (*p == ' ') *p = '_';
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        if (port->known && port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART%d", clean, port->interface_num);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART", clean);
        }
        return;
    }

    /* Ambiguous device (multiple candidate boards share this VID:PID):
     * if we weren't able to resolve it via product-string / probe, use
     * the device name instead of arbitrarily picking boards[0]. E.g.
     * STLINK-V3 with unknown chipid -> STM32_STLINK_V3_UART. */
    if (known_device_is_ambiguous(port->known)) {
        char clean[48];
        strlcpy_safe(clean, port->known->name, sizeof(clean));
        for (char *p = clean; *p; p++) {
            if (*p == ' ' || *p == '-') *p = '_';
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        snprintf(port->label, sizeof(port->label), "%.48s_UART", clean);
        return;
    }

    /* If known device with a board name */
    if (port->known && port->known->boards[0]) {
        const char *board = port->known->boards[0];
        char clean[48];
        strlcpy_safe(clean, board, sizeof(clean));
        for (char *p = clean; *p; p++) {
            if (*p == ' ') *p = '_';
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        if (strcmp(clean, "GENERIC") == 0) {
            /* Generic devices: include tty name to avoid collisions
             * when multiple unidentified adapters are present */
            snprintf(port->label, sizeof(port->label),
                     "%.24s_UART_%s", clean, port->tty_name);
        } else if (port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART%d", clean, port->interface_num);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART", clean);
        }
        return;
    }

    /* fallback: just the tty name */
    strlcpy_safe(port->label, port->tty_name, sizeof(port->label));
}

int
group_ports(tty_port_t *ports, int nports,
            device_group_t *groups, int max_groups)
{
    int ngroups = 0;

    for (int i = 0; i < nports; i++) {
        /* build group key: vid:pid:serial:usb_path */
        char key[256];
        snprintf(key, sizeof(key), "%04x:%04x:%s:%s",
                 ports[i].vid, ports[i].pid,
                 ports[i].serial, ports[i].usb_path);

        /* find existing group */
        int found = -1;
        for (int g = 0; g < ngroups; g++) {
            if (strcmp(groups[g].group_key, key) == 0) {
                found = g;
                break;
            }
        }

        if (found >= 0) {
            device_group_t *grp = &groups[found];
            if (grp->port_count < MAX_PORTS_PER_GROUP)
                grp->ports[grp->port_count++] = &ports[i];
        } else if (ngroups < max_groups) {
            device_group_t *grp = &groups[ngroups];
            strlcpy_safe(grp->group_key, key, sizeof(grp->group_key));
            grp->ports[0] = &ports[i];
            grp->port_count = 1;
            ngroups++;
        }
    }

    /* sort ports within each group by interface number */
    for (int g = 0; g < ngroups; g++) {
        device_group_t *grp = &groups[g];
        for (int i = 0; i < grp->port_count - 1; i++) {
            for (int j = i + 1; j < grp->port_count; j++) {
                if (grp->ports[j]->interface_num <
                    grp->ports[i]->interface_num) {
                    tty_port_t *tmp = grp->ports[i];
                    grp->ports[i] = grp->ports[j];
                    grp->ports[j] = tmp;
                }
            }
        }
    }

    return ngroups;
}

int
load_board_config(board_id_t *ids, int max_ids)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/.boards", home);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    char line[512];
    char current_board[128] = {0};
    int current_baud = 0;
    int nids = 0;

    while (fgets(line, sizeof(line), fp) && nids < max_ids) {
        /* look for board headers: # === Board Name === */
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "# === ", 6) == 0) {
            char *end = strstr(trimmed + 6, " ===");
            if (end) {
                size_t len = (size_t)(end - (trimmed + 6));
                if (len >= sizeof(current_board))
                    len = sizeof(current_board) - 1;
                memcpy(current_board, trimmed + 6, len);
                current_board[len] = '\0';
                /* strip parenthetical suffix e.g. " (STLINK-V3)" */
                char *paren = strrchr(current_board, '(');
                if (paren && paren > current_board && *(paren - 1) == ' ') {
                    *(paren - 1) = '\0';
                }
            }
            current_baud = 0;
            continue;
        }

        /* look for: # Baud: <rate> */
        if (current_board[0] && strncmp(trimmed, "# Baud:", 7) == 0) {
            const char *val = trimmed + 7;
            while (*val == ' ') val++;
            current_baud = atoi(val);
            continue;
        }

        /* look for: # USB: <path> | S/N: <serial> */
        if (current_board[0] && strstr(line, "# USB:") &&
            strstr(line, "S/N:")) {
            const char *sn = strstr(line, "S/N:");
            if (sn) {
                sn += 4;
                while (*sn == ' ') sn++;
                char serial[64];
                int si = 0;
                while (*sn && *sn != '\n' && *sn != '\r' && *sn != ' ' &&
                       si < (int)sizeof(serial) - 1) {
                    serial[si++] = *sn++;
                }
                serial[si] = '\0';

                if (serial[0]) {
                    strlcpy_safe(ids[nids].serial, serial,
                                sizeof(ids[nids].serial));
                    strlcpy_safe(ids[nids].board_name, current_board,
                                sizeof(ids[nids].board_name));
                    ids[nids].baud = current_baud;
                    ids[nids].dev_path[0] = '\0';
                    nids++;
                }
            }
        }

        /* look for: LABEL=/dev/ttyUSBN  (device path assignment, no S/N) */
        if (current_board[0] && trimmed[0] != '#' && trimmed[0] != '\n') {
            char *eq = strchr(trimmed, '=');
            if (eq && strncmp(eq + 1, "/dev/tty", 8) == 0) {
                /* extract device path (strip trailing comment/whitespace) */
                char *devp = eq + 1;
                char dev[256];
                int di = 0;
                while (*devp && *devp != ' ' && *devp != '\t' &&
                       *devp != '#' && *devp != '\n' &&
                       di < (int)sizeof(dev) - 1) {
                    dev[di++] = *devp++;
                }
                dev[di] = '\0';

                /* only add if no S/N entry was already added for this board */
                if (dev[0] && ids[nids > 0 ? nids - 1 : 0].dev_path[0] == '\0'
                    && (nids == 0 ||
                        strcmp(ids[nids-1].board_name, current_board) != 0)) {
                    strlcpy_safe(ids[nids].dev_path, dev,
                                sizeof(ids[nids].dev_path));
                    ids[nids].serial[0] = '\0';
                    strlcpy_safe(ids[nids].board_name, current_board,
                                sizeof(ids[nids].board_name));
                    ids[nids].baud = current_baud;
                    nids++;
                }
            }
        }
    }

    fclose(fp);
    return nids;
}

void
apply_board_config(tty_port_t *ports, int nports,
                   board_id_t *ids, int nids)
{
    for (int i = 0; i < nports; i++) {
        for (int j = 0; j < nids; j++) {
            int match = 0, match_by_serial = 0;
            /* match by serial number */
            if (ids[j].serial[0] && ports[i].serial[0] &&
                strcmp(ports[i].serial, ids[j].serial) == 0) {
                match = 1;
                match_by_serial = 1;
            }
            /* match by device path */
            if (ids[j].dev_path[0] &&
                strcmp(ports[i].dev_path, ids[j].dev_path) == 0) {
                match = 1;
            }
            if (match) {
                /* For device-path-only matches, verify compatibility with
                 * VID:PID.  Device paths are unstable and shift when devices
                 * are added or removed, so a stale path can point to the
                 * wrong device.  Serial-number matches are authoritative.
                 * Skip this check when the board config has a custom baud
                 * rate -- the user explicitly assigned this device path. */
                if (!match_by_serial && ports[i].known &&
                    ids[j].baud == 0) {
                    int compat = 0;
                    for (int b = 0; b < MAX_BOARDS_PER_DEVICE &&
                                    ports[i].known->boards[b]; b++) {
                        if (strcmp(ports[i].known->boards[b],
                                  ids[j].board_name) == 0) {
                            compat = 1;
                            break;
                        }
                    }
                    if (!compat)
                        continue;   /* stale path – skip */
                }
                ports[i].board_override = ids[j].board_name;
                if (ids[j].baud > 0)
                    ports[i].baud = ids[j].baud;
                /* regenerate label with the board override */
                get_device_label(&ports[i]);
                break;
            }
        }
    }
}

void
print_port_table(device_group_t *groups, int ngroups, int verbose)
{
    printf("\n");
    for (int i = 0; i < 100; i++) putchar('=');
    printf("\nUSB Serial Port Inventory - Grouped by Device\n");
    for (int i = 0; i < 100; i++) putchar('=');
    printf("\n");

    if (ngroups == 0) {
        printf("No USB serial ports found!\n");
        return;
    }

    for (int g = 0; g < ngroups; g++) {
        device_group_t *grp = &groups[g];
        tty_port_t *first = grp->ports[0];

        printf("\n");
        for (int i = 0; i < 100; i++) putchar('=');
        printf("\nDevice #%d: %s - %s\n",
               g + 1, first->manufacturer, first->product);
        for (int i = 0; i < 100; i++) putchar('=');
        printf("\n");

        printf("  VID:PID       : %04x:%04x\n", first->vid, first->pid);
        printf("  Device Type   : %s\n",
               first->known ? first->known->name : "Unknown");

        /* possible boards */
        printf("  Possible Board: ");
        if (first->board_override) {
            printf("%s", first->board_override);
        } else if (first->board_match) {
            printf("%s", first->board_match);
        } else if (first->known) {
            for (int b = 0; b < MAX_BOARDS_PER_DEVICE &&
                            first->known->boards[b]; b++) {
                if (b > 0) printf(", ");
                printf("%s", first->known->boards[b]);
            }
        } else {
            printf("Unknown");
        }
        printf("\n");

        if (first->serial[0])
            printf("  Serial Number : %s\n", first->serial);
        printf("  USB Path      : %s\n", first->usb_path);
        printf("  Port Count    : %d/%d\n",
               grp->port_count,
               first->known ? first->known->expected_ports : grp->port_count);

        printf("\n  %-15s %-7s %-25s %-8s\n",
               "Port", "Iface", "Function", "Access");
        printf("  %-15s %-7s %-25s %-8s\n",
               "---------------", "-------",
               "-------------------------", "--------");

        for (int p = 0; p < grp->port_count; p++) {
            tty_port_t *port = grp->ports[p];
            const char *func = port->function_name ?
                               port->function_name : "Unknown";
            char access_str[8] = "";
            if (access(port->dev_path, R_OK) == 0)
                strcat(access_str, "R");
            if (access(port->dev_path, W_OK) == 0)
                strcat(access_str, "W");
            if (access_str[0] == '\0')
                strlcpy_safe(access_str, "---", sizeof(access_str));

            printf("  %-15s %-7d %-25s %-8s\n",
                   port->dev_path, port->interface_num, func, access_str);
        }

        if (verbose) {
            printf("\n  Labels:\n");
            for (int p = 0; p < grp->port_count; p++) {
                tty_port_t *port = grp->ports[p];
                printf("    %s -> %s\n", port->dev_path, port->label);

                /* show log file path if it exists */
                char logpath[512];
                snprintf(logpath, sizeof(logpath),
                         LOG_BASE_DIR "/latest/%s.log", port->label);
                if (access(logpath, F_OK) == 0)
                    printf("      Log: %s\n", logpath);

                /* show PTY proxy path if it exists */
                char ptypath[512];
                char ptytarget[256];
                snprintf(ptypath, sizeof(ptypath),
                         LOG_BASE_DIR "/pty/%s", port->label);
                ssize_t len = readlink(ptypath, ptytarget,
                                       sizeof(ptytarget) - 1);
                if (len > 0) {
                    ptytarget[len] = '\0';
                    printf("      PTY: %s -> %s\n", ptypath, ptytarget);
                }
            }
        }
    }
}

int
cmd_identify(int argc, char *argv[])
{
    int verbose = 0;
    int save = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "--save") == 0)
            save = 1;
    }

    /* scan */
    tty_port_t ports[MAX_PORTS];
    int nports = scan_all_ports(ports, MAX_PORTS);

    /* load board config and apply overrides */
    board_id_t bids[MAX_BOARD_IDS];
    int nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(ports, nports, bids, nbids);

    /* group and print */
    device_group_t groups[MAX_GROUPS];
    memset(groups, 0, sizeof(groups));
    int ngroups = group_ports(ports, nports, groups, MAX_GROUPS);

    print_port_table(groups, ngroups, verbose);
    printf("\n");

    if (save) {
        /* TODO: save_board_config() -- for now just note */
        printf("(--save not yet implemented in C version, "
               "use python identify_tty_ports.py --save)\n");
    }

    return 0;
}
