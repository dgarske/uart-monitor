/* main.c
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
#include <stdio.h>
#include <string.h>

#include "identify.h"
#include "monitor.h"
#include "control.h"

static void
usage(const char *prog)
{
    fprintf(stderr,
        "uart-monitor -- Background UART monitor for embedded development\n"
        "\n"
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  identify        Scan and identify USB serial ports\n"
        "  monitor         Start monitoring daemon\n"
        "  status          Query running daemon status\n"
        "  yield <dev>     Release a port for flashing\n"
        "  reclaim <dev>   Re-acquire a yielded port\n"
        "  clear <dev>     Truncate log for a port (or --all)\n"
        "  tail <dev>      Tail the latest log for a port\n"
        "\n"
        "Monitor options:\n"
        "  -f, --foreground    Run in foreground (don't daemonize)\n"
        "  -p, --proxy         PTY proxy mode (bidirectional, TIOCEXCL)\n"
        "  -t, --timestamps    Prepend [timestamp] to each log line\n"
        "  --systemd           systemd notify mode (implies -f)\n"
        "  -b, --baud <rate>   Baud rate (default: 115200)\n"
        "  --only <devs>       Only monitor these devices (comma-separated)\n"
        "\n"
        "Identify options:\n"
        "  -v, --verbose       Show full USB device details\n"
        "  --save              Save config to ~/.boards\n"
        "\n"
        "Log files:  /tmp/uart-monitor/latest/<LABEL>.log\n"
        "PTY proxy:  /tmp/uart-monitor/pty/<LABEL>  (with --proxy)\n"
        "\n"
        "AI workflow: tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log\n",
        prog);
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "identify") == 0)
        return cmd_identify(argc - 1, argv + 1);
    if (strcmp(cmd, "monitor") == 0)
        return cmd_monitor(argc - 1, argv + 1);
    if (strcmp(cmd, "status") == 0)
        return cmd_status(argc - 1, argv + 1);
    if (strcmp(cmd, "yield") == 0)
        return cmd_yield(argc - 1, argv + 1);
    if (strcmp(cmd, "reclaim") == 0)
        return cmd_reclaim(argc - 1, argv + 1);
    if (strcmp(cmd, "clear") == 0)
        return cmd_clear(argc - 1, argv + 1);
    if (strcmp(cmd, "baud") == 0)
        return cmd_baud(argc - 1, argv + 1);
    if (strcmp(cmd, "tail") == 0)
        return cmd_tail(argc - 1, argv + 1);
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
