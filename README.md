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
- **Cross-platform** -- Linux and macOS from one codebase (see Platform
  Support below)
- **Device identification** -- identifies boards by USB VID:PID via sysfs
  (Linux) or IOKit (macOS): VMK180, ZCU102, PolarFire SoC, STM32, FTDI,
  CP210x, etc.
- **Hardware-named log files** -- logs use board labels (e.g.,
  `POLARFIRE_SOC_UART0.log`) instead of raw tty names
- **Hot-plug detection** -- automatically starts/stops monitoring when USB
  devices are plugged in or removed (netlink KOBJECT_UEVENT on Linux, IOKit
  matching notifications on macOS)
- **Yield/reclaim** -- release a port for flashing, then reclaim it
- **Service integration** -- systemd `Type=notify` user service (Linux) or
  launchd LaunchAgent (macOS), starts at login
- **Session-based logging** -- timestamped log files with automatic pruning
- **poll() event loop** -- single portable event loop for all I/O

## Platform Support

Runs on Linux and macOS. The two OSes share all core logic; only the
device-identity backend, hot-plug backend, and service integration differ.

| Concern            | Linux                          | macOS                              |
| ------------------ | ------------------------------ | ---------------------------------- |
| Serial device glob | `/dev/ttyUSB*`, `ttyACM*`, `ttyUART*` | `/dev/cu.usbserial*`, `cu.usbmodem*` |
| USB identity       | sysfs (`identify_linux.c`)     | IOKit registry (`identify_macos.c`) |
| Hot-plug           | netlink / inotify (`hotplug_linux.c`) | IOKit notifications (`hotplug_macos.c`) |
| Service            | systemd user unit              | launchd LaunchAgent                |

On macOS, open the call-out nodes (`/dev/cu.*`), not the dial-in nodes
(`/dev/tty.*`) which block on carrier detect. `make` auto-selects the
backend via `uname -s`.

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

### Linux (systemd)

```bash
# Install binary to ~/.local/bin/ and systemd service
make install

# Enable auto-start at login
systemctl --user enable --now uart-monitor

# Check status
systemctl --user status uart-monitor

# View daemon logs
journalctl --user -u uart-monitor -f

# Restart
systemctl --user restart uart-monitor
```

### macOS (launchd)

```bash
# Install binary to ~/.local/bin/ and the LaunchAgent plist
make install

# Enable auto-start at login (make install prints these commands)
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.wolfssl.uart-monitor.plist

# Check status (state, pid, and the binary path launchd will exec)
launchctl print gui/$(id -u)/com.wolfssl.uart-monitor

# Daemon logs (launchd has no journal; stdout/stderr go here)
tail -f /tmp/uart-monitor/daemon.log

# Restart -- launchd has no "restart" verb; kickstart -k is the equivalent
# of "systemctl --user restart". Without -k it only starts a stopped job.
launchctl kickstart -k gui/$(id -u)/com.wolfssl.uart-monitor

# Stop / disable
launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.wolfssl.uart-monitor.plist
```

Upgrading on macOS takes two steps: `make install` replaces the binary on disk, but the running agent keeps executing the old image until you restart it. `KeepAlive` restarts the job if it exits, so a plain `kill` also works, but it races the relaunch -- prefer `kickstart -k`.

Use `bootout` + `bootstrap` only when the plist itself changed (launchd caches the job definition, so `kickstart` will not pick up an edited plist).

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
uart-monitor clear STM32N657_UART  # Truncate log for a port (by label)
uart-monitor clear /dev/ttyACM0   # Truncate log for a port (by device)
uart-monitor clear --all           # Truncate all log files
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

### Clearing Logs (CI Workflow)

In CI/automated testing, clear a log before an action so you can reliably
detect new output without confusing it with stale data:

```bash
# Clear the log, then flash, then wait for new output
uart-monitor clear STM32N657_UART
# ... flash firmware or trigger action ...
# Now any output in the log is guaranteed to be new
tail -f /tmp/uart-monitor/latest/STM32N657_UART.log | grep -m1 "Boot complete"
```

Accepts device paths (`/dev/ttyACM0`), tty names (`ttyACM0`), or labels
(`STM32N657_UART`). Use `--all` to clear every monitored port at once.

### Timestamps

Start the monitor with `-t` / `--timestamps` to prepend `[YYYY-MM-DD HH:MM:SS]`
to every log line. This is useful in CI to correlate UART output with build/flash
events:

```bash
uart-monitor monitor -f --proxy --timestamps

# Log lines will look like:
# [2026-03-18 14:30:12] U-Boot SPL 2024.01
# [2026-03-18 14:30:13] DRAM: 2 GiB
```

Without `--timestamps`, raw device output is logged as-is (better for
interactive use and `grep`).

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

--- LOG CLEARED [2026-02-25 14:34:00.000] ---

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
and adds both the real serial fd and the PTY master fd to the poll() loop:

- **Real serial fd readable** -> data is logged AND written to PTY master
- **PTY master readable** -> data from PTY slave is written to real serial fd

This allows tools to interact with the port through the PTY slave path while
the monitor logs all traffic. The `TIOCEXCL` prevents other processes from
bypassing the proxy by opening the real device directly.

### Architecture

Single portable `poll()` event loop multiplexing:
- Serial port reads (one fd per monitored device)
- PTY master reads (proxy mode: one fd per proxied device)
- Hot-plug fd (Linux: netlink/inotify directly; macOS: read end of a
  self-pipe fed by an IOKit notification thread)
- Unix domain socket (control commands)
- Signal self-pipe (SIGTERM/SIGINT/SIGHUP)
- A periodic reconcile timer, driven by the `poll()` timeout deadline

Portable primitives replace the earlier Linux-only stack: `poll()` for
`epoll`, a self-pipe for `signalfd`, the `poll()` timeout for `timerfd`,
and a self-pipe for the identify worker's `eventfd`. A background thread
runs board identification (and, on macOS, the IOKit hot-plug run loop).

### Service Integration

On Linux the `sd_notify` readiness protocol is implemented directly (~20
lines of C sending a datagram to `$NOTIFY_SOCKET`); no `libsystemd` linkage
is needed. On macOS launchd handles readiness via `RunAtLoad`/`KeepAlive`,
so `--systemd` is a no-op there. The binary is fully self-contained.

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

## Troubleshooting

### `tail: inotify cannot be used, reverting to polling: Too many open files`

This warning is **harmless** -- `tail` keeps following the log, just by polling once per second instead of via inotify. It appears when the per-user inotify *instance* limit is exhausted (commonly by VS Code, browsers, file watchers, and leftover `tail` processes), not when you are out of real file descriptors. The built-in `uart-monitor tail` already passes `-F ---disable-inotify`, so it polls directly and never prints this; you only see it from a bare `tail -f`.

Check current usage against the limit:

```bash
# Count inotify instances in use by your user vs. the cap
for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null; done | grep -c anon_inode:inotify
cat /proc/sys/fs/inotify/max_user_instances
```

Raise the limit permanently (one-time, survives reboot):

```bash
echo 'fs.inotify.max_user_instances=512' | sudo tee /etc/sysctl.d/90-inotify.conf
sudo sysctl --system
```

### Stale `tail` processes

Old terminal/agent sessions can leave orphaned followers behind. Clean them up:

```bash
pkill -f 'tail .*-[fF].*/tmp/uart-monitor'
```

### A board shows up under a generic label (e.g. `STM32_VIRTUAL_COM_PORT_UART_<sn>`)

ST-LINK boards get their friendly name (e.g. `NUCLEO-H563ZI`) from `STM32_Programmer_CLI`, which reads the board name from the ST-LINK EEPROM. If that tool is **not installed / not on the daemon's PATH** (`~/.local/bin:/usr/local/bin:/usr/bin:/bin`), identification falls back to `st-info` (chip-family name only) and, if a device hot-plugs while a probe fails, to the generic `<known-name>_UART_<serial-suffix>` label. Installing `STM32_Programmer_CLI` is the root fix.

For a deterministic, probe-independent override, pin the board by USB serial in `~/.boards`. Find the serial with `udevadm info -q property -n /dev/ttyACMx | grep ID_SERIAL_SHORT`, then add a section:

```
# === NUCLEO-H563ZI ===
# USB: 1-1.2.1 | S/N: 000D001E4D4B500C20373831
```

Serial matches are authoritative (they bypass the VID:PID compatibility check), apply on every hot-plug, and survive a daemon restart. Apply with `systemctl --user restart uart-monitor` on Linux, or `launchctl kickstart -k gui/$(id -u)/com.wolfssl.uart-monitor` on macOS. A `SIGHUP` rescan (`systemctl --user reload uart-monitor`) re-reads `~/.boards`, but only applies it to ports that are not already being monitored -- changing the pin on a port the daemon already holds needs a full restart.

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
make test       # Run PTY-based unit tests
make install    # Install binary + systemd service (Linux) / launchd agent (macOS)
make uninstall  # Remove everything
make clean      # Remove build artifacts
```

`make` selects the platform backend automatically via `uname -s`.

Requirements:
- **Linux**: GCC or Clang, glibc. No external libraries.
- **macOS**: Clang (Xcode command-line tools). Links the system IOKit and
  CoreFoundation frameworks; no third-party libraries.
