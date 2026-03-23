/* hotplug.h
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
#ifndef HOTPLUG_H
#define HOTPLUG_H

typedef enum {
    HOTPLUG_ADD,
    HOTPLUG_REMOVE,
} hotplug_action_t;

typedef struct {
    hotplug_action_t action;
    char devname[64];       /* e.g. "ttyUSB0" */
    char devpath[256];      /* e.g. "/dev/ttyUSB0" */
} hotplug_event_t;

/* Initialize hotplug detection.
 * Tries netlink KOBJECT_UEVENT first, falls back to inotify on /dev/.
 * Returns the fd to add to epoll, or -1 on error. */
int hotplug_init(void);

/* Read and parse a hotplug event from the fd.
 * Returns 1 if a relevant tty event was parsed, 0 if irrelevant, -1 on error. */
int hotplug_read(int fd, hotplug_event_t *ev);

/* Check if a device name matches our monitored patterns. */
int hotplug_is_monitored(const char *devname);

/* Close the hotplug fd. */
void hotplug_close(int fd);

#endif /* HOTPLUG_H */
