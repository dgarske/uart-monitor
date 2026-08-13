/* serial.h
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
#ifndef SERIAL_H
#define SERIAL_H

#include <termios.h>

#define PTY_DIR  LOG_BASE_DIR "/pty"

typedef struct {
    int     fd;              /* real serial port fd */
    int     pty_master;      /* PTY master fd (-1 if not proxying) */
    int     pty_slave;       /* PTY slave fd (kept open to prevent EIO) */
    char    pty_path[256];   /* PTY slave path (e.g. /dev/pts/5) */
    char    dev_path[256];
    speed_t baudrate;
} serial_port_t;

/* Open a serial port read-only (O_RDONLY | O_NOCTTY | O_NONBLOCK).
 * Configures termios for the given baud, 8N1, raw mode.
 * Returns 0 on success, -1 on error. */
int serial_open(serial_port_t *sp, const char *dev_path, speed_t baud);

/* Open a serial port in proxy mode (O_RDWR) and create a PTY pair.
 * The PTY slave acts as a virtual serial port that other tools can use.
 * Data from the real port is forwarded to the PTY master (and logged).
 * Data written to the PTY slave is forwarded to the real port.
 * Returns 0 on success, -1 on error. */
int serial_open_proxy(serial_port_t *sp, const char *dev_path, speed_t baud);

/* Close a serial port (and PTY master if proxying).
 * Safe to call on already-closed port. */
void serial_close(serial_port_t *sp);

/* Clear a stale exclusive-access lock (TIOCEXCL) left on the PTY slave
 * by an exiting client (e.g. screen). The daemon holds the pty pair open
 * forever (master + keeper slave), so the kernel never clears the lock
 * on the client's last close and every later open() fails EBUSY.
 * Uses the held keeper pty_slave fd.
 * Returns 1 if an exclusive lock was present and cleared, 0 if there was
 * nothing to clear (or no PTY), -1 on ioctl error. */
int serial_pty_clear_excl(serial_port_t *sp);

/* Map a numeric baud rate (e.g. 115200) to a speed_t constant. */
speed_t baud_to_speed(int baud);

#endif /* SERIAL_H */
