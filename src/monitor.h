/* monitor.h
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
#ifndef MONITOR_H
#define MONITOR_H

#include "identify.h"
#include "identify_worker.h"
#include "serial.h"
#include "log.h"

/* Event source types for epoll dispatch */
typedef enum {
    EVT_SIGNAL,
    EVT_SERIAL,
    EVT_PTY,             /* data from PTY slave (user writing to proxy) */
    EVT_HOTPLUG,
    EVT_CONTROL,
    EVT_CONTROL_CLIENT,
    EVT_RECONCILE,       /* periodic timerfd: re-check device identities */
    EVT_IDENTIFY_DONE,   /* worker eventfd: resolved identities pending */
} event_type_t;

typedef struct {
    event_type_t type;
    int          index;     /* for EVT_SERIAL/EVT_PTY: index into ports[] */
    int          fd;        /* for EVT_CONTROL_CLIENT */
} event_ctx_t;

/* State for a single monitored port */
typedef struct {
    tty_port_t   identity;
    serial_port_t serial;
    log_file_t   log;
    event_ctx_t  evt;         /* epoll context for serial fd */
    event_ctx_t  evt_pty;     /* epoll context for PTY master fd */
    int          yielded;
    size_t       bytes_read;
} monitored_port_t;

/* Overall daemon state */
typedef struct {
    int              epoll_fd;
    int              signal_fd;
    int              hotplug_fd;
    int              control_fd;
    int              reconcile_fd;    /* timerfd for periodic re-identify */
    int              identify_fd;     /* worker eventfd (results pending) */
    identify_worker_t *iw;            /* background identification worker */
    char             session_path[512];
    monitored_port_t ports[MAX_PORTS];
    int              port_count;
    event_ctx_t      evt_signal;
    event_ctx_t      evt_hotplug;
    event_ctx_t      evt_control;
    event_ctx_t      evt_reconcile;
    event_ctx_t      evt_identify;
    volatile int     running;
    int              systemd_mode;
    int              proxy_mode;      /* --proxy: PTY proxy for shared access */
    int              timestamps;      /* --timestamps: prepend [ts] to log lines */
    speed_t          baudrate;
    char             only_filter[512];  /* comma-separated device filter */
} monitor_state_t;

/* The monitor subcommand entry point. */
int cmd_monitor(int argc, char *argv[]);

#endif /* MONITOR_H */
