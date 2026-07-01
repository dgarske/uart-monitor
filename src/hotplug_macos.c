/* hotplug_macos.c
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

/* macOS hot-plug backend.
 *
 * IOKit delivers device add/remove notifications through a CoreFoundation
 * run loop, which does not integrate with the daemon's poll() loop. So we
 * run a dedicated thread that owns a CFRunLoop with IOKit matching
 * notifications for IOSerialBSDClient. Each callback extracts the call-out
 * device path, pushes an event onto a mutex-protected ring buffer, and
 * writes one byte to a self-pipe. hotplug_init() returns the read end of
 * that pipe for the main poll() loop; hotplug_read() drains one event per
 * byte. This mirrors the identify worker's thread -> self-pipe wakeup. */

#include "hotplug.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#define HP_QUEUE_MAX 128

static int             g_pipe_r = -1;
static int             g_pipe_w = -1;
static pthread_t       g_thread;
static int             g_started = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ready_cond = PTHREAD_COND_INITIALIZER;
static CFRunLoopRef    g_runloop = NULL;   /* set by the thread when ready */

static hotplug_event_t g_queue[HP_QUEUE_MAX];
static int             g_qhead = 0;   /* pop index */
static int             g_qtail = 0;   /* push index */
static int             g_qcount = 0;

int
hotplug_is_monitored(const char *devname)
{
    /* USB serial call-out nodes: cu.usbserial-*, cu.usbmodem* */
    return strncmp(devname, "cu.usb", 6) == 0;
}

/* Enqueue an event and wake the main loop. Called on the IOKit thread. */
static void
hp_enqueue(hotplug_action_t action, const char *devpath)
{
    const char *base;

    base = strrchr(devpath, '/');
    base = base ? base + 1 : devpath;
    if (!hotplug_is_monitored(base))
        return;

    pthread_mutex_lock(&g_lock);
    if (g_qcount < HP_QUEUE_MAX) {
        hotplug_event_t *ev = &g_queue[g_qtail];
        memset(ev, 0, sizeof(*ev));
        ev->action = action;
        strlcpy_safe(ev->devname, base, sizeof(ev->devname));
        strlcpy_safe(ev->devpath, devpath, sizeof(ev->devpath));
        g_qtail = (g_qtail + 1) % HP_QUEUE_MAX;
        g_qcount++;
    }
    pthread_mutex_unlock(&g_lock);

    /* one byte per event; presence, not value, is the signal */
    if (g_pipe_w >= 0) {
        unsigned char b = 1;
        ssize_t wr = write(g_pipe_w, &b, 1);
        (void)wr;
    }
}

/* Drain an IOKit iterator, enqueuing one event per matched service. The
 * iterator MUST be fully drained or IOKit stops delivering notifications. */
static void
hp_drain_iter(io_iterator_t it, hotplug_action_t action)
{
    io_service_t svc;

    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        CFTypeRef c = IORegistryEntryCreateCFProperty(
            svc, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (c != NULL && CFGetTypeID(c) == CFStringGetTypeID()) {
            char path[256];
            if (CFStringGetCString((CFStringRef)c, path, sizeof(path),
                                   kCFStringEncodingUTF8))
                hp_enqueue(action, path);
        }
        if (c != NULL)
            CFRelease(c);
        IOObjectRelease(svc);
    }
}

static void
hp_add_cb(void *refcon, io_iterator_t it)
{
    (void)refcon;
    hp_drain_iter(it, HOTPLUG_ADD);
}

static void
hp_remove_cb(void *refcon, io_iterator_t it)
{
    (void)refcon;
    hp_drain_iter(it, HOTPLUG_REMOVE);
}

static void *
hp_thread_main(void *arg)
{
    IONotificationPortRef notify;
    CFMutableDictionaryRef match;
    io_iterator_t add_iter = IO_OBJECT_NULL;
    io_iterator_t rm_iter = IO_OBJECT_NULL;

    (void)arg;

    notify = IONotificationPortCreate(kIOMainPortDefault);
    if (notify == NULL)
        return NULL;

    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(notify),
                       kCFRunLoopDefaultMode);

    match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (match == NULL) {
        IONotificationPortDestroy(notify);
        return NULL;
    }
    /* Each IOServiceAddMatchingNotification call consumes one reference;
     * we make two calls, so retain once for the second. */
    match = (CFMutableDictionaryRef)CFRetain(match);

    IOServiceAddMatchingNotification(notify, kIOFirstMatchNotification,
                                     match, hp_add_cb, NULL, &add_iter);
    IOServiceAddMatchingNotification(notify, kIOTerminatedNotification,
                                     match, hp_remove_cb, NULL, &rm_iter);

    /* Arm the notifications by draining the initial iterators. The add
     * pass enqueues every device already present; add_port() dedups by
     * path, so these are harmless (and provide startup self-heal). */
    hp_drain_iter(add_iter, HOTPLUG_ADD);
    hp_drain_iter(rm_iter, HOTPLUG_REMOVE);

    /* publish the run loop so hotplug_close() can stop us */
    pthread_mutex_lock(&g_lock);
    g_runloop = CFRunLoopGetCurrent();
    pthread_cond_broadcast(&g_ready_cond);
    pthread_mutex_unlock(&g_lock);

    CFRunLoopRun();

    /* stopped by hotplug_close() */
    if (add_iter != IO_OBJECT_NULL)
        IOObjectRelease(add_iter);
    if (rm_iter != IO_OBJECT_NULL)
        IOObjectRelease(rm_iter);
    IONotificationPortDestroy(notify);
    return NULL;
}

int
hotplug_init(void)
{
    int fds[2];

    if (pipe(fds) < 0) {
        fprintf(stderr, "hotplug: pipe: %s\n", strerror(errno));
        return -1;
    }
    if (set_nonblock_cloexec(fds[0]) < 0 ||
        set_nonblock_cloexec(fds[1]) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    g_pipe_r = fds[0];
    g_pipe_w = fds[1];

    if (pthread_create(&g_thread, NULL, hp_thread_main, NULL) != 0) {
        fprintf(stderr, "hotplug: pthread_create failed\n");
        close(g_pipe_r);
        close(g_pipe_w);
        g_pipe_r = g_pipe_w = -1;
        return -1;
    }
    g_started = 1;

    return g_pipe_r;
}

int
hotplug_read(int fd, hotplug_event_t *ev)
{
    unsigned char b;
    ssize_t rd;
    int got = 0;

    memset(ev, 0, sizeof(*ev));

    pthread_mutex_lock(&g_lock);
    if (g_qcount > 0) {
        *ev = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % HP_QUEUE_MAX;
        g_qcount--;
        got = 1;
    }
    pthread_mutex_unlock(&g_lock);

    /* consume one wakeup byte to match the one event we popped; if the
     * queue was empty, drain any spurious bytes. */
    if (got)
        rd = read(fd, &b, 1);
    else
        while ((rd = read(fd, &b, 1)) > 0)
            ;
    (void)rd;

    return got;
}

void
hotplug_close(int fd)
{
    if (g_started) {
        /* wait (bounded) for the thread to publish its run loop, then
         * stop it so the thread returns and can be joined. */
        struct timespec deadline;
        int have_rl;

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 2;

        pthread_mutex_lock(&g_lock);
        while (g_runloop == NULL &&
               pthread_cond_timedwait(&g_ready_cond, &g_lock,
                                      &deadline) == 0)
            ;
        have_rl = (g_runloop != NULL);
        pthread_mutex_unlock(&g_lock);

        if (have_rl) {
            CFRunLoopStop(g_runloop);
            pthread_join(g_thread, NULL);
        } else {
            /* run loop never came up -- detach rather than block */
            pthread_detach(g_thread);
        }
        g_started = 0;
    }

    if (fd >= 0)
        close(fd);
    if (g_pipe_w >= 0) {
        close(g_pipe_w);
        g_pipe_w = -1;
    }
    g_pipe_r = -1;
}
