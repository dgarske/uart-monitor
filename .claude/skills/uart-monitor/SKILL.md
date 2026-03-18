---
name: uart-monitor
description: "UART monitoring daemon for embedded development. Use this skill when the user needs to monitor serial ports (ttyUSB, ttyACM, ttyUART), read UART output from embedded boards, flash firmware while monitoring, or interact with serial devices. Covers reading logs, yield/reclaim for flashing, PTY proxy mode, and board identification."
user-invokable: false
---

# uart-monitor -- Background UART Monitor Daemon

A single C binary at `~/GitHub/uart-monitor/` that monitors `/dev/ttyUSB*`,
`/dev/ttyACM*`, and `/dev/ttyUART*` ports and logs output to timestamped files.

## Why This Exists

AI tools (Claude Code) compete with flash tools for serial port access. The Linux
tty layer has a single input buffer per port -- bytes read by one process are
consumed and unavailable to any other reader. This daemon solves the problem by
being the sole reader and logging everything to files that multiple processes can
`tail -f`.

## Quick Reference

### CLI Usage

```
uart-monitor identify              # Scan and identify USB serial ports
uart-monitor identify -v           # Verbose with labels and sysfs details
uart-monitor identify --save       # Save config to ~/.boards

uart-monitor monitor -f            # Start monitoring (foreground, read-only)
uart-monitor monitor -f --proxy    # PTY proxy mode (bidirectional)
uart-monitor monitor -f -t         # Enable [timestamp] prefix on log lines
uart-monitor monitor --systemd     # systemd notify mode (implies -f)
uart-monitor monitor -b 9600       # Custom baud rate (default: 115200)
uart-monitor monitor --only /dev/ttyUSB0,/dev/ttyACM0  # Filter ports

uart-monitor status                # Query running daemon status (JSON)
uart-monitor yield /dev/ttyUSB0    # Release port for flashing
uart-monitor reclaim /dev/ttyUSB0  # Re-acquire port after flashing
uart-monitor clear STM32N657_UART  # Truncate log (by label, tty name, or path)
uart-monitor clear --all           # Truncate all log files
uart-monitor tail POLARFIRE_SOC_UART0  # Tail latest log by label
uart-monitor tail ttyUSB0          # Tail latest log by tty name
```

### Reading UART Output

```bash
# Tail a log by board label or tty name:
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log
tail -f /tmp/uart-monitor/latest/ttyUSB0.log   # symlink to label-named file

# Or use the built-in command:
uart-monitor tail POLARFIRE_SOC_UART0
uart-monitor tail ttyUSB0
```

### Clearing Logs (CI / Automated Testing)

Clear a log before an action to guarantee any subsequent output is new:

```bash
uart-monitor clear STM32N657_UART       # clear by label
uart-monitor clear /dev/ttyACM0         # clear by device path
uart-monitor clear ttyACM0              # clear by tty name
uart-monitor clear --all                # clear all ports

# CI pattern: clear, act, then wait for new output
uart-monitor clear STM32N657_UART
# ... flash firmware or trigger action ...
tail -f /tmp/uart-monitor/latest/STM32N657_UART.log | grep -m1 "Boot complete"
```

### Timestamps for CI

Use `--timestamps` (`-t`) when running in CI to correlate UART output with
build/flash events. Each log line gets a `[YYYY-MM-DD HH:MM:SS]` prefix:

```bash
uart-monitor monitor -f --proxy --timestamps
# Produces: [2026-03-18 14:30:12] U-Boot SPL 2024.01
```

Without `--timestamps`, raw device output is logged as-is (better for
interactive use and grep).

### Flashing Firmware (Yield/Reclaim)

Before flashing, release the port so the flash tool gets exclusive access:

```bash
uart-monitor yield /dev/ttyUSB0
# ... run flash tool ...
uart-monitor reclaim /dev/ttyUSB0
```

### PTY Proxy Mode

With `--proxy`, the daemon creates virtual serial ports. Flash tools and
interactive terminals use the PTY path instead of the real device:

```bash
uart-monitor monitor -f --proxy

# Virtual ports appear at:
#   /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0 -> /dev/pts/N
#   /tmp/uart-monitor/pty/VMK180_UART1 -> /dev/pts/M

# Flash tool uses PTY path:
my-flash-tool --port /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0

# Interactive terminal via PTY:
picocom /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0

# Multiple readers still use log files:
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log
```

## Operating Modes

| Mode | Flag | Port access | TIOCEXCL | Use case |
|------|------|-------------|----------|----------|
| Read-only (default) | none | `O_RDONLY` | No | Passive monitoring, yield for flashing |
| PTY proxy | `--proxy` | `O_RDWR` | Yes | Bidirectional proxy, no yield needed |

## Monitor Options

| Flag | Short | Description |
|------|-------|-------------|
| `--foreground` | `-f` | Run in foreground (don't daemonize) |
| `--proxy` | `-p` | PTY proxy mode (bidirectional, TIOCEXCL) |
| `--timestamps` | `-t` | Prepend `[timestamp]` to each log line (off by default) |
| `--systemd` | | systemd notify mode (implies `-f`) |
| `--baud <rate>` | `-b` | Baud rate (default: 115200) |
| `--only <devs>` | | Only monitor these devices (comma-separated) |

## File Layout

```
/tmp/uart-monitor/
  latest -> session-YYYYMMDD-HHMMSS/      # symlink to current session
  session-YYYYMMDD-HHMMSS/
    POLARFIRE_SOC_UART0.log                # log named by board label
    ttyUSB0.log -> POLARFIRE_SOC_UART0.log # compat symlink
  pty/                                     # (proxy mode only)
    POLARFIRE_SOC_UART0 -> /dev/pts/N      # virtual serial port
  status.json                              # machine-readable status
  uart-monitor.sock                        # control socket
  uart-monitor.pid                         # PID file
```

## Supported Boards

The daemon auto-identifies boards by USB VID:PID via sysfs:
VMK180, ZCU102, PolarFire SoC, NXP LPC54S018M-EVK (LPC-Link2), STM32H563,
STM32N657 (STLINK-V3), FTDI (FT232/FT2232/FT4232), CP210x, CH340, PL2303,
Cypress FX3. Board overrides from `~/.boards`.

## Architecture

Single-threaded `epoll` event loop multiplexing:
- Serial port reads (one fd per monitored device)
- PTY master reads (proxy mode: one fd per proxied device)
- Netlink `KOBJECT_UEVENT` socket (hot-plug detection)
- Unix domain socket (control commands)
- `signalfd` (SIGTERM/SIGINT/SIGHUP)

No threads, no locks, no heap allocation in the read loop. Dynamic epoll timeout
(blocks indefinitely when idle, 200ms when partial line data is buffered).

## Source Code

Built with `make` (gcc, `-Wall -Wextra -Werror -pedantic -std=c11`).
Tests: `make test`. Install: `make install` (systemd user service).

Key source files in `~/GitHub/uart-monitor/src/`:
- `monitor.c` -- epoll event loop, proxy forwarding, yield/reclaim
- `serial.c` -- serial open (read-only or proxy with PTY), termios config
- `identify.c` -- sysfs scanning, VID:PID lookup, board labeling
- `log.c` -- session dirs, line-buffered logging (optional timestamps)
- `control.c` -- Unix socket server/client, status/yield/reclaim/tail commands
- `hotplug.c` -- netlink KOBJECT_UEVENT for USB hot-plug detection
- `devices.h` -- known device table (VID:PID -> chip name, board, port functions)
