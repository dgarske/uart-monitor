/* log.h
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
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stddef.h>
#include <time.h>

#define LOG_BASE_DIR      "/tmp/uart-monitor"
#define LOG_LINE_BUF_SIZE 2048
#define LOG_MAX_SESSIONS  10

typedef struct {
    FILE  *fp;
    char   filepath[512];
    size_t bytes_written;
    time_t session_start;
    /* line buffer for timestamp insertion */
    char   linebuf[LOG_LINE_BUF_SIZE];
    int    linebuf_len;
    int    last_was_cr;       /* track \r across read() boundaries */
    int    timestamps;        /* prepend [timestamp] to each line */
    struct timespec last_flush;
} log_file_t;

/* Create a new session directory under LOG_BASE_DIR and update the
 * "latest" symlink. Writes session name into session_path.
 * Returns 0 on success. */
int log_create_session(char *session_path, size_t sz);

/* Open a per-port log file inside the session directory.
 * header is written at top (device info, baud, etc). */
int log_open(log_file_t *lf, const char *session_path,
             const char *tty_name, const char *header);

/* Write raw serial data to the log, inserting timestamps on each line.
 * Buffers partial lines until '\n' or flush timeout. */
int log_write(log_file_t *lf, const char *data, size_t len);

/* Flush any buffered partial line (called on timeout or close). */
void log_flush(log_file_t *lf);

/* Write a marker line (e.g. yield/reclaim/disconnect). */
void log_marker(log_file_t *lf, const char *msg);

/* Truncate/clear a log file. Resets contents and byte counter.
 * Writes a "LOG CLEARED" marker after truncation. */
void log_clear(log_file_t *lf);

/* Close a log file. */
void log_close(log_file_t *lf);

/* Remove old session directories, keeping the most recent 'keep'. */
int log_prune_sessions(int keep);

#endif /* LOG_H */
