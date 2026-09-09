---
name: uart-monitor
description: "UART monitor daemon for embedded boards (ttyUSB/ACM). Use to read serial logs, flash with monitor, PTY proxy."
user-invokable: false
---

# uart-monitor -- Background UART Monitor Daemon

Single C binary at `~/GitHub/uart-monitor/` that monitors `/dev/ttyUSB*`,
`/dev/ttyACM*`, `/dev/ttyUART*` and logs to timestamped files. Runs as a
user systemd service (`systemctl --user status uart-monitor`) and is the
**sole reader** of each real serial device -- everything else reads via the
log file or writes via the per-port PTY.

## NOTE: For AI / agents -- canonical command names

There is **no `uart-monitor list` subcommand.** The discovery command is
`uart-monitor status`, which prints a JSON document with a top-level
`"ports"` array. Parse it; don't grep it. To find a specific board by label:

```bash
# Get device + log + PTY for a known board label (e.g. "CW_VPX3_152")
uart-monitor status | jq -r '.ports[] | select(.label=="CW_VPX3_152")
    | "\(.device) log=\(.log_file) pty=\(.pty_device)"'

# List all attached labels
uart-monitor status | jq -r '.ports[].label'
```

**Never hardcode `ttyUSBnn`** -- the kernel renumbers across reboots and
hot-plug events. Look it up in `uart-monitor status` every time. The label
(e.g. `CW_VPX3_152`, `POLARFIRE_SOC_UART0`) is stable; the tty name is not.

## Read this first (decision tree)

Before touching anything, check the running mode:

```bash
uart-monitor status | head -5      # look for "proxy_mode": true|false
```

Then pick the right path for what you need to do:

| You want to... | Proxy mode (`proxy_mode: true`) | Read-only mode (`proxy_mode: false`) |
|----------------|---------------------------------|--------------------------------------|
| **Read UART output** | `tail -f /tmp/uart-monitor/latest/<LABEL>.log` or `uart-monitor tail <LABEL>` | Same -- log file is authoritative |
| **Write / interactive terminal** | Use the PTY path: `/tmp/uart-monitor/pty/<LABEL>` | `uart-monitor yield /dev/ttyXXX` -> use -> `uart-monitor reclaim /dev/ttyXXX` |
| **Flash** | PTY path if the tool accepts it; otherwise `yield` -> flash -> `reclaim` | `yield` -> flash -> `reclaim` |
| **Check a board is connected** | `uart-monitor status` (JSON) | same |

The daemon on this system is installed as a systemd user service with
`--proxy`, so **proxy mode is the default**. Assume proxy mode unless
`uart-monitor status` says otherwise.

## DO NOT

These actions break the daemon or other concurrent sessions. Do not take them
unless the user explicitly asks:

- **Do not** `systemctl --user stop|restart uart-monitor`. Other sessions
  (including the user's own terminals) depend on it. Stop = everyone loses
  their serial output. **`restart` is just as destructive as `stop`** -- it
  bounces the daemon for every board on the bench, not only yours. If a port
  looks stuck / disconnected, **do not reach for `restart`** -- use
  `systemctl --user reload` (see below), which rescans without dropping
  anyone.
- **Do not** `kill` / `pkill` `uart-monitor`.
- **Do not** run a bare `uart-monitor monitor` by hand -- it fights the
  systemd instance for `/tmp/uart-monitor/uart-monitor.sock`, the PID file
  and `status.json`. If you need your own instance (testing a code change,
  say), set `UART_MONITOR_DIR` to relocate ALL of its runtime state and
  restrict it with `--only`, which makes it fully isolated:

  ```bash
  export UART_MONITOR_DIR=/tmp/uart-monitor-test
  mkdir -p "$UART_MONITOR_DIR"
  uart-monitor monitor -f --proxy --only /dev/ttyUSB99
  ```

  The shared daemon still holds every real port (`TIOCEXCL` in proxy mode),
  so borrow one with `yield` first and `reclaim` it when you are done.
- **Do not** open `/dev/ttyUSB*` / `/dev/ttyACM*` directly in proxy mode.
  The daemon holds `TIOCEXCL`; your open will `EBUSY` or disrupt state.
  Use the PTY path instead.
- **Do not** stop the daemon or switch it to read-only mode just to flash.
  `yield`/`reclaim` work fine in proxy mode -- they close only that one
  port's fd, leaving every other board untouched. Prefer the PTY path when
  the flash tool can take an arbitrary device path; use `yield`/`reclaim`
  when the tool must drive the RAW device (e.g. `uartfwburn`, which changes
  the line baud itself). If a flash script calls `systemctl stop/start`,
  fix the script to use `yield`/`reclaim` rather than running it as-is.
- **Do not** assume a port is missing because `tail` shows nothing -- check
  `uart-monitor status` for `"status": "monitoring"` vs `"yielded"`.

If the daemon is genuinely broken and needs restarting, ask the user first.

## Reading UART output

The log file is always the right answer, regardless of mode:

```bash
tail -f /tmp/uart-monitor/latest/POLARFIRE_SOC_UART0.log

# Built-in helper (resolves label OR tty name OR /dev path via status.json):
uart-monitor tail POLARFIRE_SOC_UART0
uart-monitor tail ttyACM0
uart-monitor tail /dev/ttyACM0
```

There are **no `ttyUSB0.log -> LABEL.log` symlinks** any more. A tty number is
not a stable identity -- the kernel recycles minor numbers, and the alias was
never cleaned up on disconnect, so it silently came to point at a different
board's log. Use `uart-monitor tail <tty>`, which resolves the live `/dev` path
through `status.json` instead of trusting a stale symlink.

Never read from `/dev/ttyXXX` directly. The kernel tty layer gives each byte
to exactly one reader; the daemon already owns that.

**Logs are append-only.** The daemon never truncates a log on its own: it opens
every log with `O_APPEND` and only ever truncates in response to an explicit
`uart-monitor clear`. A disconnect writes a `--- PORT DISCONNECTED ---` marker
and the same file is reopened and appended to when the board returns, so a
marker you write before flashing, or a byte offset you record, DOES survive a
reset. Anchor a build->flash->read loop on your own marker or on
`--- PORT DISCONNECTED ---`.

(Earlier versions of this skill claimed the daemon auto-truncated on reset.
That was a misreading of a real bug: several boards used to collapse onto one
shared label, hence one shared log file, so `uart-monitor clear` on one board
wiped another board's capture. Labels are now unique per port, so this cannot
happen -- see "Port labels" below.)

### Clearing logs before an action (CI pattern)

```bash
uart-monitor clear STM32N657_UART         # by label
uart-monitor clear /dev/ttyACM0           # by device path
uart-monitor clear ttyACM0                # by tty name
uart-monitor clear --all

# Pattern: clear, act, wait for new output
uart-monitor clear STM32N657_UART
# ... flash firmware or trigger action ...
tail -f /tmp/uart-monitor/latest/STM32N657_UART.log | grep -m1 "Boot complete"
```

## Flashing firmware

### Proxy mode (default on this system)

The daemon already owns the device. Point the flash tool at the PTY:

```bash
# PTY path is per-port, named by label:
my-flash-tool --port /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0

# Interactive terminal:
picocom /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0
```

Everything written to the PTY reaches the real device; everything the device
sends reaches both the log file and any PTY readers. **No yield/reclaim
required.**

If the tool is hard-coded to want a `/dev/ttyXXX` path, symlink the PTY:

```bash
ln -sf /tmp/uart-monitor/pty/POLARFIRE_SOC_UART0 /tmp/my-tty
my-flash-tool --port /tmp/my-tty
```

### Writing / sending input to the device (scripted)

Proxy-mode PTY writes are reliable (the earlier PTY write bug is fixed) -- you do
NOT need yield/reclaim to send bytes. Write straight to the per-port PTY; each
write reaches the real device and the device's reply lands in the log as usual.

```bash
PTY=/tmp/uart-monitor/pty/<LABEL>

# Send a line to a shell / login prompt. Serial consoles take CR for Enter;
# send \r (add \n only if the target needs LF). Keep the log tail open in
# another shell to watch the response.
printf 'root\r'      > "$PTY"      # e.g. answer a "login:" prompt
sleep 1
printf '\r'          > "$PTY"      # empty password -> just Enter
sleep 1
printf 'uname -a\r'  > "$PTY"      # run a command, then read the log

# One-shot send + capture the reply that follows (anchor on the log):
printf 'cat /etc/os-release\r' > "$PTY"
sleep 1; tail -n 20 /tmp/uart-monitor/latest/<LABEL>.log
```

Notes:
- Redirect (`> "$PTY"`) opens and closes the PTY per write; that's fine for
  one-shot lines. Send a small `sleep` between lines so the target keeps up.
- Do NOT `stty`/`picocom` the underlying `/dev/ttyXXX` -- write to the PTY path.
- If a keystroke seems ignored, try `\r\n` instead of `\r` (or vice-versa); some
  getty/login prompts differ.

### Flash tools that need the RAW device (either mode)

Some tools must drive `/dev/ttyXXX` itself -- typically because they change
the line baud mid-flash (`uartfwburn`) -- and cannot be pointed at a PTY.
Release just that one port first. This works in **proxy mode too**: it closes
only that port's fd and leaves every other board monitored. It does NOT
require stopping the daemon or switching it to read-only mode.

**Full device path required**, not a label:

```bash
uart-monitor yield /dev/ttyUSB0
# ... run flash tool on /dev/ttyUSB0 ...
uart-monitor reclaim /dev/ttyUSB0
```

If you forget to `reclaim`, the port stays `"status": "yielded"` and no log
output is captured until reclaimed. A yielded port is also skipped by the
reconcile and relabel paths, so it will not be disturbed while your tool has
it. Always `reclaim` in a trap/cleanup so a failed flash does not leave the
board dark.

## CLI quick reference

```
uart-monitor identify              # Scan and identify USB serial ports
uart-monitor identify -v           # Verbose (labels, sysfs details)
uart-monitor identify --save       # Save config to ~/.boards

uart-monitor status                # Query running daemon (JSON)
uart-monitor tail POLARFIRE_SOC_UART0   # Tail latest log by label or tty name
uart-monitor clear STM32N657_UART       # Truncate log (label, tty, path, or --all)
uart-monitor baud /dev/ttyUSB0 9600     # Change baud for a port at runtime

uart-monitor yield /dev/ttyUSB0    # Release port (read-only mode ONLY)
uart-monitor reclaim /dev/ttyUSB0  # Re-acquire port (read-only mode ONLY)

# Env: relocate ALL runtime state (logs, status.json, PID, control socket)
#   UART_MONITOR_DIR=/tmp/uart-monitor-test uart-monitor status

# These are the daemon, not the client -- don't run them manually:
uart-monitor monitor -f            # Foreground read-only mode
uart-monitor monitor -f --proxy    # Foreground proxy mode
uart-monitor monitor --systemd     # systemd notify mode (what the service uses)
```

## Troubleshooting: daemon seems down / output stopped

Check the state before doing anything:

```bash
systemctl --user status uart-monitor             # Active: running ?
journalctl --user -u uart-monitor -n 50          # last 50 log lines
journalctl --user -u uart-monitor --since "1h ago" | grep -E "Stopping|SIGTERM|Started|Failed"
```

Interpreting what you see:

- **"Received SIGTERM, shutting down..."** preceded by `systemd[...]: Stopping
  uart-monitor.service` -> something ran `systemctl stop|restart`. Not a
  crash. Find who. Do **not** assume you should restart it -- ask the user.
- **"Main process exited, code=killed, signal=..."** or `code=dumped` ->
  real crash. Report to user with the journal excerpt.
- **"Hot-plug: ... removed"** -> USB device unplugged. Log file stops growing
  until it reappears.
- **`"status": "yielded"`** in `uart-monitor status` -> someone yielded and
  didn't reclaim. Run `uart-monitor reclaim /dev/ttyXXX` only if you are
  sure no flash tool is still using the port.
- **`"flapping": true`, or a burst of connect/disconnect lines** -> the board
  is re-enumerating repeatedly. Almost always physical: the board has no power
  applied, a failing cable, or an overloaded hub. The daemon damps this
  automatically (it quiets per-event logging and defers identification until
  the hardware settles) and reports one summary line plus `disconnect_count`,
  so check `uart-monitor status` for the count rather than counting journal
  lines. This is NOT something a daemon restart fixes.
- **A board that was just re-plugged shows nothing / `tail` says
  disconnected, and it's absent from `uart-monitor status`** -> the daemon
  likely lost the re-add (historically a udev-ACL race: the open hit
  `EACCES` before udev applied permissions to the fresh node). The daemon
  now self-heals on its periodic reconcile (~30s) and retries ~1s after a
  hot-plug add. To rescan **immediately without disrupting any other board**,
  run `systemctl --user reload uart-monitor` (a SIGHUP rescan -- it re-adds
  new/missing ports and never drops existing ones). **Never** use
  `restart`/`stop` for this.

## File layout

```
/tmp/uart-monitor/
  latest -> session-YYYYMMDD-HHMMSS/     # symlink to current session
  session-YYYYMMDD-HHMMSS/
    POLARFIRE_SOC_UART0.log              # log named by board label
    GENERIC_UART_AL00KKC6.log            # unidentified: keyed by USB serial
    FTDI_FT4232H_UART0_1_6_1.log         # no serial: keyed by USB topology
  pty/                                   # (proxy mode only)
    POLARFIRE_SOC_UART0 -> /dev/pts/N    # per-port PTY
  status.json                            # machine-readable status
  uart-monitor.sock                      # control socket
  uart-monitor.pid                       # PID file
```

All of the above moves under `$UART_MONITOR_DIR` when that is set.

## Port labels

The label names the log file and the PTY symlink, so it is built from the most
stable identifier the port has -- never from the tty number, which changes on
every re-enumeration:

| Available | Label form | Example |
|-----------|-----------|---------|
| `~/.boards` pin | `<BOARD>_UART[<iface>]` | `POLARFIRE_SOC_UART0` |
| Probe / product match | `<BOARD>_UART[<iface>]` | `NUCLEO_H563ZI_UART` |
| USB serial | `<CHIP>_UART[<iface>]_<serial>` | `GENERIC_UART_AL00KKC6` |
| USB topology only | `<CHIP>_UART[<iface>]_<path>` | `FTDI_FT4232H_UART0_1_6_1` |

A multi-port bridge always carries its interface number, so the four interfaces
of a quad FTDI stay four distinct labels, logs and PTYs.

A label is stable across replug, so it is safe to hardcode in a test script --
unlike `ttyUSBnn`. If a label ends in a tty name (`_TTYUSB4`), the device gave
us neither a serial nor a topology path; that is the last-resort form and it
will move.

## Pinning a board in ~/.boards

Give a board a friendly, deterministic name. Three keys, strongest first:

```
# === NUCLEO-H563ZI ===
# USB: 1-1.2.1 | S/N: 000D001E4D4B500C20373831   <- serial: best
```

```
# === IMX8QM_MEK ===
# Baud: 115200
# USB: 1-6.1                                     <- topology: for adapters
```

```
# === Some Board ===
SOME_BOARD=/dev/ttyUSB8                          <- /dev path: weakest
```

- A **serial** pin is authoritative and survives anything.
- A **topology** pin (`# USB:` with no `S/N:`) is for adapters strapped with
  `SerialNumber=0`, such as the FT4232H on many dev boards -- they have no
  serial and their `/dev` path floats, so the hub port is the only stable key.
  Get the path from the `USB Path` field of `uart-monitor identify`.
- When an entry has **both** `# USB:` and `S/N:`, only the serial is a match
  key; the topology value is then just a note of where the board was last seen,
  and those notes go stale as boards move.
- A **`/dev` path** pin is checked against the device actually on that path. If
  the VID:PID does not fit the pinned board the pin is ignored and the daemon
  says so once, naming the topology to re-pin by.

Each `# === Board ===` section contributes at most one pin, and a stronger key
wins within a section.

Apply a change with `systemctl --user reload uart-monitor` (SIGHUP). This
re-applies pins to ports the daemon is **already** monitoring as well as new
ones -- a relabelled port has only its log file and PTY symlink reopened, never
its USB fd, so no other board is disturbed. A restart is not needed.

## status.json fields

Per entry in `.ports[]`:

| Field | Meaning |
|-------|---------|
| `device` | live `/dev` path (changes across replug) |
| `label` | stable name; drives `log_file` and `pty_device` |
| `board` | resolved board, or `Unknown` when the VID:PID is shared and unpinned |
| `function` | e.g. `UART0`, `Main UART` |
| `vid` / `pid` | USB IDs as 4-hex-digit strings |
| `status` | `monitoring` or `yielded` |
| `log_file` | absolute path to the log |
| `pty_device` / `pty_slave` | proxy mode only |
| `bytes_logged` | bytes captured this session |
| `flapping` | true if the port is re-enumerating repeatedly right now |
| `disconnect_count` | disconnects since the daemon started |
| `last_disconnect` | Unix timestamp, 0 if never |

`.identified_ports[]` lists eligible ports the daemon is NOT monitoring
(filtered out by `--only`, or not currently openable). A board missing from
`.ports[]` but present here is enumerated but unmonitored.

`board: "Unknown"` is deliberate, not a failure: when a VID:PID is shared by
several boards and nothing has resolved which one this is, the daemon says so
rather than naming the first candidate. Pin it to give it a real name.

## Monitor options (for reference; daemon is already running)

| Flag | Short | Description |
|------|-------|-------------|
| `--foreground` | `-f` | Run in foreground (don't daemonize) |
| `--proxy` | `-p` | PTY proxy mode (bidirectional, TIOCEXCL) |
| `--timestamps` | `-t` | Prepend `[YYYY-MM-DD HH:MM:SS]` to each log line |
| `--systemd` | | systemd notify mode (implies `-f`) |
| `--baud <rate>` | `-b` | Baud rate (default: 115200) |
| `--only <devs>` | | Only monitor these devices (comma-separated) |

## Supported boards

Auto-identified by USB VID:PID via sysfs:
VMK180, ZCU102, PolarFire SoC, NXP LPC54S018M-EVK (LPC-Link2), AMD Spartan,
STM32 NUCLEO boards via STLINK-V3 (H5/H7, U3/U5, N6, C5, F4/F7, G0/G4,
L4/L5, WB/WL -- resolved by active probe, see below), FTDI
(FT232/FT2232/FT4232), CP210x, CH340, PL2303 / PL2303GC, Cypress FX3,
Moxa UPort 1150, Microsemi/Microchip FlashPro5, TI XDS110, SEGGER J-Link,
Nuvoton Nu-Link2, Lauterbach TRACE32. Board overrides from `~/.boards`.

A chip whose VID:PID maps to several possible boards (the FTDI FT2232H /
FT4232H, for instance) reports `board: "Unknown"` until a probe or a
`~/.boards` pin resolves it.

### STLINK-V3 disambiguation

The three STM32 ST-LINK PIDs (`0x0483:0x374b`, `0x374e`, `0x3754`) are all
shared across many STM32 families and NUCLEO boards, so `uart-monitor
identify` actively probes any port with one of those PIDs:

1. **`STM32_Programmer_CLI --list`** is tried first. It reads the full
   NUCLEO board name straight from the ST-LINK firmware (per S/N), works
   even when the target is locked / asleep, and yields labels like
   `NUCLEO_H563ZI_UART`. The CLI is auto-located via `command -v` and
   common install paths (`/opt/st/.../STM32CubeProgrammer/bin/`,
   `~/STM32CubeProgrammer/bin/`).
2. **`st-info --probe`** (from `stlink-tools`, `sudo apt install stlink-tools`)
   is the fallback. It reads DBGMCU IDCODE and maps to family only -- the
   table covers F4/F7, G0/G4, H5/H7, L4/L5, U3/U5, N6, WB/WL (full list
   in `src/devices.h:STM32_CHIPID_MAP`). Examples: `0x454` -> STM32U3,
   `0x484` -> STM32H563, `0x505` -> STM32N657.
3. **Last resort:** if both probes fail (target wedged, neither tool
   installed), the label gets a `_<last-8-of-SN>` suffix so multiple
   unresolved STLINK-V3s on the same bench don't collide.

Force a specific name via `~/.boards` keyed by serial number.

## Architecture (for when something breaks)

One `poll()` event loop on the main thread, multiplexing:
- Serial port reads (one fd per monitored device)
- PTY master reads (proxy mode: one fd per proxied device)
- Netlink `KOBJECT_UEVENT` socket (hot-plug detection; inotify on `/dev`
  as fallback, IOKit on macOS)
- Unix domain socket (control commands)
- Signal self-pipe (SIGTERM/SIGINT/SIGHUP)
- The identify worker's result pipe

Plus **one background thread**: the identify worker (`identify_worker.c`).
Board identification can shell out to `STM32_Programmer_CLI` / `st-info`,
which blocks for seconds, so it never runs on the event loop. The worker
probes off-thread and hands results back through a self-pipe; the main
thread applies them. No locks in the read path and no heap allocation per
read. A periodic reconcile (~30s) re-checks that the hardware behind each
tty still matches the label it was opened under, catching board swaps whose
hot-plug events were missed.

Source files in `~/GitHub/uart-monitor/src/`:
- `monitor.c` -- event loop, proxy forwarding, yield/reclaim, flap tracking
- `serial.c` -- serial open (read-only or proxy with PTY), termios config
- `identify.c` -- sysfs scanning, VID:PID lookup, board labeling, ~/.boards
- `identify_linux.c` / `identify_macos.c` -- per-platform sysfs / IOKit reads
- `identify_worker.c` -- background probe thread
- `log.c` -- session dirs, line-buffered logging (optional timestamps)
- `control.c` -- Unix socket, status/yield/reclaim/tail/clear/baud
- `hotplug_linux.c` / `hotplug_macos.c` -- hot-plug detection per platform
- `util.c` -- string helpers, status.json lookup
- `devices.h` -- VID:PID -> chip / board / port-function table

Built with `make` (gcc, `-Wall -Wextra -Werror -pedantic -std=c11`).
Tests: `make test`. Install: `make install` (systemd user service).
