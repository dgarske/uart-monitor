/* hotplug.h -- USB serial device hot-plug detection */
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
