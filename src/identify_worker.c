/* identify_worker.c
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
#include "identify_worker.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Seconds iw_stop() waits for the worker to finish its current probe
 * before giving up and letting the exiting process reap the thread. A
 * wedged st-info / STM32_Programmer_CLI must never hang shutdown. */
#define IW_STOP_JOIN_TIMEOUT_SEC 3

/* Bounded queues. MAX_PORTS is 64; double it so a full rescan plus a few
 * hot-plugs can never overflow. Excess submits are dropped (logged by the
 * caller's reconcile/hotplug retry path picking them up next time). */
#define IW_QUEUE_MAX (MAX_PORTS * 2)

/* Extra settle before a retry probe when the first one left the device on
 * its generic fallback label (probe raced USB enumeration / target in
 * reset). Mirrors the old inline hot-plug retry. */
#define IW_RETRY_SETTLE_MS 1500

typedef struct {
    char dev_path[256];
    int  settle_ms;
} iw_job_t;

struct identify_worker {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_cond_t  done_cond;    /* signaled when the thread exits */
    int             pipe_r;       /* self-pipe read end (main loop polls) */
    int             pipe_w;       /* self-pipe write end (worker writes) */
    int             running;      /* cleared by iw_stop to end the thread */
    int             started;      /* thread was successfully created */
    int             finished;     /* thread has returned from its main loop */

    iw_job_t          jobs[IW_QUEUE_MAX];
    int               job_count;

    identify_result_t results[IW_QUEUE_MAX];
    int               result_count;
};

/* Append one resolved identity to the result queue and wake the main
 * thread. Caller must NOT hold the lock. */
static void
iw_push_result(identify_worker_t *iw, const char *dev_path,
               const tty_port_t *identity)
{
    pthread_mutex_lock(&iw->lock);
    if (iw->result_count < IW_QUEUE_MAX) {
        identify_result_t *r = &iw->results[iw->result_count++];
        strlcpy_safe(r->dev_path, dev_path, sizeof(r->dev_path));
        r->identity = *identity;
    }
    pthread_mutex_unlock(&iw->lock);

    /* wake the main loop: one byte per result, presence is the signal */
    unsigned char one = 1;
    ssize_t wr = write(iw->pipe_w, &one, sizeof(one));
    (void)wr;
}

static void *
iw_thread_main(void *arg)
{
    identify_worker_t *iw = (identify_worker_t *)arg;
    iw_job_t batch[IW_QUEUE_MAX];
    int nbatch;
    int i;

    for (;;) {
        pthread_mutex_lock(&iw->lock);
        while (iw->running && iw->job_count == 0)
            pthread_cond_wait(&iw->cond, &iw->lock);
        if (!iw->running) {
            pthread_mutex_unlock(&iw->lock);
            break;
        }
        /* take the whole queue as one batch so a single st-info /
         * STM32_Programmer_CLI --list (cached for a couple seconds)
         * serves every pending device. */
        nbatch = iw->job_count;
        memcpy(batch, iw->jobs, (size_t)nbatch * sizeof(batch[0]));
        iw->job_count = 0;
        pthread_mutex_unlock(&iw->lock);

        /* one fresh probe pass for the batch */
        identify_reset_probe_caches();

        for (i = 0; i < nbatch; i++) {
            tty_port_t port;
            int rc;

            if (batch[i].settle_ms > 0)
                usleep((useconds_t)batch[i].settle_ms * 1000);

            rc = identify_port(batch[i].dev_path, &port);
            if (rc != 0)
                continue; /* device vanished -- main thread will drop it */

            /* If we only got the generic fallback (ambiguous known device
             * with no resolved board), the probe likely raced the USB
             * enumeration. Wait a bit and try once more. */
            if (port.known && known_device_is_ambiguous(port.known) &&
                !port.board_match) {
                usleep((useconds_t)IW_RETRY_SETTLE_MS * 1000);
                identify_reset_probe_caches();
                if (identify_port(batch[i].dev_path, &port) != 0)
                    continue;
            }

            iw_push_result(iw, batch[i].dev_path, &port);
        }
    }

    /* announce exit so iw_stop() can bound its wait portably (no
     * pthread_timedjoin_np, which does not exist on macOS). */
    pthread_mutex_lock(&iw->lock);
    iw->finished = 1;
    pthread_cond_broadcast(&iw->done_cond);
    pthread_mutex_unlock(&iw->lock);

    return NULL;
}

identify_worker_t *
iw_start(int *event_fd_out)
{
    identify_worker_t *iw;

    if (event_fd_out != NULL)
        *event_fd_out = -1;

    iw = calloc(1, sizeof(*iw));
    if (iw == NULL)
        return NULL;

    iw->pipe_r = -1;
    iw->pipe_w = -1;

    {
        int fds[2];
        if (pipe(fds) < 0) {
            free(iw);
            return NULL;
        }
        /* nonblocking + cloexec, portable (no eventfd / pipe2). */
        if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
            close(fds[0]);
            close(fds[1]);
            free(iw);
            return NULL;
        }
        iw->pipe_r = fds[0];
        iw->pipe_w = fds[1];
    }

    if (pthread_mutex_init(&iw->lock, NULL) != 0) {
        close(iw->pipe_r);
        close(iw->pipe_w);
        free(iw);
        return NULL;
    }
    if (pthread_cond_init(&iw->cond, NULL) != 0) {
        pthread_mutex_destroy(&iw->lock);
        close(iw->pipe_r);
        close(iw->pipe_w);
        free(iw);
        return NULL;
    }
    if (pthread_cond_init(&iw->done_cond, NULL) != 0) {
        pthread_cond_destroy(&iw->cond);
        pthread_mutex_destroy(&iw->lock);
        close(iw->pipe_r);
        close(iw->pipe_w);
        free(iw);
        return NULL;
    }

    iw->running = 1;
    if (pthread_create(&iw->thread, NULL, iw_thread_main, iw) != 0) {
        pthread_cond_destroy(&iw->done_cond);
        pthread_cond_destroy(&iw->cond);
        pthread_mutex_destroy(&iw->lock);
        close(iw->pipe_r);
        close(iw->pipe_w);
        free(iw);
        return NULL;
    }
    iw->started = 1;

    if (event_fd_out != NULL)
        *event_fd_out = iw->pipe_r;
    return iw;
}

void
iw_submit(identify_worker_t *iw, const char *dev_path, int settle_ms)
{
    int i;

    if (iw == NULL || dev_path == NULL || dev_path[0] == '\0')
        return;

    pthread_mutex_lock(&iw->lock);

    /* dedup against jobs still queued (a job already in flight has been
     * removed from the queue; allowing a re-submit there costs at most one
     * redundant probe and avoids missing a fresh change). */
    for (i = 0; i < iw->job_count; i++) {
        if (strcmp(iw->jobs[i].dev_path, dev_path) == 0) {
            pthread_mutex_unlock(&iw->lock);
            return;
        }
    }

    if (iw->job_count < IW_QUEUE_MAX) {
        iw_job_t *j = &iw->jobs[iw->job_count++];
        strlcpy_safe(j->dev_path, dev_path, sizeof(j->dev_path));
        j->settle_ms = settle_ms;
        pthread_cond_signal(&iw->cond);
    }

    pthread_mutex_unlock(&iw->lock);
}

int
iw_drain(identify_worker_t *iw, identify_result_t *out, int max)
{
    int n;
    int remaining;
    unsigned char drainbuf[256];
    ssize_t rd;

    if (iw == NULL || out == NULL || max <= 0)
        return 0;

    pthread_mutex_lock(&iw->lock);

    n = iw->result_count;
    if (n > max)
        n = max;
    if (n > 0) {
        memcpy(out, iw->results, (size_t)n * sizeof(out[0]));
        remaining = iw->result_count - n;
        if (remaining > 0)
            memmove(iw->results, &iw->results[n],
                    (size_t)remaining * sizeof(iw->results[0]));
        iw->result_count = remaining;
    } else {
        remaining = 0;
    }

    /* drain all pending wake bytes, then re-arm one if results are still
     * queued so the level-triggered poll() fires again. Done under the
     * lock so a worker push cannot slip between the drain and the re-arm. */
    while ((rd = read(iw->pipe_r, drainbuf, sizeof(drainbuf))) > 0)
        ;
    (void)rd;
    if (remaining > 0) {
        unsigned char one = 1;
        ssize_t wr = write(iw->pipe_w, &one, sizeof(one));
        (void)wr;
    }

    pthread_mutex_unlock(&iw->lock);
    return n;
}

void
iw_stop(identify_worker_t *iw)
{
    if (iw == NULL)
        return;

    pthread_mutex_lock(&iw->lock);
    iw->running = 0;
    pthread_cond_broadcast(&iw->cond);
    pthread_mutex_unlock(&iw->lock);

    if (iw->started) {
        /* Bound the wait portably: wait on done_cond (signaled when the
         * thread exits) up to the timeout. pthread_timedjoin_np does not
         * exist on macOS. */
        struct timespec deadline;
        int finished;

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += IW_STOP_JOIN_TIMEOUT_SEC;

        pthread_mutex_lock(&iw->lock);
        while (!iw->finished) {
            if (pthread_cond_timedwait(&iw->done_cond, &iw->lock,
                                       &deadline) != 0)
                break; /* timed out (or error) */
        }
        finished = iw->finished;
        pthread_mutex_unlock(&iw->lock);

        if (!finished) {
            /* worker is wedged in a probe subprocess. The process is on its
             * way out, so detach and leak the handle rather than block
             * shutdown or risk freeing state the thread still touches. */
            pthread_detach(iw->thread);
            return;
        }
        pthread_join(iw->thread, NULL);
    }

    pthread_cond_destroy(&iw->done_cond);
    pthread_cond_destroy(&iw->cond);
    pthread_mutex_destroy(&iw->lock);
    if (iw->pipe_r >= 0)
        close(iw->pipe_r);
    if (iw->pipe_w >= 0)
        close(iw->pipe_w);
    free(iw);
}
