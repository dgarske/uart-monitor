# uart-monitor

Background UART monitor daemon for embedded development. Designed to solve the
problem of AI tools (Claude Code) competing with flash tools for serial port
access.

Instead of the AI opening serial ports directly, `uart-monitor` runs in the
background, reads all UART output, and logs it to timestamped files. The AI
simply `tail -f` the log files.

## Features

- **Single C binary** -- no external dependencies beyond libc
- **Two operating modes**:
  - **Read-only** (default) -- opens ports `O_RDONLY`, never writes to UARTs
  - **PTY proxy** (`--proxy`) -- bidirectional forwarding via pseudo-terminals
- **Device identification** -- reads sysfs to identify boards by USB VID:PID
  (VMK180, ZCU102, PolarFire SoC, STM32, FTDI, CP210x, etc.)
- **Hardware-named log files** -- logs use board labels (e.g.,
  `POLARFIRE_SOC_UART0.log`) instead of raw tty names
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

# Start monitoring (foreground, read-only)
./uart-monitor monitor -f

# Start monitoring with PTY proxy (bidirectional)
./uart-monitor monitor -f --proxy

# In another terminal, tail the log (by label or tty name)
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log
tail -f /tmp/uart-monitor/latest/ttyUSB0.log    # symlink also works

# Or use the built-in tail command
./uart-monitor tail POLARFIRE_SOC_UART0
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

uart-monitor monitor -f         # Start monitoring (foreground, read-only)
uart-monitor monitor -f --proxy # PTY proxy mode (bidirectional)
uart-monitor monitor --systemd  # systemd notify mode (used by service)
uart-monitor monitor -b 9600    # Custom baud rate
uart-monitor monitor --only /dev/ttyUSB0,/dev/ttyACM0  # Filter ports

uart-monitor status             # Query running daemon status (JSON)
uart-monitor yield /dev/ttyUSB0 # Release port for flashing
uart-monitor reclaim /dev/ttyUSB0  # Re-acquire port after flashing
uart-monitor tail POLARFIRE_SOC_UART0  # Tail latest log by label
uart-monitor tail ttyUSB0       # Tail latest log by tty name
```

### AI Workflow (Read-Only Mode)

The default mode: daemon monitors UARTs read-only, AI reads log files.

```bash
# Start the daemon (or use systemd)
uart-monitor monitor -f &

# AI reads UART output by tailing log files:
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log

# Before flashing firmware, yield the port:
uart-monitor yield /dev/ttyUSB0

# ... flash firmware ...

# Reclaim the port to resume monitoring:
uart-monitor reclaim /dev/ttyUSB0
```

### PTY Proxy Mode

With `--proxy`, the monitor opens ports `O_RDWR`, creates a PTY pair for each
port, and sets `TIOCEXCL` on the real device. All access goes through the
virtual PTY device exposed at `/tmp/uart-monitor/pty/<LABEL>`.

```bash
# Start with proxy mode
uart-monitor monitor -f --proxy

# The daemon creates virtual serial ports:
#   /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0 -> /dev/pts/5
#   /tmp/uart-monitor/pty/VMK180_UART1        -> /dev/pts/6

# Interactive terminal access (one user at a time):
picocom /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0

# Multiple readers can still tail the log files:
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log

# Flash tools use the PTY path instead of the real device:
my-flash-tool --port /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0
```

**How it works**:
- Real serial port is opened `O_RDWR` with `TIOCEXCL` (exclusive access)
- A PTY pair is created via `openpty()` for each port
- Data from the real port is written to the log file AND forwarded to the PTY
- Data written to the PTY slave is forwarded to the real serial port
- The PTY slave path is symlinked to `/tmp/uart-monitor/pty/<LABEL>`
- All traffic is logged regardless of direction

**Concurrency**:
- Multiple processes can `tail -f` the log files simultaneously (file I/O)
- Only one process should write to the PTY at a time (serial protocol)
- Yield/reclaim still works: yield closes the real serial fd, reclaim reopens it

### Log File Structure

```
/tmp/uart-monitor/
  latest -> session-20260225-143012/         # symlink to current session
  session-20260225-143012/
    POLARFIRE_SOC_UART0.log                  # log file named by board label
    POLARFIRE_SOC_UART1.log
    STM32H563_UART.log
    ttyUSB0.log -> POLARFIRE_SOC_UART0.log   # compat symlink (tty name)
    ttyUSB1.log -> POLARFIRE_SOC_UART1.log
    ttyACM0.log -> STM32H563_UART.log
  pty/                                       # (proxy mode only)
    POLARFIRE_SOC_UART0 -> /dev/pts/5
    POLARFIRE_SOC_UART1 -> /dev/pts/6
  status.json                                # machine-readable status
  uart-monitor.sock                          # control socket
  uart-monitor.pid                           # PID file
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
  "proxy_mode": true,
  "port_count": 5,
  "ports": [
    {
      "device": "/dev/ttyUSB0",
      "label": "POLARFIRE_SOC_UART0",
      "board": "PolarFire SoC",
      "function": "UART0",
      "status": "monitoring",
      "log_file": "/tmp/uart-monitor/session-20260225-143012/POLARFIRE_SOC_UART0.log",
      "pty_device": "/tmp/uart-monitor/pty/POLARFIRE_SOC_UART0",
      "pty_slave": "/dev/pts/5",
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

### Read-Only Mode (default)

The monitor opens serial ports with `O_RDONLY | O_NOCTTY | O_NONBLOCK` and
configures termios for 115200 8N1 raw mode. It **never** calls `write()` on
the serial fd. It does **not** set `TIOCEXCL`, so flash tools can still open
the same port for writing.

### PTY Proxy Mode (`--proxy`)

The monitor opens serial ports with `O_RDWR | O_NOCTTY | O_NONBLOCK` and sets
`TIOCEXCL` for exclusive access. It creates a `openpty()` pair for each port
and adds both the real serial fd and the PTY master fd to the epoll loop:

- **Real serial fd readable** -> data is logged AND written to PTY master
- **PTY master readable** -> data from PTY slave is written to real serial fd

This allows tools to interact with the port through the PTY slave path while
the monitor logs all traffic. The `TIOCEXCL` prevents other processes from
bypassing the proxy by opening the real device directly.

### Architecture

Single-threaded `epoll` event loop multiplexing:
- Serial port reads (one fd per monitored device)
- PTY master reads (proxy mode: one fd per proxied device)
- Netlink `KOBJECT_UEVENT` socket (hot-plug detection)
- Unix domain socket (control commands)
- `signalfd` (SIGTERM/SIGINT/SIGHUP)

No threads, no locks, no heap allocation in the read loop.

### systemd Integration

The `sd_notify` protocol is implemented directly (~20 lines of C sending a
datagram to `$NOTIFY_SOCKET`). No `libsystemd` linkage is needed. The binary
is fully self-contained.

## Concurrent Access / Port Sharing

### Read-Only Mode

The monitor opens ports `O_RDONLY` and does **not** set `TIOCEXCL`, so other
processes can still `open()` the same device. However, the Linux tty layer has
a single input buffer per port -- **bytes read by one process are consumed and
unavailable to any other reader**. This means:

| Other process does... | Works? | Notes |
|-----------------------|--------|-------|
| Write-only (flash tool sending firmware) | Yes | Monitor keeps reading output |
| Read+write (picocom, minicom, screen) | **No** | Both compete for incoming bytes |
| Read responses (flash ACK/NACK) | **No** | Monitor may steal response bytes |

**Workflow**: use `uart-monitor yield /dev/ttyUSBx` before running any
tool that needs to read from the port, then `uart-monitor reclaim` afterward.

### PTY Proxy Mode

The monitor sets `TIOCEXCL` on the real device and exposes PTY slaves as
virtual serial ports. This eliminates the byte-splitting problem:

| Access pattern | Works? | Notes |
|----------------|--------|-------|
| `tail -f` log files | Yes | Multiple readers, no byte loss |
| Interactive terminal via PTY | Yes | One user at a time |
| Flash tool via PTY path | Yes | Uses PTY instead of real device |
| Direct access to real device | **No** | Blocked by TIOCEXCL |

## Future TODO

- [ ] **Auto-yield via fuser**: Poll `fuser /dev/ttyUSBx` every few seconds to
  detect when another process opens the port. Auto-yield when a foreign PID is
  detected, auto-reclaim when it exits (with a grace period).

- [ ] **Save board config** (`uart-monitor identify --save`): Port the Python
  `save_config()` to C so the identify command can write `~/.boards` natively.

- [ ] **ANSI escape stripping** (`--strip-ansi`): Remove terminal escape
  sequences from log files for cleaner grep/search.

- [ ] **Configurable exclude list**: Skip specific ports (e.g. USB relay
  controllers that use binary protocols) via a config file or `--exclude` flag.

## Building

```bash
make            # Build with -Wall -Wextra -Werror -pedantic -std=c11
make test       # Run PTY-based unit tests (21 tests)
make install    # Install binary + systemd service
make uninstall  # Remove everything
make clean      # Remove build artifacts
```

Requirements: Linux, GCC, glibc. No external libraries.
