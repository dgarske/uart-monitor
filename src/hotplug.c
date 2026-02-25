/* hotplug.c -- USB serial device hot-plug detection.
 *
 * Tier 1: Netlink KOBJECT_UEVENT socket (zero deps, immediate).
 * Tier 2: inotify on /dev/ (fallback if netlink fails).
 */
#include "hotplug.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <unistd.h>

/* Which backend we ended up using */
static enum { HP_NETLINK, HP_INOTIFY } hp_mode;

int
hotplug_is_monitored(const char *devname)
{
    return (strncmp(devname, "ttyUSB", 6) == 0 ||
            strncmp(devname, "ttyACM", 6) == 0 ||
            strncmp(devname, "ttyUART", 7) == 0);
}

static int
try_netlink(void)
{
    int fd = socket(AF_NETLINK,
                    SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0)
        return -1;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid    = (unsigned)getpid();
    addr.nl_groups = 1; /* KOBJECT_UEVENT multicast group */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
try_inotify(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;

    if (inotify_add_watch(fd, "/dev", IN_CREATE | IN_DELETE) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int
hotplug_init(void)
{
    int fd = try_netlink();
    if (fd >= 0) {
        hp_mode = HP_NETLINK;
        return fd;
    }

    fprintf(stderr, "hotplug: netlink failed, falling back to inotify\n");
    fd = try_inotify();
    if (fd >= 0) {
        hp_mode = HP_INOTIFY;
        return fd;
    }

    fprintf(stderr, "hotplug: inotify failed: %s\n", strerror(errno));
    return -1;
}

/* Parse a netlink KOBJECT_UEVENT message.
 * The message is a sequence of NUL-terminated strings:
 *   add@/devices/.../ttyUSB0\0
 *   ACTION=add\0
 *   SUBSYSTEM=tty\0
 *   DEVNAME=ttyUSB0\0
 *   ...
 */
static int
parse_netlink(const char *buf, size_t len, hotplug_event_t *ev)
{
    char action[32] = {0};
    char subsystem[32] = {0};
    char devname[64] = {0};

    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        size_t slen = strnlen(p, (size_t)(end - p));
        if (slen == 0) { p++; continue; }

        if (strncmp(p, "ACTION=", 7) == 0)
            strlcpy_safe(action, p + 7, sizeof(action));
        else if (strncmp(p, "SUBSYSTEM=", 10) == 0)
            strlcpy_safe(subsystem, p + 10, sizeof(subsystem));
        else if (strncmp(p, "DEVNAME=", 8) == 0)
            strlcpy_safe(devname, p + 8, sizeof(devname));

        p += slen + 1;
    }

    if (strcmp(subsystem, "tty") != 0)
        return 0; /* not a tty event */

    if (!hotplug_is_monitored(devname))
        return 0;

    if (strcmp(action, "add") == 0)
        ev->action = HOTPLUG_ADD;
    else if (strcmp(action, "remove") == 0)
        ev->action = HOTPLUG_REMOVE;
    else
        return 0;

    strlcpy_safe(ev->devname, devname, sizeof(ev->devname));
    snprintf(ev->devpath, sizeof(ev->devpath), "/dev/%s", devname);
    return 1;
}

/* Parse an inotify event from /dev/. */
static int
parse_inotify(int fd, hotplug_event_t *ev)
{
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
        return -1;

    const char *p = buf;
    const char *end = buf + n;

    while (p < end) {
        const struct inotify_event *ie = (const struct inotify_event *)p;

        if (ie->len > 0 && hotplug_is_monitored(ie->name)) {
            strlcpy_safe(ev->devname, ie->name, sizeof(ev->devname));
            snprintf(ev->devpath, sizeof(ev->devpath), "/dev/%s", ie->name);

            if (ie->mask & IN_CREATE)
                ev->action = HOTPLUG_ADD;
            else if (ie->mask & IN_DELETE)
                ev->action = HOTPLUG_REMOVE;
            else {
                p += sizeof(struct inotify_event) + ie->len;
                continue;
            }
            return 1;
        }

        p += sizeof(struct inotify_event) + ie->len;
    }

    return 0;
}

int
hotplug_read(int fd, hotplug_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));

    if (hp_mode == HP_NETLINK) {
        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        return parse_netlink(buf, (size_t)n, ev);
    }

    return parse_inotify(fd, ev);
}

void
hotplug_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
