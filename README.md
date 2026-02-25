# uart-monitor

Background UART monitor daemon for embedded development. Designed to solve the
problem of AI tools (Claude Code) competing with flash tools for serial port
access.

Instead of the AI opening serial ports directly, `uart-monitor` runs in the
background, reads all UART output, and logs it to timestamped files. The AI
simply `tail -f` the log files.

## Features

- **Single C binary** -- no external dependencies beyond libc
- **Read-only serial access** -- opens ports `O_RDONLY`, never writes to UARTs
- **Device identification** -- reads sysfs to identify boards by USB VID:PID
  (VMK180, ZCU102, PolarFire SoC, STM32, FTDI, CP210x, etc.)
- **Hot-plug detection** -- automatically starts/stops monitoring when USB
  devices are plugged in or removed (via netlink KOBJECT_UEVENT)
- **Yield/reclaim** -- release a port for flashing, then reclaim it
- **systemd integration** -- `Type=notify` user service, starts at login
- **Session-based logging** -- timestamped log files with automatic pruning
- **epoll event loop** -- single-threaded, handles all I/O without threads

## Quick Start

```bash
# Build
make

# Identify connected ports
./uart-monitor identify
./uart-monitor identify -v    # verbose with labels

# Start monitoring (foreground)
./uart-monitor monitor -f

# In another terminal, tail the log
tail -f /tmp/uart-monitor/latest/ttyUSB0.log

# Or use the built-in tail command
./uart-monitor tail ttyUSB0
```

## Installation

```bash
# Install binary to ~/.local/bin/ and systemd service
make install

# Enable auto-start at login
systemctl --user enable --now uart-monitor

# Check status
systemctl --user status uart-monitor

# View daemon logs
journalctl --user -u uart-monitor -f
```

## Usage

### Commands

```
uart-monitor identify           # Scan and identify USB serial ports
uart-monitor identify -v        # Verbose output with labels
uart-monitor identify --save    # Save config to ~/.boards

uart-monitor monitor -f         # Start monitoring (foreground)
uart-monitor monitor --systemd  # systemd notify mode (used by service)
uart-monitor monitor -b 9600    # Custom baud rate
uart-monitor monitor --only /dev/ttyUSB0,/dev/ttyACM0  # Filter ports

uart-monitor status             # Query running daemon status (JSON)
uart-monitor yield /dev/ttyUSB0 # Release port for flashing
uart-monitor reclaim /dev/ttyUSB0  # Re-acquire port after flashing
uart-monitor tail ttyUSB0       # Tail the latest log for a port
```

### AI Workflow

The primary use case: let the daemon monitor UARTs while the AI reads log
files.

```bash
# Start the daemon (or use systemd)
uart-monitor monitor -f &

# AI reads UART output by tailing log files:
tail -f /tmp/uart-monitor/latest/ttyUSB0.log

# Before flashing firmware, yield the port:
uart-monitor yield /dev/ttyUSB0

# ... flash firmware ...

# Reclaim the port to resume monitoring:
uart-monitor reclaim /dev/ttyUSB0
```

### Log File Structure

```
/tmp/uart-monitor/
  latest -> session-20260225-143012/     # symlink to current session
  session-20260225-143012/
    ttyUSB0.log                          # PolarFire UART0
    ttyUSB1.log                          # PolarFire UART1
    ttyACM0.log                          # STM32 Virtual COM
  status.json                            # machine-readable status
  uart-monitor.sock                      # control socket
  uart-monitor.pid                       # PID file
```

### Log Format

Each line is prefixed with a millisecond-precision timestamp:

```
=== UART Monitor Session ===
Device: /dev/ttyUSB0 (POLARFIRE_SOC_UART0)
Board: PolarFire SoC | Interface 0 | Function: UART0
Baud: 115200 8N1
Started: 2026-02-25 14:30:12.456
===

[2026-02-25 14:30:12.789] U-Boot SPL 2024.01 (Jan 15 2024 - 10:23:45)
[2026-02-25 14:30:12.801] DRAM:  2 GiB
[2026-02-25 14:30:13.002] Loading kernel...

--- PORT YIELDED (released for flashing) [2026-02-25 14:35:00.123] ---

--- PORT RECLAIMED (monitoring resumed) [2026-02-25 14:40:15.456] ---

[2026-02-25 14:40:16.789] U-Boot SPL 2024.01 (Feb 20 2026 - 09:00:00)
```

### Status JSON

`uart-monitor status` returns machine-readable JSON:

```json
{
  "pid": 12345,
  "session": "session-20260225-143012",
  "port_count": 5,
  "ports": [
    {
      "device": "/dev/ttyUSB0",
      "label": "POLARFIRE_SOC_UART0",
      "board": "PolarFire SoC",
      "function": "UART0",
      "status": "monitoring",
      "log_file": "/tmp/uart-monitor/session-20260225-143012/ttyUSB0.log",
      "bytes_logged": 45678
    }
  ]
}
```

## Supported Boards

| VID:PID | Chip | Boards |
|---------|------|--------|
| 0403:6010 | FTDI FT2232H | VMK180, ZCU102 |
| 0403:6011 | FTDI FT4232H | VMK180, ZCU102 |
| 0403:6014 | FTDI FT232H | Generic |
| 0403:6001 | FTDI FT232R | Generic |
| 04b4:0008 | Cypress FX3 | Versal VMK180, ZCU102 |
| 10c4:ea71 | Silicon Labs CP210x | PolarFire SoC |
| 10c4:ea60 | Silicon Labs CP210x | PolarFire SoC, Generic |
| 0483:374b | STM32 ST-LINK | STM32H563 |
| 0483:374e | STM32 Virtual COM | STM32H563 |
| 0483:5740 | STM32 USB CDC | USB Relay Controller |
| 1a86:7523 | CH340 | USB Relay, Generic |
| 067b:2303 | Prolific PL2303 | Generic |

Board identifications from `~/.boards` (generated by `identify_tty_ports.py
--save`) are automatically applied as overrides.

## Technical Details

### Read-Only Access

The monitor opens serial ports with `O_RDONLY | O_NOCTTY | O_NONBLOCK` and
configures termios for 115200 8N1 raw mode. It **never** calls `write()` on
the serial fd. It does **not** set `TIOCEXCL`, so flash tools can still open
the same port for writing.

### Architecture

Single-threaded `epoll` event loop multiplexing:
- Serial port reads (one fd per monitored device)
- Netlink `KOBJECT_UEVENT` socket (hot-plug detection)
- Unix domain socket (control commands)
- `signalfd` (SIGTERM/SIGINT/SIGHUP)

No threads, no locks, no heap allocation in the read loop.

### systemd Integration

The `sd_notify` protocol is implemented directly (~20 lines of C sending a
datagram to `$NOTIFY_SOCKET`). No `libsystemd` linkage is needed. The binary
is fully self-contained.

## Building

```bash
make            # Build with -Wall -Wextra -Werror -pedantic -std=c11
make test       # Run PTY-based unit tests (17 tests)
make install    # Install binary + systemd service
make uninstall  # Remove everything
make clean      # Remove build artifacts
```

Requirements: Linux, GCC, glibc. No external libraries.
