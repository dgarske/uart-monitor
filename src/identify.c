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
#include "platform.h"
#include "log.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
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

/* Sized to comfortably hold every ST-LINK that fits on a typical USB
 * hub fan-out (4-port hub * 4 levels). The daemon must keep ALL probes
 * in the cache because a newly hot-plugged device may appear at any
 * index in `st-info --probe` output; if it gets dropped, identify_port
 * falls back to the generic "STM32_ST_LINK_UART_<sn>" label and the
 * sticky cache never learns the real board name. */
#define STLINK_PROBE_CACHE_SZ 64
#define STLINK_PROBE_CACHE_TTL_SEC 2
static stlink_probe_entry_t stlink_probe_cache[STLINK_PROBE_CACHE_SZ];
static int stlink_probe_cache_count = 0;
static int stlink_probe_cache_populated = 0;
static time_t stlink_probe_cache_time = 0;

/* Arena for probe-derived board names. The tty_port_t->board_match field
 * is `const char *` and expects pointers that live for the process, so
 * we can't hand out stack buffers. Sized to comfortably hold a unique
 * NUCLEO-* name for every entry in the per-scan probe caches.
 *
 * Entries are interned, not merely appended: the same board is re-probed
 * on every hot-plug event, and a port that flaps (a board whose power is
 * off, a failing cable) re-probes every couple of seconds. Appending a
 * duplicate per probe would exhaust the arena within hours, after which
 * intern_board_name() returns NULL and EVERY port's probe result silently
 * degrades to a generic label -- one dead board corrupting the labels of
 * the whole bench. The set of distinct board names is small and bounded,
 * so a linear scan for an existing copy keeps the arena flat. */
#define PROBE_NAME_ARENA_SZ 4096
static char probe_name_arena[PROBE_NAME_ARENA_SZ];
static size_t probe_name_arena_used = 0;
static int probe_name_arena_warned = 0;

const char *
intern_board_name(const char *name)
{
    size_t need;
    size_t off;
    char *slot;

    if (!name)
        return NULL;

    /* Already interned? Hand back the existing copy. */
    for (off = 0; off < probe_name_arena_used;
         off += strlen(probe_name_arena + off) + 1) {
        if (strcmp(probe_name_arena + off, name) == 0)
            return probe_name_arena + off;
    }

    need = strlen(name) + 1;
    if (probe_name_arena_used + need > sizeof(probe_name_arena)) {
        if (!probe_name_arena_warned) {
            probe_name_arena_warned = 1;
            fprintf(stderr, "identify: board-name arena full (%zu bytes); "
                    "further probe results will fall back to generic "
                    "labels\n", sizeof(probe_name_arena));
        }
        return NULL;
    }
    slot = probe_name_arena + probe_name_arena_used;
    memcpy(slot, name, need);
    probe_name_arena_used += need;
    return slot;
}

size_t
intern_board_name_used(void)
{
    return probe_name_arena_used;
}

/* Forward decl: defined below the STM32CubeProgrammer probe block. */
static void stm32_prog_cache_reset(void);

/* Reset caches at the start of each full scan (see scan_all_ports). */
static void
stlink_probe_cache_reset(void)
{
    stlink_probe_cache_count = 0;
    stlink_probe_cache_populated = 0;
    stlink_probe_cache_time = 0;
    stm32_prog_cache_reset();
}

/* Public wrapper used by the daemon's hot-plug handler. */
void
identify_reset_probe_caches(void)
{
    stlink_probe_cache_reset();
}

/* -------------------------------------------------------------------- */
/* Sticky identity cache, keyed by USB serial number.                   */
/*                                                                      */
/* The per-scan probe caches above are wiped on every full rescan /     */
/* hotplug event so that fresh st-info / STM32_Programmer_CLI results   */
/* are picked up. That makes us correct, but fragile: if a probe races  */
/* USB enumeration (12+ STLINKs on the bus, target in reset, etc.) we   */
/* fall through to the generic STM32_STLINK_V3_UART_<sn> label and stay */
/* there until daemon restart.                                          */
/*                                                                      */
/* Once we have resolved a serial to a real board_match, that mapping   */
/* is physically immutable for the life of the device on the bus, so    */
/* we remember it process-wide. A later identify_port() for the same    */
/* serial whose live probe failed will fall back to the sticky entry    */
/* instead of the generic label.                                        */
/* -------------------------------------------------------------------- */

typedef struct {
    char serial[64];
    char usb_path[128];      /* USB topology path, e.g. "1-6.2" */
    const char *board_match; /* interned in probe_name_arena */
    time_t stamp;            /* last refresh, for the topology-keyed TTL */
} sticky_identity_t;

#define STICKY_IDENTITY_SZ 64

/* How long a topology-keyed (serial-less) entry stays valid. A USB serial
 * is globally unique, so a serial-keyed entry can only ever be matched by
 * the same physical device and never expires. A hub port is not unique
 * over time -- unplug one serial-less adapter and plug in another, and the
 * topology path is identical -- so those entries age out. */
#define STICKY_IDENTITY_PATH_TTL_SEC 3600

static sticky_identity_t sticky_identity[STICKY_IDENTITY_SZ];
static int sticky_identity_count = 0;

/* Look up a remembered board name.
 *
 * A device with a USB serial is matched on that serial alone. It must NOT
 * fall back to the topology path: a *different* board plugged into a hub
 * port previously used by another board would otherwise inherit the old
 * board's name whenever its own probe failed, which is precisely the
 * stale-label failure this cache is supposed to prevent.
 *
 * The topology fallback exists for devices that report no serial at all
 * (e.g. an FT4232H strapped with SerialNumber=0), which have no other
 * stable key. Those match only against other serial-less entries, and
 * only within the TTL. */
static const char *
sticky_identity_lookup(const char *serial, const char *usb_path)
{
    int i;
    time_t now;

    if (serial && serial[0] != '\0') {
        for (i = 0; i < sticky_identity_count; i++) {
            if (sticky_identity[i].serial[0] != '\0' &&
                strcmp(sticky_identity[i].serial, serial) == 0)
                return sticky_identity[i].board_match;
        }
        return NULL;
    }

    if (usb_path && usb_path[0] != '\0') {
        now = time(NULL);
        for (i = 0; i < sticky_identity_count; i++) {
            if (sticky_identity[i].serial[0] != '\0')
                continue;   /* belongs to a device that has a serial */
            if (sticky_identity[i].usb_path[0] == '\0' ||
                strcmp(sticky_identity[i].usb_path, usb_path) != 0)
                continue;
            if (now - sticky_identity[i].stamp >
                STICKY_IDENTITY_PATH_TTL_SEC)
                continue;   /* too old to trust for a reusable hub port */
            return sticky_identity[i].board_match;
        }
    }
    return NULL;
}

static void
sticky_identity_remember(const char *serial, const char *usb_path,
                         const char *board_match)
{
    int i, oldest;
    int have_serial = (serial != NULL && serial[0] != '\0');
    int have_path = (usb_path != NULL && usb_path[0] != '\0');

    if (!board_match || (!have_serial && !have_path))
        return;

    for (i = 0; i < sticky_identity_count; i++) {
        int hit;
        if (have_serial) {
            hit = (sticky_identity[i].serial[0] != '\0' &&
                   strcmp(sticky_identity[i].serial, serial) == 0);
        } else {
            hit = (sticky_identity[i].serial[0] == '\0' &&
                   strcmp(sticky_identity[i].usb_path, usb_path) == 0);
        }
        if (hit) {
            sticky_identity[i].board_match = board_match;
            if (have_path)
                strlcpy_safe(sticky_identity[i].usb_path, usb_path,
                             sizeof(sticky_identity[i].usb_path));
            sticky_identity[i].stamp = time(NULL);
            return;
        }
    }

    if (sticky_identity_count < STICKY_IDENTITY_SZ) {
        oldest = sticky_identity_count++;
    } else {
        /* Full: evict the least recently refreshed entry rather than
         * silently dropping every new identity for the daemon's life. */
        oldest = 0;
        for (i = 1; i < sticky_identity_count; i++) {
            if (sticky_identity[i].stamp < sticky_identity[oldest].stamp)
                oldest = i;
        }
    }
    memset(&sticky_identity[oldest], 0, sizeof(sticky_identity[oldest]));
    if (have_serial)
        strlcpy_safe(sticky_identity[oldest].serial, serial,
                     sizeof(sticky_identity[0].serial));
    if (have_path)
        strlcpy_safe(sticky_identity[oldest].usb_path, usb_path,
                     sizeof(sticky_identity[0].usb_path));
    sticky_identity[oldest].board_match = board_match;
    sticky_identity[oldest].stamp = time(NULL);
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

/* -------------------------------------------------------------------- */
/* STM32CubeProgrammer CLI probe                                        */
/*                                                                      */
/* `STM32_Programmer_CLI --list` reads the board name directly from     */
/* the ST-LINK firmware EEPROM (no SWD transaction required) and        */
/* returns full NUCLEO names (e.g. "NUCLEO-U575ZI-Q"). Far more robust  */
/* than st-info DBGMCU IDCODE probing because it works even when the    */
/* target MCU is held in reset, in low-power, or has no firmware.       */
/* When the CLI is not installed we silently fall back to st-info.      */
/* -------------------------------------------------------------------- */

typedef struct {
    char serial[64];
    char board[64];
} stm32_prog_entry_t;

/* Match STLINK_PROBE_CACHE_SZ so the CubeProgrammer path never silently
 * drops a probe before st-info gets a chance. */
#define STM32_PROG_CACHE_SZ 64
#define STM32_PROG_CACHE_TTL_SEC 2
static stm32_prog_entry_t stm32_prog_cache[STM32_PROG_CACHE_SZ];
static int stm32_prog_cache_count = 0;
static int stm32_prog_cache_populated = 0;
static time_t stm32_prog_cache_time = 0;

/* Path to STM32_Programmer_CLI, found once per process and cached.
 * Empty string = not installed (negative cache). */
static char stm32_prog_cli_path[512] = {0};
static int stm32_prog_cli_searched = 0;

static void
stm32_prog_cache_reset(void)
{
    stm32_prog_cache_count = 0;
    stm32_prog_cache_populated = 0;
    stm32_prog_cache_time = 0;
    /* don't reset the path cache -- the binary location doesn't move */
}

/* Try a candidate path and return 1 if it's executable. */
static int
try_cli_path(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (access(path, X_OK) != 0)
        return 0;
    strlcpy_safe(stm32_prog_cli_path, path, sizeof(stm32_prog_cli_path));
    return 1;
}

/* Locate STM32_Programmer_CLI. Returns path or NULL if not installed.
 * Search order:
 *   1. $STM32_PROGRAMMER_CLI environment override
 *   2. $PATH (via "command -v")
 *   3. ~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/...
 *   4. /opt/STMicroelectronics/.../bin/...
 *   5. /usr/local/STMicroelectronics/.../bin/... */
static const char *
find_stm32_programmer_cli(void)
{
    if (stm32_prog_cli_searched)
        return stm32_prog_cli_path[0] ? stm32_prog_cli_path : NULL;
    stm32_prog_cli_searched = 1;

    const char *env = getenv("STM32_PROGRAMMER_CLI");
    if (try_cli_path(env))
        return stm32_prog_cli_path;

    /* $PATH lookup via popen("command -v ...") */
    FILE *fp = popen("command -v STM32_Programmer_CLI 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof(buf), fp)) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            if (try_cli_path(buf)) {
                pclose(fp);
                return stm32_prog_cli_path;
            }
        }
        pclose(fp);
    }

    /* Common install dirs */
    const char *home = getenv("HOME");
    if (home) {
        char candidate[512];
        snprintf(candidate, sizeof(candidate),
                 "%s/STMicroelectronics/STM32Cube/STM32CubeProgrammer"
                 "/bin/STM32_Programmer_CLI", home);
        if (try_cli_path(candidate))
            return stm32_prog_cli_path;
    }
    if (try_cli_path("/opt/STMicroelectronics/STM32Cube/"
                     "STM32CubeProgrammer/bin/STM32_Programmer_CLI"))
        return stm32_prog_cli_path;
    if (try_cli_path("/usr/local/STMicroelectronics/STM32Cube/"
                     "STM32CubeProgrammer/bin/STM32_Programmer_CLI"))
        return stm32_prog_cli_path;

    return NULL;   /* not installed */
}

/* Strip ANSI CSI escape sequences (ESC '[' ... letter) in place.
 * STM32_Programmer_CLI emits color codes by default; we don't want
 * them in our parsed strings. */
static void
strip_ansi(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && !((*r >= 'A' && *r <= 'Z') ||
                           (*r >= 'a' && *r <= 'z')))
                r++;
            if (*r) r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Trim leading whitespace and trailing whitespace/CR/LF in place. */
static void
trim_inplace(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

/* Populate per-S/N board name table by running STM32_Programmer_CLI.
 * Returns 0 on success (incl. zero entries), -1 if the CLI is not
 * installed or the subprocess failed. */
static int
stm32_prog_cache_populate(void)
{
    time_t now = time(NULL);
    if (stm32_prog_cache_populated &&
        now - stm32_prog_cache_time < STM32_PROG_CACHE_TTL_SEC)
        return stm32_prog_cache_count > 0 ? 0 : -1;
    stm32_prog_cache_populated = 1;
    stm32_prog_cache_time = now;
    stm32_prog_cache_count = 0;

    const char *cli = find_stm32_programmer_cli();
    if (!cli)
        return -1;

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "\"%s\" --list 2>/dev/null", cli);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    char line[512];
    char cur_serial[64] = {0};
    char cur_board[64] = {0};
    int in_probe_block = 0;

    /* Helper macro: flush partial block into cache if complete. */
    #define STM32_PROG_FLUSH() do {                                  \
        if (cur_serial[0] && cur_board[0] && cur_board[0] != '-' && \
            stm32_prog_cache_count < STM32_PROG_CACHE_SZ) {         \
            stm32_prog_entry_t *e =                                  \
                &stm32_prog_cache[stm32_prog_cache_count++];         \
            strlcpy_safe(e->serial, cur_serial, sizeof(e->serial)); \
            strlcpy_safe(e->board, cur_board, sizeof(e->board));    \
        }                                                            \
        cur_serial[0] = '\0';                                        \
        cur_board[0] = '\0';                                         \
    } while (0)

    while (fgets(line, sizeof(line), fp)) {
        strip_ansi(line);
        trim_inplace(line);

        /* Block start: "ST-Link Probe N :" or "ST-LINK Probe N :"
         * Marks the beginning of a probe entry inside the
         * "Connected ST-LINK Probes List" section. */
        if ((strncmp(line, "ST-Link Probe", 13) == 0 ||
             strncmp(line, "ST-LINK Probe", 13) == 0) &&
            strchr(line, ':')) {
            STM32_PROG_FLUSH();
            in_probe_block = 1;
            continue;
        }

        /* End-of-list delimiter: a row of dashes closes the probe
         * list. The CLI's later sections ("UART Interface", etc.)
         * use the same Board Name / ST-LINK SN keys but in reversed
         * order which would corrupt our state machine, so stop
         * parsing once the probe list ends. */
        if (line[0] == '-' && strspn(line, "-") >= 5) {
            STM32_PROG_FLUSH();
            in_probe_block = 0;
            continue;
        }

        if (!in_probe_block)
            continue;

        const char *colon = strchr(line, ':');
        if (!colon)
            continue;

        if (strncmp(line, "ST-LINK SN", 10) == 0 ||
            strncmp(line, "ST-Link SN", 10) == 0) {
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            strlcpy_safe(cur_serial, v, sizeof(cur_serial));
            trim_inplace(cur_serial);
        } else if (strncmp(line, "Board Name", 10) == 0) {
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            strlcpy_safe(cur_board, v, sizeof(cur_board));
            trim_inplace(cur_board);
        }
    }

    /* flush trailing block (no end-of-list dashes seen) */
    STM32_PROG_FLUSH();
    #undef STM32_PROG_FLUSH

    int rc = pclose(fp);
    /* CLI exit code is unreliable (returns non-zero when any single
     * probe fails). Trust whatever entries we parsed. */
    (void)rc;

    return stm32_prog_cache_count > 0 ? 0 : -1;
}

/* Look up board name from STM32_Programmer_CLI by S/N. Returns 0 +
 * fills board_out on success, -1 if not found / CLI not installed. */
static int
probe_stm32_programmer(const char *serial, char *board_out, size_t board_sz)
{
    if (!serial || !serial[0])
        return -1;
    if (stm32_prog_cache_populate() < 0)
        return -1;
    for (int i = 0; i < stm32_prog_cache_count; i++) {
        if (strcmp(stm32_prog_cache[i].serial, serial) == 0) {
            strlcpy_safe(board_out, stm32_prog_cache[i].board, board_sz);
            return 0;
        }
    }
    return -1;
}

/* Core device identification shared by identify_port_cheap() and
 * identify_port(): fills vid/pid/serial/product/known and the
 * product-string board match via the platform USB backend. Runs NO
 * external probe and sets no label, so it touches none of the probe-cache
 * statics and is safe to call from the daemon's main thread. */
static int
identify_port_core(const char *dev_path, tty_port_t *port)
{
    memset(port, 0, sizeof(*port));
    strlcpy_safe(port->dev_path, dev_path, sizeof(port->dev_path));

    /* extract tty name from path */
    const char *slash = strrchr(dev_path, '/');
    strlcpy_safe(port->tty_name, slash ? slash + 1 : dev_path,
                 sizeof(port->tty_name));

    /* read vid/pid/serial/manufacturer/product/interface_num/usb_path
     * from the platform USB backend (sysfs on Linux, IOKit on macOS).
     * A failure means the device has no USB backing (gone / virtual). */
    if (plat_read_usb_info(port) != 0)
        return -1;

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
    return 0;
}

/* Finish identification: assign the function name and the human-readable
 * label. Shared by identify_port_cheap() and identify_port(). */
static void
identify_finish(tty_port_t *port)
{
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
}

/* Cheap identification: sysfs only, no external probe. Touches no
 * probe-cache statics, so it is safe on the daemon's main epoll thread.
 * Ambiguous ST-LINK devices get the generic fallback label here; the
 * identify worker resolves the real board name later. */
int
identify_port_cheap(const char *dev_path, tty_port_t *port)
{
    if (identify_port_core(dev_path, port) != 0)
        return -1;
    identify_finish(port);
    return 0;
}

/* Full identification: core sysfs read plus, for ambiguous known
 * devices, an external SWD/CLI probe to resolve the real board name.
 * The probe shells out (st-info / STM32_Programmer_CLI) and mutates the
 * probe-cache statics, so inside the daemon this must run ONLY on the
 * identify worker thread, never the main epoll thread. */
int
identify_port(const char *dev_path, tty_port_t *port)
{
    if (identify_port_core(dev_path, port) != 0)
        return -1;

    /* For ambiguous devices (VID:PID shared by multiple boards / MCU
     * families like the STLINK-V3), probe to resolve the actual board.
     * Try STM32_Programmer_CLI first -- it returns the full NUCLEO
     * board name and works even when the target is in reset / sleep.
     * Fall back to st-info DBGMCU IDCODE probing if the CLI is not
     * installed. Best-effort: silently falls through on any failure. */
    if (port->known && !port->board_match && port->serial[0] &&
        known_device_is_ambiguous(port->known)) {
        char probed[64];
        if (probe_stm32_programmer(port->serial, probed,
                                   sizeof(probed)) == 0)
            port->board_match = intern_board_name(probed);
        else if (probe_stlink_chipid(port->serial, probed,
                                     sizeof(probed)) == 0)
            port->board_match = intern_board_name(probed);

        /* Probe succeeded: remember the (serial -> board) mapping so a
         * later transient probe failure (e.g. raced hot-plug, target in
         * reset) can recover without downgrading the label. Probe
         * failed: consult the sticky cache before falling through to
         * the generic STM32_STLINK_V3_UART_<sn> fallback. */
        if (port->board_match)
            sticky_identity_remember(port->serial, port->usb_path,
                                     port->board_match);
        else
            port->board_match = sticky_identity_lookup(port->serial,
                                                       port->usb_path);
    }

    identify_finish(port);
    return 0;
}

int
scan_all_ports(tty_port_t *ports, int max_ports)
{
    glob_t g;
    int n = 0;

    memset(&g, 0, sizeof(g));

    /* Reset per-scan probe cache so stale hotplug state doesn't leak. */
    stlink_probe_cache_reset();

    plat_glob_serial_ports(&g);

    for (size_t i = 0; i < g.gl_pathc && n < max_ports; i++) {
        if (identify_port(g.gl_pathv[i], &ports[n]) == 0)
            n++;
    }

    globfree(&g);
    return n;
}

/* Same enumeration as scan_all_ports() but uses identify_port_cheap() --
 * no external probe, no probe-cache reset. Safe to call on the daemon's
 * main epoll thread (e.g. from write_status_json / SIGHUP rescan) while
 * the identify worker owns the probe path. Ambiguous ST-LINK ports come
 * back with their generic fallback labels. */
int
scan_all_ports_cheap(tty_port_t *ports, int max_ports)
{
    glob_t g;
    int n = 0;

    memset(&g, 0, sizeof(g));

    plat_glob_serial_ports(&g);

    for (size_t i = 0; i < g.gl_pathc && n < max_ports; i++) {
        if (identify_port_cheap(g.gl_pathv[i], &ports[n]) == 0)
            n++;
    }

    globfree(&g);
    return n;
}

/* Normalize a board name into a filesystem-safe label fragment:
 * spaces and '-' become '_', lowercase letters become uppercase,
 * other ASCII non-alnum becomes '_'. */
static void
sanitize_label(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 32;
            continue;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
            continue;
        *p = '_';
    }
}

/* Build "_<key>" from the most stable identifier the port has, for use as
 * a label suffix when the board itself could not be resolved and several
 * unresolved devices would otherwise share one name.
 *
 * Order matters, because the label names the log file and the PTY symlink
 * and both must survive a re-enumeration:
 *   serial   - unique and stable. Preferred whenever present.
 *   usb_path - the hub port. Stable while the cable stays put, and the
 *              only stable key a serial-less adapter has.
 *   tty_name - last resort only. The kernel recycles minor numbers, so a
 *              tty-derived label both renames itself on every replug and
 *              can be inherited by an unrelated board later.
 * Writes an empty string only if the port has none of the three. */
static void
append_stable_suffix(char *suffix, size_t sz, const tty_port_t *port)
{
    suffix[0] = '\0';

    if (port->serial[0]) {
        size_t slen = strlen(port->serial);
        const char *tail = port->serial + (slen > 8 ? slen - 8 : 0);
        snprintf(suffix, sz, "_%.22s", tail);
    } else if (port->usb_path[0]) {
        snprintf(suffix, sz, "_%.22s", port->usb_path);
    } else if (port->tty_name[0]) {
        snprintf(suffix, sz, "_%.22s", port->tty_name);
    }
    sanitize_label(suffix);
}

/* Human-readable board name for a port, most specific source first: a
 * ~/.boards pin, then a USB-product-string or probe match, then the first
 * board listed for the USB device. Callers that report a board name must
 * use this rather than open-coding the chain -- status.json and the log
 * header previously each had their own and disagreed. */
const char *
get_board_name(const tty_port_t *port)
{
    if (port->board_override[0])
        return port->board_override;
    if (port->board_match)
        return port->board_match;
    /* boards[0] is only a guess when the VID:PID is shared by several
     * boards, and reporting a guess as fact is worse than admitting we
     * don't know: an unpinned FT4232H cable was being reported as a
     * "VMK180" while the real VMK180 sat on a different USB device. The
     * label path already declines to name the board in this case, so
     * returning "Unknown" also keeps status.json and the log header
     * consistent with the log file name. */
    if (port->known && port->known->boards[0] &&
        !known_device_is_ambiguous(port->known))
        return port->known->boards[0];
    return "Unknown";
}

void
get_device_label(tty_port_t *port)
{
    /* If we have a board override and known device, use board + UART + iface.
     * Mirror the board_match path below: only single-UART devices (or
     * unknown ones) get a bare "_UART" -- multi-port devices keep the
     * interface number. Without this a ~/.boards serial pin for a
     * single-UART board (e.g. NUCLEO-H563ZI) would produce
     * "NUCLEO_H563ZI_UART0", not the "NUCLEO_H563ZI_UART" the probe path
     * produces, breaking label continuity across a probe failure. */
    if (port->board_override[0]) {
        char board[48];
        strlcpy_safe(board, port->board_override, sizeof(board));
        sanitize_label(board);
        if (port->known && port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART%d", board, port->interface_num);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART", board);
        }
        return;
    }

    /* If board matched by USB product string or by active probe
     * (STM32_Programmer_CLI / st-info chipid) */
    if (port->board_match) {
        char clean[48];
        strlcpy_safe(clean, port->board_match, sizeof(clean));
        sanitize_label(clean);
        if (port->known && port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART%d", clean, port->interface_num);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART", clean);
        }
        return;
    }

    /* Ambiguous device with no resolved board: probe failed (target in
     * reset, no SWD, CLI not installed, etc.).  Use the known_device
     * name plus the interface number and the best stable key available,
     * so multiple unresolved boards don't collide.
     * E.g. STM32_VIRTUAL_COM_PORT_UART_38363431, FTDI_FT4232H_UART2_1_7_2.
     *
     * The interface number is not optional here. A serial-less multi-port
     * bridge (an FT4232H strapped with SerialNumber=0) has nothing else to
     * tell its four interfaces apart, and without it all four collapse
     * onto one label -- one log file carrying four interleaved consoles,
     * and three PTY symlinks silently overwritten by the fourth. */
    if (known_device_is_ambiguous(port->known)) {
        char clean[48];
        char suffix[24];
        strlcpy_safe(clean, port->known->name, sizeof(clean));
        sanitize_label(clean);
        append_stable_suffix(suffix, sizeof(suffix), port);
        if (port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.20s_UART%d%.22s", clean, port->interface_num,
                     suffix);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.24s_UART%.22s", clean, suffix);
        }
        return;
    }

    /* If known device with a board name */
    if (port->known && port->known->boards[0]) {
        const char *board = port->known->boards[0];
        char clean[48];
        strlcpy_safe(clean, board, sizeof(clean));
        sanitize_label(clean);
        if (strcmp(clean, "GENERIC") == 0) {
            /* Generic devices: append the best stable key so several
             * unidentified adapters don't collide, and so the label does
             * not change when the tty number does. */
            char suffix[24];
            append_stable_suffix(suffix, sizeof(suffix), port);
            snprintf(port->label, sizeof(port->label),
                     "%.24s_UART%.22s", clean, suffix);
        } else if (port->known->expected_ports > 1) {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART%d", clean, port->interface_num);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.48s_UART", clean);
        }
        return;
    }

    /* Device is not in KNOWN_DEVICES at all. Build from whatever the USB
     * descriptors offer plus the best stable key, so the label still
     * survives a re-enumeration; fall back to the bare tty name only when
     * there is no USB product string either (a real serial port, say). */
    if (port->product[0]) {
        char clean[48];
        char suffix[24];
        strlcpy_safe(clean, port->product, sizeof(clean));
        sanitize_label(clean);
        append_stable_suffix(suffix, sizeof(suffix), port);
        if (port->interface_num > 0) {
            snprintf(port->label, sizeof(port->label),
                     "%.20s_UART%d%.22s", clean, port->interface_num,
                     suffix);
        } else {
            snprintf(port->label, sizeof(port->label),
                     "%.24s_UART%.22s", clean, suffix);
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
    /* Index of the entry this section has already emitted, or -1. Each
     * "# === Board ===" section contributes at most one pin; a stronger
     * key (serial / topology) overwrites a device-path pin in the same
     * section. Tracking it per section rather than by inspecting the
     * previously emitted entry is what lets two consecutive path-pinned
     * sections both load -- the old heuristic silently dropped the
     * second one. */
    int section_pin = -1;

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
            section_pin = -1;
            continue;
        }

        /* look for: # Baud: <rate> */
        if (current_board[0] && strncmp(trimmed, "# Baud:", 7) == 0) {
            const char *val = trimmed + 7;
            while (*val == ' ') val++;
            current_baud = atoi(val);
            if (section_pin >= 0)
                ids[section_pin].baud = current_baud;
            continue;
        }

        /* look for: # USB: <path>  (optionally followed by | S/N: <serial>)
         *
         * Both keys are precise, so this line always claims the section's
         * pin slot, overwriting a device-path pin picked up earlier in the
         * same section. A section that gives only a topology path still
         * yields a usable pin -- that is the only stable key available for
         * an adapter that reports no serial. */
        if (current_board[0] && strstr(line, "# USB:")) {
            const char *up = strstr(line, "# USB:") + 6;
            const char *sn = strstr(line, "S/N:");
            char usb_path[128];
            char serial[64];
            int si = 0;

            while (*up == ' ') up++;
            while (*up && *up != '\n' && *up != '\r' && *up != ' ' &&
                   *up != '|' && si < (int)sizeof(usb_path) - 1) {
                usb_path[si++] = *up++;
            }
            usb_path[si] = '\0';

            serial[0] = '\0';
            if (sn) {
                sn += 4;
                while (*sn == ' ') sn++;
                si = 0;
                while (*sn && *sn != '\n' && *sn != '\r' && *sn != ' ' &&
                       si < (int)sizeof(serial) - 1) {
                    serial[si++] = *sn++;
                }
                serial[si] = '\0';
            }

            if (serial[0] || usb_path[0]) {
                int slot = (section_pin >= 0) ? section_pin : nids;
                memset(&ids[slot], 0, sizeof(ids[slot]));
                strlcpy_safe(ids[slot].serial, serial,
                             sizeof(ids[slot].serial));
                strlcpy_safe(ids[slot].usb_path, usb_path,
                             sizeof(ids[slot].usb_path));
                strlcpy_safe(ids[slot].board_name, current_board,
                             sizeof(ids[slot].board_name));
                ids[slot].baud = current_baud;
                if (section_pin < 0) {
                    section_pin = nids;
                    nids++;
                }
            }
            continue;
        }

        /* look for: LABEL=/dev/ttyUSBN or LABEL=/dev/cu.usbserial-XX
         * (device path assignment). Only the first such line in a section
         * counts, and only when the section has offered no stronger key --
         * the remaining LABEL= lines are the operator's notes on the other
         * interfaces of the same physical device, and the override is
         * applied per device, not per interface. */
        if (current_board[0] && section_pin < 0 &&
            trimmed[0] != '#' && trimmed[0] != '\n') {
            char *eq = strchr(trimmed, '=');
            if (eq && strncmp(eq + 1, "/dev/", 5) == 0) {
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

                if (dev[0]) {
                    memset(&ids[nids], 0, sizeof(ids[nids]));
                    strlcpy_safe(ids[nids].dev_path, dev,
                                sizeof(ids[nids].dev_path));
                    strlcpy_safe(ids[nids].board_name, current_board,
                                sizeof(ids[nids].board_name));
                    ids[nids].baud = current_baud;
                    section_pin = nids;
                    nids++;
                }
            }
        }
    }

    fclose(fp);
    return nids;
}

/* apply_board_config() runs on every status.json write, so a rejected pin
 * must not print on every pass. Remember which (pin, board) pairs have
 * already been reported; the table is tiny and the daemon is long-lived. */
#define STALE_PIN_WARN_SZ 32
static struct {
    char dev_path[256];
    char board_name[128];
} stale_pin_warned[STALE_PIN_WARN_SZ];
static int stale_pin_warned_count = 0;

static void
warn_stale_pin_once(const board_id_t *id, const tty_port_t *port)
{
    int i;

    for (i = 0; i < stale_pin_warned_count; i++) {
        if (strcmp(stale_pin_warned[i].dev_path, id->dev_path) == 0 &&
            strcmp(stale_pin_warned[i].board_name, id->board_name) == 0)
            return;
    }
    if (stale_pin_warned_count < STALE_PIN_WARN_SZ) {
        strlcpy_safe(stale_pin_warned[stale_pin_warned_count].dev_path,
                     id->dev_path,
                     sizeof(stale_pin_warned[0].dev_path));
        strlcpy_safe(stale_pin_warned[stale_pin_warned_count].board_name,
                     id->board_name,
                     sizeof(stale_pin_warned[0].board_name));
        stale_pin_warned_count++;
    }

    fprintf(stderr,
            "identify: ~/.boards pin '%s=%s' ignored: that path is now a "
            "%s (%04x:%04x). Re-pin it by serial, or by '# USB: %s'.\n",
            id->board_name, id->dev_path,
            port->known ? port->known->name : "different device",
            port->vid, port->pid,
            port->usb_path[0] ? port->usb_path : "<topology>");
}

void
apply_board_config(tty_port_t *ports, int nports,
                   board_id_t *ids, int nids)
{
    /* Match keys in descending order of trustworthiness, so a precise pin
     * elsewhere in the file always beats a volatile /dev path pin here.
     * PIN_NONE keeps the "no match" case out of the ordering. */
    enum { PIN_NONE = 0, PIN_DEV_PATH, PIN_USB_PATH, PIN_SERIAL };

    for (int i = 0; i < nports; i++) {
        int best = PIN_NONE;
        int best_j = -1;

        for (int j = 0; j < nids; j++) {
            int kind = PIN_NONE;

            if (ids[j].serial[0] && ports[i].serial[0] &&
                strcmp(ports[i].serial, ids[j].serial) == 0) {
                kind = PIN_SERIAL;
            } else if (ids[j].usb_path[0] && !ids[j].serial[0] &&
                       ports[i].usb_path[0] &&
                       strcmp(ports[i].usb_path, ids[j].usb_path) == 0) {
                /* Topology is a match key only for an entry that has no
                 * serial. When a serial is present it is the entry's
                 * identity and the "# USB:" value is just a human note on
                 * where the board was last seen -- often months stale. A
                 * stale note put NUCLEO-H563ZI's name on whichever STLINK
                 * happened to occupy its old hub port. */
                kind = PIN_USB_PATH;
            } else if (ids[j].dev_path[0] &&
                       strcmp(ports[i].dev_path, ids[j].dev_path) == 0) {
                kind = PIN_DEV_PATH;
            }

            if (kind <= best)
                continue;

            /* A bare /dev path is the one key the kernel reuses: minor
             * numbers are recycled, so a pin written months ago can land
             * on an entirely unrelated board (a stale PL2303 pin was
             * sitting on a FlashPro5 this way). Sanity-check it against
             * the VID:PID's board list and say so when it fails, rather
             * than skipping in silence. Serial and topology matches are
             * precise and need no such check. */
            if (kind == PIN_DEV_PATH && ports[i].known) {
                int compat = 0;
                for (int b = 0; b < MAX_BOARDS_PER_DEVICE &&
                                ports[i].known->boards[b]; b++) {
                    if (strcmp(ports[i].known->boards[b],
                              ids[j].board_name) == 0) {
                        compat = 1;
                        break;
                    }
                }
                if (!compat) {
                    warn_stale_pin_once(&ids[j], &ports[i]);
                    continue;
                }
            }

            best = kind;
            best_j = j;
            if (best == PIN_SERIAL)
                break;    /* nothing can outrank a serial match */
        }

        if (best_j >= 0) {
            strlcpy_safe(ports[i].board_override, ids[best_j].board_name,
                         sizeof(ports[i].board_override));
            if (ids[best_j].baud > 0)
                ports[i].baud = ids[best_j].baud;
            /* regenerate label with the board override */
            get_device_label(&ports[i]);
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
        if (first->board_override[0]) {
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

                /* prefer the running daemon's view -- its label may differ
                 * from the freshly-predicted one if the daemon was started
                 * before the latest identify logic, and its log_file is
                 * the file `tail` actually needs. */
                char d_label[256];
                char d_log[512];
                d_label[0] = '\0';
                d_log[0] = '\0';
                int monitored = status_lookup(port->dev_path,
                                              d_label, sizeof(d_label),
                                              d_log, sizeof(d_log));

                if (monitored == 0 && d_label[0] != '\0' &&
                    strcmp(d_label, port->label) != 0) {
                    printf("      Monitored as: %s (daemon restart picks up"
                           " new label)\n", d_label);
                }

                if (monitored == 0 && d_log[0] != '\0') {
                    printf("      Log: %s\n", d_log);
                } else {
                    /* daemon isn't monitoring this port -- fall back to
                     * the predicted-label path if it happens to exist */
                    char logpath[512];
                    snprintf(logpath, sizeof(logpath),
                             "%s/latest/%s.log", log_base_dir_path(), port->label);
                    if (access(logpath, F_OK) == 0)
                        printf("      Log: %s\n", logpath);
                }

                /* show PTY proxy path if it exists */
                char ptypath[512];
                char ptytarget[256];
                snprintf(ptypath, sizeof(ptypath),
                         "%s/pty/%s", log_base_dir_path(), port->label);
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
