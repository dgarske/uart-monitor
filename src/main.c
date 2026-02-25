/* main.c -- uart-monitor entry point and subcommand dispatch */
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
        "  tail <dev>      Tail the latest log for a port\n"
        "\n"
        "Monitor options:\n"
        "  -f, --foreground    Run in foreground (don't daemonize)\n"
        "  --systemd           systemd notify mode (implies -f)\n"
        "  -b, --baud <rate>   Baud rate (default: 115200)\n"
        "  --only <devs>       Only monitor these devices (comma-separated)\n"
        "\n"
        "Identify options:\n"
        "  -v, --verbose       Show full sysfs/udev details\n"
        "  --save              Save config to ~/.boards\n"
        "\n"
        "Log files are written to /tmp/uart-monitor/latest/<tty>.log\n"
        "AI workflow: tail -f /tmp/uart-monitor/latest/ttyUSB0.log\n",
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
