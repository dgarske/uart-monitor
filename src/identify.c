/* identify.c -- USB serial port scanning and identification.
 *
 * Ported from identify_tty_ports.py. Reads sysfs directly, no udevadm.
 * This tool NEVER writes to serial ports.
 */
#include "identify.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    /* If known device with a board name */
    if (port->known && port->known->boards[0]) {
        const char *board = port->known->boards[0];
        char clean[48];
        strlcpy_safe(clean, board, sizeof(clean));
        for (char *p = clean; *p; p++) {
            if (*p == ' ') *p = '_';
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        if (port->known->expected_ports > 1) {
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
            }
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
        if (ports[i].serial[0] == '\0')
            continue;
        for (int j = 0; j < nids; j++) {
            if (strcmp(ports[i].serial, ids[j].serial) == 0) {
                ports[i].board_override = ids[j].board_name;
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
                printf("    %s -> %s\n",
                       grp->ports[p]->dev_path,
                       grp->ports[p]->label);
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
