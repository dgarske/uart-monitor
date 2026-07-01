/* identify_worker.h
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
#ifndef IDENTIFY_WORKER_H
#define IDENTIFY_WORKER_H

#include "identify.h"

/* Background device-identification worker.
 *
 * The daemon's main epoll thread must never run the full identify path
 * (it shells out to st-info / STM32_Programmer_CLI, which can block for
 * tens of seconds and freezes all logging). Instead it opens each port
 * immediately under a cheap provisional label and submits the device
 * path here. A dedicated worker thread runs the slow probe and hands the
 * resolved identity back via a result queue; the main thread is woken by
 * a self-pipe and applies the new label (relabel) when it is ready.
 */

typedef struct identify_worker identify_worker_t;

/* A resolved identity handed back to the main thread. Communicated by
 * value -- the worker holds no pointer into the daemon's ports[]. */
typedef struct {
    char       dev_path[256];
    tty_port_t identity;
} identify_result_t;

/* Start the worker thread. On success returns a handle and stores a
 * readable self-pipe fd in *event_fd_out (add it to the poll() set; it
 * becomes readable when one or more results are pending). Returns NULL on
 * failure, in which case *event_fd_out is set to -1. */
identify_worker_t *iw_start(int *event_fd_out);

/* Submit a device path for (re)identification. settle_ms is an optional
 * delay the worker waits before probing (give a freshly hot-plugged
 * device time to finish USB enumeration; pass 0 for startup / reconcile
 * where the device is already settled). Deduplicated against jobs that
 * are already queued or in flight. No-op if iw is NULL. */
void iw_submit(identify_worker_t *iw, const char *dev_path, int settle_ms);

/* Drain up to max ready results into out[] (main thread, on the self-pipe
 * event). Returns the number copied and re-arms the self-pipe if more
 * results remain. Returns 0 if iw is NULL. */
int iw_drain(identify_worker_t *iw, identify_result_t *out, int max);

/* Stop and join the worker, close the self-pipe, free the handle. Safe to
 * call with NULL. */
void iw_stop(identify_worker_t *iw);

#endif /* IDENTIFY_WORKER_H */
