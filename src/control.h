/* control.h
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
#ifndef CONTROL_H
#define CONTROL_H

#define CONTROL_SOCK_PATH LOG_BASE_DIR "/uart-monitor.sock"
#define CONTROL_MAX_MSG   4096

/* Initialize the control socket server.
 * Returns listening fd, or -1 on error. */
int control_init(const char *sock_path);

/* Close the control socket and remove the socket file. */
void control_close(int listen_fd, const char *sock_path);

/* Send a command to the daemon's control socket and print the response.
 * Used by CLI client subcommands. Returns 0 on success. */
int control_send_cmd(const char *sock_path, const char *cmd);

/* CLI subcommands that talk to the running daemon */
int cmd_status(int argc, char *argv[]);
int cmd_yield(int argc, char *argv[]);
int cmd_reclaim(int argc, char *argv[]);
int cmd_clear(int argc, char *argv[]);
int cmd_tail(int argc, char *argv[]);

#endif /* CONTROL_H */
