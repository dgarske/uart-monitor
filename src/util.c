/* util.c
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
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int
sysfs_read_attr(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);

    if (n < 0)
        return -1;

    buf[n] = '\0';

    /* strip trailing whitespace / newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }

    return (int)n;
}

int
sysfs_read_hex(const char *path, uint16_t *val)
{
    char buf[16];
    if (sysfs_read_attr(path, buf, sizeof(buf)) < 0)
        return -1;

    unsigned long v = strtoul(buf, NULL, 16);
    *val = (uint16_t)v;
    return 0;
}

void
strlcpy_safe(char *dst, const char *src, size_t sz)
{
    if (sz == 0)
        return;
    size_t n = strlen(src);
    if (n >= sz)
        n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void
timestamp_now(char *buf, size_t bufsz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000);
}

void
timestamp_filename(char *buf, size_t bufsz)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    snprintf(buf, bufsz, "%04d%02d%02d-%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int
mkdirp(const char *path)
{
    char tmp[512];
    strlcpy_safe(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);

    /* strip trailing slash */
    if (len > 0 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;

    return 0;
}

int
symlink_update(const char *target, const char *linkpath)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", linkpath, getpid());

    unlink(tmp);
    if (symlink(target, tmp) < 0)
        return -1;
    if (rename(tmp, linkpath) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
