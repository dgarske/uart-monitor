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
#include "log.h"

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

int
set_nonblock_cloexec(int fd)
{
    int fl;

    if (fd < 0)
        return -1;

    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;

    fl = fcntl(fd, F_GETFD);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
        return -1;

    return 0;
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

/* Extract the quoted string value of `key` inside [from, stop).
 * On success writes into out and returns a pointer past the closing
 * quote. Returns NULL if key not present within the bounds. */
static const char *
json_get_str(const char *from, const char *stop, const char *key,
             char *out, size_t outsz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);

    const char *p = strstr(from, needle);
    if (p == NULL || p >= stop)
        return NULL;

    p = strchr(p + strlen(needle), '"');
    if (p == NULL || p >= stop)
        return NULL;

    const char *end = strchr(p + 1, '"');
    if (end == NULL || end >= stop)
        return NULL;

    size_t n = (size_t)(end - p - 1);
    if (outsz > 0) {
        if (n >= outsz)
            n = outsz - 1;
        memcpy(out, p + 1, n);
        out[n] = '\0';
    }
    return end + 1;
}

int
status_lookup(const char *device_or_label,
              char *label_out, size_t label_sz,
              char *log_out, size_t log_sz)
{
    if (device_or_label == NULL)
        return -1;

    FILE *fp = fopen(LOG_BASE_DIR "/status.json", "r");
    if (fp == NULL)
        return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > (long)(1024 * 1024)) {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[nr] = '\0';

    /* normalise input to a /dev/ path for the device-match comparison */
    char dev_path[256];
    if (strncmp(device_or_label, "/dev/", 5) == 0)
        strlcpy_safe(dev_path, device_or_label, sizeof(dev_path));
    else
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", device_or_label);

    int found = 0;
    const char *p = buf;
    while ((p = strstr(p, "\"device\":")) != NULL) {
        /* a port block ends with "    }" at column 4 in write_status_json */
        const char *block_end = strstr(p, "    }");
        if (block_end == NULL)
            block_end = buf + nr;

        char device[256];
        char label[256];
        char logfile[512];
        device[0] = '\0';
        label[0] = '\0';
        logfile[0] = '\0';

        json_get_str(p, block_end, "device", device, sizeof(device));
        json_get_str(p, block_end, "label", label, sizeof(label));
        json_get_str(p, block_end, "log_file", logfile, sizeof(logfile));

        if (strcmp(device, dev_path) == 0 ||
            (label[0] != '\0' && strcmp(label, device_or_label) == 0)) {
            if (label_out != NULL)
                strlcpy_safe(label_out, label, label_sz);
            if (log_out != NULL)
                strlcpy_safe(log_out, logfile, log_sz);
            found = 1;
            break;
        }

        p = block_end;
    }

    free(buf);
    return found ? 0 : -1;
}
