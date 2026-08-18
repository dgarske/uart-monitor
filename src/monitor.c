/* monitor.c
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
#include "monitor.h"
#include "hotplug.h"
#include "control.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef __APPLE__
#include <sys/inotify.h>
#endif

/* poll() set size: each port contributes a serial fd and (in proxy mode)
 * a PTY master, plus a handful of fixed sources (signal / hot-plug /
 * control / identify). */
#define MAX_POLL_FDS      (MAX_PORTS * 2 + 16)
#define READ_BUF_SIZE     4096
#define PID_FILE          LOG_BASE_DIR "/uart-monitor.pid"
#define STATUS_FILE       LOG_BASE_DIR "/status.json"

/* How often the daemon re-checks that the hardware behind each monitored
 * tty node still matches the label it was opened under. Catches a board
 * swapped onto a node whose remove/add events the daemon missed (e.g. USB
 * renumbering), without requiring a daemon restart. */
#define RECONCILE_INTERVAL_SEC 30

/* ------------------------------------------------------------------ */
/*  sd_notify -- no libsystemd dependency                             */
/* ------------------------------------------------------------------ */

static void
sd_notify_send(const char *state)
{
    const char *sock = getenv("NOTIFY_SOCKET");
    if (!sock)
        return;

    /* plain SOCK_DGRAM (portable): SOCK_CLOEXEC in the type is Linux-only.
     * The fd is closed immediately after the send. */
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    set_nonblock_cloexec(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (sock[0] == '@') {
        /* abstract socket */
        addr.sun_path[0] = '\0';
        strlcpy_safe(addr.sun_path + 1, sock + 1,
                     sizeof(addr.sun_path) - 1);
    } else {
        strlcpy_safe(addr.sun_path, sock, sizeof(addr.sun_path));
    }

    sendto(fd, state, strlen(state), 0,
           (struct sockaddr *)&addr,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                       strlen(sock)));
    close(fd);
}

/* ------------------------------------------------------------------ */
/*  Signal handling via self-pipe                                     */
/*                                                                    */
/*  Portable replacement for Linux signalfd: an async-signal-safe     */
/*  handler writes the signal number as one byte to a pipe whose read */
/*  end sits in the main poll() set. write() is async-signal-safe.    */
/* ------------------------------------------------------------------ */

static int g_signal_pipe_w = -1;

static void
signal_handler(int signo)
{
    unsigned char b = (unsigned char)signo;
    if (g_signal_pipe_w >= 0) {
        ssize_t wr = write(g_signal_pipe_w, &b, 1);
        (void)wr;
    }
}

/* ------------------------------------------------------------------ */
/*  PID file                                                          */
/* ------------------------------------------------------------------ */

static int
pidfile_create(void)
{
    /* check for stale pid file */
    FILE *fp = fopen(PID_FILE, "r");
    if (fp) {
        int old_pid = 0;
        if (fscanf(fp, "%d", &old_pid) == 1 && old_pid > 0) {
            if (kill(old_pid, 0) == 0) {
                fprintf(stderr,
                    "monitor: daemon already running (PID %d)\n", old_pid);
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
        unlink(PID_FILE);
    }

    fp = fopen(PID_FILE, "w");
    if (!fp)
        return -1;
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

static void
pidfile_remove(void)
{
    unlink(PID_FILE);
}

/* ------------------------------------------------------------------ */
/*  PTY symlink management                                            */
/* ------------------------------------------------------------------ */

static void
pty_create_symlink(const char *label, const char *pty_path)
{
    mkdirp(PTY_DIR);

    char link[512];
    snprintf(link, sizeof(link), "%s/%s", PTY_DIR, label);
    symlink_update(pty_path, link);
}

static void
pty_remove_symlink(const char *label)
{
    char link[512];
    snprintf(link, sizeof(link), "%s/%s", PTY_DIR, label);
    unlink(link);
}

/* forward declarations -- defined further down */
static int find_port_by_path(monitor_state_t *state, const char *dev_path);
static int port_needs_probe(const tty_port_t *p);

/* ------------------------------------------------------------------ */
/*  Status JSON                                                       */
/* ------------------------------------------------------------------ */

static void
write_status_json(monitor_state_t *state)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", STATUS_FILE, getpid());

    FILE *fp = fopen(tmp, "w");
    if (!fp)
        return;

    /* extract session name from path */
    const char *session_name = strrchr(state->session_path, '/');
    session_name = session_name ? session_name + 1 : state->session_path;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"pid\": %d,\n", getpid());
    fprintf(fp, "  \"session\": \"%s\",\n", session_name);
    fprintf(fp, "  \"proxy_mode\": %s,\n",
            state->proxy_mode ? "true" : "false");
    fprintf(fp, "  \"port_count\": %d,\n", state->port_count);
    fprintf(fp, "  \"ports\": [\n");

    for (int i = 0; i < state->port_count; i++) {
        monitored_port_t *mp = &state->ports[i];
        const char *board = get_board_name(&mp->identity);

        const char *func = mp->identity.function_name ?
                           mp->identity.function_name : "Unknown";

        fprintf(fp, "    {\n");
        fprintf(fp, "      \"device\": \"%s\",\n", mp->identity.dev_path);
        fprintf(fp, "      \"label\": \"%s\",\n", mp->identity.label);
        fprintf(fp, "      \"board\": \"%s\",\n", board);
        fprintf(fp, "      \"function\": \"%s\",\n", func);
        fprintf(fp, "      \"vid\": \"%04x\",\n", mp->identity.vid);
        fprintf(fp, "      \"pid\": \"%04x\",\n", mp->identity.pid);
        fprintf(fp, "      \"status\": \"%s\",\n",
                mp->yielded ? "yielded" : "monitoring");
        fprintf(fp, "      \"log_file\": \"%s\",\n", mp->log.filepath);
        if (mp->serial.pty_master >= 0) {
            fprintf(fp, "      \"pty_device\": \"%s/%s\",\n",
                    PTY_DIR, mp->identity.label);
            fprintf(fp, "      \"pty_slave\": \"%s\",\n",
                    mp->serial.pty_path);
        }
        fprintf(fp, "      \"bytes_logged\": %zu\n", mp->log.bytes_written);
        fprintf(fp, "    }%s\n",
                (i < state->port_count - 1) ? "," : "");
    }

    fprintf(fp, "  ],\n");

    /* scan all ports and include those not currently monitored. Cheap
     * (sysfs-only) scan: status.json is written after every add / remove /
     * hot-plug / reconcile / control command, so it must never trigger the
     * blocking st-info / STM32_Programmer_CLI probe path. */
    tty_port_t all_ports[MAX_PORTS];
    int nall = scan_all_ports_cheap(all_ports, MAX_PORTS);
    board_id_t bids[MAX_BOARD_IDS];
    int nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(all_ports, nall, bids, nbids);

    /* count non-monitored ports */
    int n_identified = 0;
    for (int i = 0; i < nall; i++) {
        if (find_port_by_path(state, all_ports[i].dev_path) < 0)
            n_identified++;
    }

    fprintf(fp, "  \"total_ports\": %d,\n",
            state->port_count + n_identified);
    fprintf(fp, "  \"identified_ports\": [\n");

    int written = 0;
    for (int i = 0; i < nall; i++) {
        if (find_port_by_path(state, all_ports[i].dev_path) >= 0)
            continue; /* already in "ports" array */

        tty_port_t *p = &all_ports[i];
        const char *board = get_board_name(p);

        const char *func = p->function_name ? p->function_name : "Unknown";

        fprintf(fp, "    {\n");
        fprintf(fp, "      \"device\": \"%s\",\n", p->dev_path);
        fprintf(fp, "      \"label\": \"%s\",\n", p->label);
        fprintf(fp, "      \"board\": \"%s\",\n", board);
        fprintf(fp, "      \"function\": \"%s\",\n", func);
        fprintf(fp, "      \"vid\": \"%04x\",\n", p->vid);
        fprintf(fp, "      \"pid\": \"%04x\",\n", p->pid);
        fprintf(fp, "      \"status\": \"not_monitored\"\n");
        fprintf(fp, "    }%s\n",
                (++written < n_identified) ? "," : "");
    }

    fprintf(fp, "  ]\n}\n");
    fclose(fp);

    rename(tmp, STATUS_FILE);
}

/* ------------------------------------------------------------------ */
/*  Port management                                                   */
/* ------------------------------------------------------------------ */

static int
port_matches_filter(const char *dev_path, const char *filter)
{
    if (!filter || filter[0] == '\0')
        return 1; /* no filter = match all */

    /* comma-separated list of devices or tty names */
    char buf[512];
    strlcpy_safe(buf, filter, sizeof(buf));

    char *saveptr;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        if (strcmp(tok, dev_path) == 0)
            return 1;
        /* also match just the tty name */
        const char *slash = strrchr(dev_path, '/');
        if (slash && strcmp(tok, slash + 1) == 0)
            return 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

static int
add_port(monitor_state_t *state, tty_port_t *identity)
{
    if (state->port_count >= MAX_PORTS)
        return -1;

    /* check filter */
    if (!port_matches_filter(identity->dev_path, state->only_filter))
        return -1;

    /* check for duplicate */
    for (int i = 0; i < state->port_count; i++) {
        if (strcmp(state->ports[i].identity.dev_path,
                   identity->dev_path) == 0)
            return -1; /* already monitoring */
    }

    int idx = state->port_count;
    monitored_port_t *mp = &state->ports[idx];
    memset(mp, 0, sizeof(*mp));
    mp->identity = *identity;
    mp->serial.fd = -1;
    mp->serial.pty_master = -1;
    mp->pty_watch_wd = -1;

    /* open serial port (proxy or read-only) */
    speed_t baud = (identity->baud > 0)
                   ? baud_to_speed(identity->baud)
                   : state->baudrate;
    int rc;
    if (state->proxy_mode)
        rc = serial_open_proxy(&mp->serial, identity->dev_path, baud);
    else
        rc = serial_open(&mp->serial, identity->dev_path, baud);

    if (rc < 0)
        return -1;

    /* build log header */
    /* Sized for the worst case the format can produce: dev_path[256] +
     * label[64] + board_override[128] plus the fixed text and numbers. */
    char header[1024];
    const char *board = get_board_name(identity);

    snprintf(header, sizeof(header),
             "Device: %s (%s)\n"
             "Board: %s | Interface %d | Function: %s\n"
             "Baud: %d 8N1\n",
             identity->dev_path, identity->label,
             board, identity->interface_num,
             identity->function_name ? identity->function_name : "Unknown",
             identity->baud > 0 ? identity->baud : 115200);

    /* open log file -- use label as filename for human-friendly names */
    if (log_open(&mp->log, state->session_path,
                 identity->label, header) < 0) {
        serial_close(&mp->serial);
        return -1;
    }
    mp->log.timestamps = state->timestamps;

    /* create a tty_name.log -> label.log symlink for compatibility */
    if (strcmp(identity->tty_name, identity->label) != 0) {
        char link_path[768];
        char label_log[128];
        snprintf(link_path, sizeof(link_path),
                 "%s/%s.log", state->session_path, identity->tty_name);
        snprintf(label_log, sizeof(label_log), "%s.log", identity->label);
        /* only create symlink if it doesn't already exist */
        if (access(link_path, F_OK) != 0) {
            int sret = symlink(label_log, link_path);
            (void)sret;
        }
    }

    /* tag the serial fd for poll() dispatch. The pollfd set is rebuilt
     * from the ports array each loop iteration, so there is no epoll
     * registration to maintain here. */
    mp->evt.type = EVT_SERIAL;
    mp->evt.index = idx;
    mp->evt.fd = mp->serial.fd;

    /* if proxy mode, tag the PTY master and create its symlink */
    if (mp->serial.pty_master >= 0) {
        mp->evt_pty.type = EVT_PTY;
        mp->evt_pty.index = idx;
        mp->evt_pty.fd = mp->serial.pty_master;

        /* create PTY symlink */
        pty_create_symlink(identity->label, mp->serial.pty_path);

#ifndef __APPLE__
        /* Watch the slave node so a client's exit (close) lets us clear
         * any TIOCEXCL it left behind; see handle_pty_watch(). */
        if (state->inotify_fd >= 0) {
            mp->pty_watch_wd = inotify_add_watch(state->inotify_fd,
                mp->serial.pty_path, IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
            if (mp->pty_watch_wd < 0) {
                fprintf(stderr, "monitor: inotify watch %s [%s]: %s "
                        "(excl heal falls back to reconcile)\n",
                        mp->serial.pty_path, identity->label,
                        strerror(errno));
            }
        }
#endif
    }

    state->port_count++;

    if (mp->serial.pty_master >= 0) {
        printf("  Monitoring: %s [%s] -> %s\n"
               "    PTY proxy: %s/%s -> %s\n",
               identity->dev_path, identity->label, mp->log.filepath,
               PTY_DIR, identity->label, mp->serial.pty_path);
    } else {
        printf("  Monitoring: %s [%s] -> %s\n",
               identity->dev_path, identity->label, mp->log.filepath);
    }

    return idx;
}

static void
remove_port(monitor_state_t *state, int idx)
{
    if (idx < 0 || idx >= state->port_count)
        return;

    monitored_port_t *mp = &state->ports[idx];

    if (mp->serial.pty_master >= 0)
        pty_remove_symlink(mp->identity.label);

#ifndef __APPLE__
    if (mp->pty_watch_wd >= 0 && state->inotify_fd >= 0) {
        /* EINVAL if the kernel already auto-removed it (IN_IGNORED) --
         * harmless, ignore. */
        (void)inotify_rm_watch(state->inotify_fd, mp->pty_watch_wd);
        mp->pty_watch_wd = -1;
    }
#endif

    log_marker(&mp->log, "PORT DISCONNECTED");
    log_close(&mp->log);
    serial_close(&mp->serial);

    printf("  Removed: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    /* shift remaining ports down; the poll() set is rebuilt next
     * iteration, so only the per-slot dispatch index needs fixing up. */
    for (int i = idx; i < state->port_count - 1; i++) {
        state->ports[i] = state->ports[i + 1];
        state->ports[i].evt.index = i;
        state->ports[i].evt_pty.index = i;
    }
    state->port_count--;
}

static int
find_port_by_path(monitor_state_t *state, const char *dev_path)
{
    for (int i = 0; i < state->port_count; i++) {
        if (strcmp(state->ports[i].identity.dev_path, dev_path) == 0)
            return i;
    }
    return -1;
}

/* Find a port by device path, label, or tty name.
 * Tries exact dev_path match first, then label, then tty_name. */
static int
find_port_by_name(monitor_state_t *state, const char *name)
{
    /* try exact device path first */
    int idx = find_port_by_path(state, name);
    if (idx >= 0)
        return idx;

    /* strip /dev/ prefix if present */
    const char *stripped = name;
    if (strncmp(name, "/dev/", 5) == 0)
        stripped = name + 5;

    /* try label match, then tty_name */
    for (int i = 0; i < state->port_count; i++) {
        if (strcmp(state->ports[i].identity.label, stripped) == 0)
            return i;
        if (strcmp(state->ports[i].identity.tty_name, stripped) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Yield / Reclaim                                                   */
/* ------------------------------------------------------------------ */

static void
yield_port(monitor_state_t *state, int idx, char *resp, size_t resp_sz)
{
    monitored_port_t *mp = &state->ports[idx];

    if (mp->yielded) {
        snprintf(resp, resp_sz, "OK already yielded %s\n",
                 mp->identity.dev_path);
        return;
    }

    /* close the serial fd (the poll() set skips yielded ports when it is
     * rebuilt). The PTY master fd stays open so the slave survives for the
     * reader to reconnect after reclaim. */
    if (mp->serial.fd >= 0) {
        close(mp->serial.fd);
        mp->serial.fd = -1;
    }

    mp->yielded = 1;
    log_marker(&mp->log, "PORT YIELDED (released for flashing)");

    printf("  Yielded: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    write_status_json(state);

    snprintf(resp, resp_sz, "OK yielded %s\n", mp->identity.dev_path);
}

static void
reclaim_port(monitor_state_t *state, int idx, char *resp, size_t resp_sz)
{
    monitored_port_t *mp = &state->ports[idx];

    if (!mp->yielded) {
        snprintf(resp, resp_sz, "OK already monitoring %s\n",
                 mp->identity.dev_path);
        return;
    }

    /* reopen serial port */
    int open_flags;
    if (state->proxy_mode)
        open_flags = O_RDWR | O_NOCTTY | O_NONBLOCK;
    else
        open_flags = O_RDONLY | O_NOCTTY | O_NONBLOCK;

    mp->serial.fd = open(mp->identity.dev_path, open_flags);
    if (mp->serial.fd < 0) {
        snprintf(resp, resp_sz, "ERROR cannot reopen %s: %s\n",
                 mp->identity.dev_path, strerror(errno));
        return;
    }

    /* reconfigure termios (use per-device baud if set) */
    speed_t reclaim_baud = (mp->identity.baud > 0)
                           ? baud_to_speed(mp->identity.baud)
                           : state->baudrate;
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    cfsetispeed(&tty, reclaim_baud);
    cfsetospeed(&tty, reclaim_baud);
    tty.c_cflag = reclaim_baud | CS8 | CREAD | CLOCAL;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tcsetattr(mp->serial.fd, TCSANOW, &tty);

    /* refresh the dispatch fd; the poll() set picks the reopened serial
     * fd back up on the next loop rebuild (yielded flag cleared below). */
    mp->evt.fd = mp->serial.fd;

    mp->yielded = 0;
    log_marker(&mp->log, "PORT RECLAIMED (monitoring resumed)");

    printf("  Reclaimed: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    write_status_json(state);

    snprintf(resp, resp_sz, "OK reclaimed %s\n", mp->identity.dev_path);
}

/* ------------------------------------------------------------------ */
/*  Clear logs                                                        */
/* ------------------------------------------------------------------ */

static void
clear_port_log(monitor_state_t *state, int idx, char *resp, size_t resp_sz)
{
    monitored_port_t *mp = &state->ports[idx];
    log_clear(&mp->log);

    printf("  Cleared: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    write_status_json(state);

    snprintf(resp, resp_sz, "OK cleared %s\n", mp->identity.dev_path);
}

static void
clear_all_logs(monitor_state_t *state, char *resp, size_t resp_sz)
{
    for (int i = 0; i < state->port_count; i++) {
        log_clear(&state->ports[i].log);
        printf("  Cleared: %s [%s]\n",
               state->ports[i].identity.dev_path,
               state->ports[i].identity.label);
    }

    write_status_json(state);

    snprintf(resp, resp_sz, "OK cleared %d port(s)\n", state->port_count);
}

/* ------------------------------------------------------------------ */
/*  Control socket command handling                                    */
/* ------------------------------------------------------------------ */

static void
handle_control_cmd(monitor_state_t *state, int client_fd)
{
    char buf[512];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    /* strip trailing newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    char resp[CONTROL_MAX_MSG];

    if (strcmp(buf, "STATUS") == 0) {
        /* write fresh status and send it */
        write_status_json(state);

        FILE *fp = fopen(STATUS_FILE, "r");
        if (fp) {
            size_t nr = fread(resp, 1, sizeof(resp) - 1, fp);
            resp[nr] = '\0';
            fclose(fp);
        } else {
            snprintf(resp, sizeof(resp), "ERROR cannot read status\n");
        }
    } else if (strncmp(buf, "YIELD ", 6) == 0) {
        const char *dev = buf + 6;
        int idx = find_port_by_path(state, dev);
        if (idx < 0) {
            snprintf(resp, sizeof(resp),
                     "ERROR port not found: %s\n", dev);
        } else {
            yield_port(state, idx, resp, sizeof(resp));
        }
    } else if (strncmp(buf, "RECLAIM ", 8) == 0) {
        const char *dev = buf + 8;
        int idx = find_port_by_path(state, dev);
        if (idx < 0) {
            snprintf(resp, sizeof(resp),
                     "ERROR port not found: %s\n", dev);
        } else {
            reclaim_port(state, idx, resp, sizeof(resp));
        }
    } else if (strncmp(buf, "CLEAR", 5) == 0) {
        if (strcmp(buf, "CLEAR --all") == 0 ||
            strcmp(buf, "CLEAR") == 0) {
            clear_all_logs(state, resp, sizeof(resp));
        } else if (strncmp(buf, "CLEAR ", 6) == 0) {
            const char *name = buf + 6;
            int idx = find_port_by_name(state, name);
            if (idx < 0) {
                snprintf(resp, sizeof(resp),
                         "ERROR port not found: %s\n", name);
            } else {
                clear_port_log(state, idx, resp, sizeof(resp));
            }
        }
    } else if (strncmp(buf, "BAUD ", 5) == 0) {
        /* BAUD <device|label> <rate> -- change baud rate on a running port */
        char name[256];
        int rate = 0;
        if (sscanf(buf + 5, "%255s %d", name, &rate) != 2 || rate <= 0) {
            snprintf(resp, sizeof(resp),
                     "ERROR usage: BAUD <device|label> <rate>\n");
        } else {
            int idx = find_port_by_name(state, name);
            if (idx < 0)
                idx = find_port_by_path(state, name);
            if (idx < 0) {
                snprintf(resp, sizeof(resp),
                         "ERROR port not found: %s\n", name);
            } else {
                monitored_port_t *mp = &state->ports[idx];
                speed_t spd = baud_to_speed(rate);
                if (spd == B0) {
                    snprintf(resp, sizeof(resp),
                             "ERROR unsupported baud rate: %d\n", rate);
                } else {
                    struct termios tty;
                    if (tcgetattr(mp->serial.fd, &tty) == 0) {
                        /* cfset*speed sets the baud portably; no CBAUD
                         * masking (Linux-only) needed. */
                        cfsetispeed(&tty, spd);
                        cfsetospeed(&tty, spd);
                        tcsetattr(mp->serial.fd, TCSANOW, &tty);
                        mp->identity.baud = rate;
                        snprintf(resp, sizeof(resp),
                                 "OK baud %d on %s\n", rate,
                                 mp->identity.dev_path);
                    } else {
                        snprintf(resp, sizeof(resp),
                                 "ERROR tcgetattr: %s\n", strerror(errno));
                    }
                }
            }
        }
    } else if (strcmp(buf, "QUIT") == 0) {
        snprintf(resp, sizeof(resp), "OK shutting down\n");
        state->running = 0;
    } else {
        snprintf(resp, sizeof(resp),
                 "ERROR unknown command: %s\n", buf);
    }

    /* send response (best effort) and close. Loop because writes on
     * Unix sockets can short-return for payloads larger than the
     * kernel buffer (we allow up to 64 KB now). */
    size_t resp_len = strlen(resp);
    size_t sent = 0;
    while (sent < resp_len) {
        ssize_t nw = write(client_fd, resp + sent, resp_len - sent);
        if (nw < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (nw == 0)
            break;
        sent += (size_t)nw;
    }
    close(client_fd);
}

/* ------------------------------------------------------------------ */
/*  Signal handling                                                    */
/* ------------------------------------------------------------------ */

static void
handle_signal(monitor_state_t *state)
{
    unsigned char b;
    ssize_t n;

    /* Drain every byte the signal handler queued; act on each signal. */
    while ((n = read(state->signal_fd, &b, 1)) > 0) {
    int signo = (int)b;

    switch (signo) {
    case SIGTERM:
    case SIGINT:
        printf("\nReceived SIG%s, shutting down...\n",
               signo == SIGTERM ? "TERM" : "INT");
        state->running = 0;
        break;

    case SIGHUP: {
        printf("Received SIGHUP, rescanning ports...\n");

        /* rescan and add any new ports. Cheap (sysfs-only) identify on the
         * event loop; ambiguous ports are resolved off-thread by the
         * worker. */
        tty_port_t ports[MAX_PORTS];
        int nports = scan_all_ports_cheap(ports, MAX_PORTS);

        board_id_t bids[MAX_BOARD_IDS];
        int nbids = load_board_config(bids, MAX_BOARD_IDS);
        if (nbids > 0)
            apply_board_config(ports, nports, bids, nbids);

        for (int i = 0; i < nports; i++) {
            /* add_port checks for duplicates internally */
            add_port(state, &ports[i]);
            /* probe only ports we are actually monitoring (respects the
             * --only filter); the worker resolves the real board name. */
            if (port_needs_probe(&ports[i]) &&
                find_port_by_path(state, ports[i].dev_path) >= 0)
                iw_submit(state->iw, ports[i].dev_path, 0);
        }

        write_status_json(state);
        break;
    }
    }
    }
}

/* ------------------------------------------------------------------ */
/*  Re-identify / relabel                                              */
/* ------------------------------------------------------------------ */

/* Returns 1 if the freshly probed identity represents a different device
 * (or a different resolved board) than the one currently stored. The USB
 * serial is authoritative per physical ST-LINK / probe; fall back to the
 * resolved label only when neither identity exposes a serial. */
static int
identity_changed(const tty_port_t *cur, const tty_port_t *fresh)
{
    if (cur->serial[0] != '\0' || fresh->serial[0] != '\0')
        return strcmp(cur->serial, fresh->serial) != 0;
    return strcmp(cur->label, fresh->label) != 0;
}

/* Drop the stale entry at idx and re-add under the fresh identity so the
 * log is reopened with the correct label. remove_port() writes a
 * "PORT DISCONNECTED" marker and closes the old log; add_port() opens a
 * new log / PTY under fresh->label. Use this only when the hardware behind
 * the node actually changed (board swap): the old serial fd is stale and
 * must be re-opened. For a same-device label refinement use
 * relabel_port_inplace(), which never re-opens the USB device. */
static void
relabel_port(monitor_state_t *state, int idx, tty_port_t *fresh)
{
    if (idx < 0 || idx >= state->port_count)
        return;
    printf("  Relabel: %s [%s] -> [%s]\n",
           fresh->dev_path, state->ports[idx].identity.label, fresh->label);
    remove_port(state, idx);
    add_port(state, fresh);
}

/* Relabel a port in place: the physical device is unchanged (same USB
 * serial) and only its resolved label improved (e.g. generic ST-LINK
 * fallback -> NUCLEO board name). Keep the serial and PTY master fds -- and
 * their epoll registrations, which key off this unchanged slot -- open, so
 * we never call open() on the USB device again. open()/termios on a wedged
 * device blocks uninterruptibly in the kernel, so re-opening a perfectly
 * good fd just to rename a log is both wasteful and risky. Only the log
 * file and the friendly PTY symlink follow the new label. */
static void
relabel_port_inplace(monitor_state_t *state, int idx, tty_port_t *fresh)
{
    monitored_port_t *mp;
    /* Sized for the worst case the format can produce: dev_path[256] +
     * label[64] + board_override[128] plus the fixed text and numbers. */
    char header[1024];
    char marker[160];
    const char *board;

    if (idx < 0 || idx >= state->port_count)
        return;
    mp = &state->ports[idx];

    printf("  Relabel: %s [%s] -> [%s] (in place)\n",
           fresh->dev_path, mp->identity.label, fresh->label);

    /* drop the old friendly-name PTY symlink before the label changes */
    if (mp->serial.pty_master >= 0)
        pty_remove_symlink(mp->identity.label);

    /* close the old (provisional-label) log; the serial/PTY fds stay open */
    snprintf(marker, sizeof(marker), "RELABELED to %s", fresh->label);
    log_marker(&mp->log, marker);
    log_close(&mp->log);

    /* adopt the new identity. This is metadata only -- the serial fds and
     * epoll context (mp->serial, mp->evt, mp->evt_pty) live outside the
     * identity struct and are deliberately left untouched. */
    mp->identity = *fresh;

    board = get_board_name(fresh);

    snprintf(header, sizeof(header),
             "Device: %s (%s)\n"
             "Board: %s | Interface %d | Function: %s\n"
             "Baud: %d 8N1\n",
             fresh->dev_path, fresh->label,
             board, fresh->interface_num,
             fresh->function_name ? fresh->function_name : "Unknown",
             fresh->baud > 0 ? fresh->baud : 115200);

    if (log_open(&mp->log, state->session_path, fresh->label, header) < 0) {
        /* cannot open the new log -- drop the port cleanly so we do not
         * leave a slot with a closed log still wired into epoll. */
        fprintf(stderr, "monitor: relabel %s: log_open failed\n",
                fresh->dev_path);
        remove_port(state, idx);
        return;
    }
    mp->log.timestamps = state->timestamps;

    /* compat symlink tty_name.log -> label.log (mirrors add_port) */
    if (strcmp(fresh->tty_name, fresh->label) != 0) {
        char link_path[768];
        char label_log[128];
        snprintf(link_path, sizeof(link_path),
                 "%s/%s.log", state->session_path, fresh->tty_name);
        snprintf(label_log, sizeof(label_log), "%s.log", fresh->label);
        if (access(link_path, F_OK) != 0) {
            int sret = symlink(label_log, link_path);
            (void)sret;
        }
    }

    /* recreate the friendly-name PTY symlink pointing at the same slave */
    if (mp->serial.pty_master >= 0)
        pty_create_symlink(fresh->label, mp->serial.pty_path);
}

/* True if a cheaply-identified port still needs the background probe to
 * resolve its real board name: a known but ambiguous device (e.g. an
 * STLINK-V3 shared VID:PID) that the product string / ~/.boards pin did
 * not already resolve. Non-ambiguous and pinned ports are final after the
 * cheap identify, so we never probe them. */
static int
port_needs_probe(const tty_port_t *p)
{
    if (p->board_override[0])
        return 0; /* pinned by ~/.boards -- label is authoritative */
    if (p->board_match)
        return 0; /* already resolved by USB product string */
    return p->known && known_device_is_ambiguous(p->known);
}

/* Apply one resolved identity handed back by the identify worker. The
 * worker only refines the *label* of a device whose serial is unchanged,
 * so compare by label (identity_changed() keys on serial and would miss a
 * generic->board upgrade). Stale results (device removed or swapped since
 * submit) are dropped. Returns 1 if a port was relabeled. */
static int
apply_identify_result(monitor_state_t *state, const identify_result_t *r)
{
    board_id_t bids[MAX_BOARD_IDS];
    tty_port_t fresh;
    int idx, nbids;

    idx = find_port_by_path(state, r->dev_path);
    if (idx < 0)
        return 0; /* device went away while the probe was in flight */
    if (state->ports[idx].yielded)
        return 0; /* held for flashing -- never touch */

    /* the device behind the node must still be the one we probed */
    if (strcmp(state->ports[idx].identity.serial, r->identity.serial) != 0)
        return 0; /* swapped since submit -- a fresh event will re-probe */

    fresh = r->identity;
    nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(&fresh, 1, bids, nbids);

    if (strcmp(state->ports[idx].identity.label, fresh.label) == 0)
        return 0; /* probe did not change the label (still generic) */

    /* same physical device (serial matched above), only the label
     * improved -- relabel without re-opening the serial fd. */
    relabel_port_inplace(state, idx, &fresh);
    return 1;
}

/* Cheap sysfs rescan: add any monitor-eligible /dev node that is present
 * but not currently in the table. This recovers a port whose hot-plug
 * add() failed transiently -- most commonly when the open() raced udev and
 * hit EACCES because the plugdev group / ACL had not been applied to the
 * freshly created node yet -- without waiting for a manual SIGHUP or a
 * daemon restart. Mirrors the SIGHUP rescan path. Returns the number of
 * ports newly added. */
static int
rescan_and_add(monitor_state_t *state)
{
    tty_port_t ports[MAX_PORTS];
    board_id_t bids[MAX_BOARD_IDS];
    int nports, nbids, i, added;

    nports = scan_all_ports_cheap(ports, MAX_PORTS);
    nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(ports, nports, bids, nbids);

    added = 0;
    for (i = 0; i < nports; i++) {
        if (find_port_by_path(state, ports[i].dev_path) >= 0)
            continue; /* already monitoring */
        if (add_port(state, &ports[i]) < 0)
            continue; /* filtered out, or still failing -- retry next pass */
        added++;
        /* resolve the real board name off-thread if still ambiguous */
        if (port_needs_probe(&ports[i]) &&
            find_port_by_path(state, ports[i].dev_path) >= 0)
            iw_submit(state->iw, ports[i].dev_path, 0);
    }
    return added;
}

/* Bring the next reconcile forward to ~800ms from now (unless one is
 * already due sooner). Used to retry a transient hot-plug add() failure
 * quickly (the udev ACL race resolves in tens of ms) instead of leaving
 * the port dark until the next full reconcile interval. */
static void
schedule_fast_reconcile(monitor_state_t *state)
{
    struct timespec now;
    struct timespec soon;

    clock_gettime(CLOCK_MONOTONIC, &now);
    soon = now;
    soon.tv_nsec += 800 * 1000 * 1000; /* +800ms */
    if (soon.tv_nsec >= 1000000000L) {
        soon.tv_nsec -= 1000000000L;
        soon.tv_sec += 1;
    }

    if (soon.tv_sec < state->reconcile_deadline.tv_sec ||
        (soon.tv_sec == state->reconcile_deadline.tv_sec &&
         soon.tv_nsec < state->reconcile_deadline.tv_nsec))
        state->reconcile_deadline = soon;
}

/* Periodic sweep: for each monitored port, cheaply re-read the sysfs USB
 * serial behind its tty node. If the node has gone, drop the port. If the
 * serial changed, the hardware behind the node was swapped -- hand it to
 * the identify worker to re-probe and relabel off the event loop. Ports
 * yielded for flashing are left untouched. Finally, pick up any eligible
 * node that is present but not monitored (self-heal for a transient add
 * failure). */
static void
reconcile_ports(monitor_state_t *state)
{
    char serial[64];
    int i, rc, changed;

    changed = 0;
    for (i = 0; i < state->port_count; i++) {
        monitored_port_t *mp = &state->ports[i];

        if (mp->yielded)
            continue;

        /* Heal a PTY slave left exclusive by an exiting client. On Linux
         * the inotify close-watch does this instantly; this pass covers
         * macOS (no inotify/TIOCGEXCL) and any port whose watch add
         * failed. Degraded-mode tradeoff: this can drop exclusivity out
         * from under a still-attached client, which beats a permanently
         * EBUSY pty. */
        if (mp->serial.pty_master >= 0 && mp->pty_watch_wd < 0) {
            if (serial_pty_clear_excl(&mp->serial) > 0)
                printf("  Reconcile: cleared PTY excl on [%s]\n",
                       mp->identity.label);
        }

        rc = read_port_serial(mp->identity.dev_path, serial, sizeof(serial));
        if (rc < 0) {
            /* node vanished without a remove event -- drop it */
            printf("  Reconcile: %s gone, removing [%s]\n",
                   mp->identity.dev_path, mp->identity.label);
            remove_port(state, i);
            i--; /* array shifted down */
            changed = 1;
            continue;
        }

        if (strcmp(serial, mp->identity.serial) == 0)
            continue; /* same physical device -- nothing to do */

        /* hardware behind this node changed -- re-identify off-thread.
         * No settle delay: the node is already enumerated. The worker's
         * result is applied later via apply_identify_result(). */
        iw_submit(state->iw, mp->identity.dev_path, 0);
    }

    /* Self-heal: re-add any eligible node that is present but not currently
     * monitored. Recovers a port whose hot-plug add() failed transiently
     * (e.g. the open raced udev's ACL and got EACCES) instead of leaving it
     * dark until the next SIGHUP / restart. */
    if (rescan_and_add(state) > 0)
        changed = 1;

    if (changed)
        write_status_json(state);
}

/* Drain and apply all resolved identities the worker has posted. */
static void
handle_identify_done(monitor_state_t *state)
{
    identify_result_t results[MAX_PORTS];
    int n, i, changed;

    n = iw_drain(state->iw, results, MAX_PORTS);
    changed = 0;
    for (i = 0; i < n; i++) {
        if (apply_identify_result(state, &results[i]))
            changed = 1;
    }
    if (changed)
        write_status_json(state);
}

#ifndef __APPLE__
/* A client closed a proxied PTY slave. If it left TIOCEXCL set (screen
 * does), the pty pair never dies -- the daemon holds the master and a
 * keeper slave fd -- so the kernel never clears exclusivity and every
 * later open() gets EBUSY. Clear it via the keeper fd now that the
 * client is gone. */
static void
handle_pty_watch(monitor_state_t *state)
{
    char buf[4096];
    ssize_t n;

    while ((n = read(state->inotify_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
            const struct inotify_event *ie =
                (const struct inotify_event *)(buf + off);
            int i;

            if (ie->mask & IN_Q_OVERFLOW) {
                /* lost events: sweep every proxied port */
                for (i = 0; i < state->port_count; i++) {
                    if (serial_pty_clear_excl(
                            &state->ports[i].serial) > 0)
                        printf("  PTY excl cleared: [%s]\n",
                               state->ports[i].identity.label);
                }
            }
            else {
                /* map wd -> port: linear search; the wd lives in the
                 * port struct so ports[] compaction cannot desync it */
                for (i = 0; i < state->port_count; i++) {
                    monitored_port_t *mp = &state->ports[i];
                    if (mp->pty_watch_wd != ie->wd)
                        continue;
                    if (ie->mask & IN_IGNORED) {
                        mp->pty_watch_wd = -1; /* watch auto-removed */
                    }
                    else if (ie->mask &
                             (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)) {
                        if (serial_pty_clear_excl(&mp->serial) > 0)
                            printf("  PTY excl cleared: [%s]\n",
                                   mp->identity.label);
                    }
                    break;
                }
            }
            off += (ssize_t)(sizeof(struct inotify_event) + ie->len);
        }
    }
    /* n < 0 with EAGAIN ends the drain; other errors are transient
     * and retried on the next poll wakeup */
}
#endif /* !__APPLE__ */

/* ------------------------------------------------------------------ */
/*  Hot-plug handling                                                  */
/* ------------------------------------------------------------------ */

static void
handle_hotplug(monitor_state_t *state)
{
    hotplug_event_t hev;
    int ret = hotplug_read(state->hotplug_fd, &hev);
    if (ret <= 0)
        return;

    if (hev.action == HOTPLUG_ADD) {
        printf("  Hot-plug: %s added\n", hev.devpath);

        /* Cheap (sysfs-only) identify so we can start logging immediately;
         * the slow SWD/CLI probe runs off-thread in the identify worker.
         * No usleep here -- the event loop must not stall. */
        tty_port_t port;
        int rc = identify_port_cheap(hev.devpath, &port);

        if (rc == 0) {
            /* apply board config */
            board_id_t bids[MAX_BOARD_IDS];
            int nbids = load_board_config(bids, MAX_BOARD_IDS);
            int existing;
            if (nbids > 0)
                apply_board_config(&port, 1, bids, nbids);

            /* If this node was already monitored but the hardware behind
             * it changed (board swapped onto the same tty node), drop the
             * stale entry so it reopens under the new label. A port
             * yielded for flashing is left untouched. Unchanged identities
             * fall through to add_port(), which dedups by path. */
            existing = find_port_by_path(state, hev.devpath);
            if (existing >= 0 && !state->ports[existing].yielded &&
                identity_changed(&state->ports[existing].identity, &port))
                relabel_port(state, existing, &port);
            else
                add_port(state, &port);

            write_status_json(state);

            /* hand off to the worker to resolve the real board name if the
             * cheap label is still a generic/ambiguous fallback. Give a USB
             * settle window since the device was just hot-plugged. Skip
             * yielded ports. */
            existing = find_port_by_path(state, hev.devpath);
            if (existing >= 0 && !state->ports[existing].yielded &&
                port_needs_probe(&state->ports[existing].identity))
                iw_submit(state->iw, hev.devpath, 800);

            /* If the add failed, the node is enumerated but not monitored --
             * most often the open raced udev's ACL and got EACCES. Arm a
             * fast reconcile to retry shortly rather than leaving it dark
             * until the periodic sweep. */
            if (existing < 0 &&
                port_matches_filter(hev.devpath, state->only_filter))
                schedule_fast_reconcile(state);
        }
    } else if (hev.action == HOTPLUG_REMOVE) {
        printf("  Hot-plug: %s removed\n", hev.devpath);

        int idx = find_port_by_path(state, hev.devpath);
        if (idx >= 0) {
            remove_port(state, idx);
            write_status_json(state);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Flush partial lines on timeout                                    */
/* ------------------------------------------------------------------ */

static void
flush_stale_lines(monitor_state_t *state)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int i = 0; i < state->port_count; i++) {
        monitored_port_t *mp = &state->ports[i];
        if (mp->log.linebuf_len > 0) {
            long elapsed_ms =
                (now.tv_sec - mp->log.last_flush.tv_sec) * 1000 +
                (now.tv_nsec - mp->log.last_flush.tv_nsec) / 1000000;
            if (elapsed_ms > 200)
                log_flush(&mp->log);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main event loop                                                   */
/* ------------------------------------------------------------------ */

int
cmd_monitor(int argc, char *argv[])
{
    monitor_state_t state;
    memset(&state, 0, sizeof(state));
    state.running = 1;
    state.baudrate = B115200;
    state.signal_fd = -1;
    state.hotplug_fd = -1;
    state.control_fd = -1;
    state.identify_fd = -1;
    state.inotify_fd = -1;
    state.iw = NULL;

    int foreground = 0;

    /* Under systemd stdout is a pipe, so glibc block-buffers it and
     * runtime event lines (removals, reconciles, PTY excl heals) sit in
     * the buffer instead of reaching the journal. Line-buffer it. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Ignore SIGPIPE: a closed PTY slave or disconnected control socket
     * must not kill the daemon. write() returns -1/EPIPE instead. */
    signal(SIGPIPE, SIG_IGN);

    /* parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 ||
            strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--systemd") == 0) {
            state.systemd_mode = 1;
            foreground = 1;
        } else if (strcmp(argv[i], "--proxy") == 0 ||
                   strcmp(argv[i], "-p") == 0) {
            state.proxy_mode = 1;
        } else if (strcmp(argv[i], "--timestamps") == 0 ||
                   strcmp(argv[i], "-t") == 0) {
            state.timestamps = 1;
        } else if ((strcmp(argv[i], "-b") == 0 ||
                    strcmp(argv[i], "--baud") == 0) && i + 1 < argc) {
            state.baudrate = baud_to_speed(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            strlcpy_safe(state.only_filter, argv[++i],
                        sizeof(state.only_filter));
        }
    }

    /* ensure base directory exists */
    if (mkdirp(LOG_BASE_DIR) < 0) {
        fprintf(stderr, "monitor: cannot create %s\n", LOG_BASE_DIR);
        return 1;
    }

    /* PID file */
    if (pidfile_create() < 0)
        return 1;

    /* create session */
    if (log_create_session(state.session_path,
                           sizeof(state.session_path)) < 0) {
        pidfile_remove();
        return 1;
    }

    /* create PTY directory for proxy mode */
    if (state.proxy_mode) {
        if (mkdirp(PTY_DIR) < 0) {
            fprintf(stderr, "monitor: cannot create %s\n", PTY_DIR);
        }
    }

    /* prune old sessions */
    log_prune_sessions(LOG_MAX_SESSIONS);

    printf("uart-monitor starting%s...\n",
           state.proxy_mode ? " (proxy mode)" : "");
    printf("Session: %s\n", state.session_path);

    /* scan and identify ports. Cheap (sysfs-only) identify so the daemon
     * comes up and signals systemd READY in milliseconds even with many
     * ST-LINK probes attached; the slow SWD/CLI probe that resolves
     * ambiguous boards runs afterward in the identify worker. */
    tty_port_t ports[MAX_PORTS];
    int nports = scan_all_ports_cheap(ports, MAX_PORTS);

    /* load board config */
    board_id_t bids[MAX_BOARD_IDS];
    int nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(ports, nports, bids, nbids);

    printf("Found %d serial port(s)\n", nports);

    /* setup signal self-pipe: an async-signal-safe handler writes the
     * signal number to the pipe; its read end sits in the poll() set. */
    {
        int sp[2];
        if (pipe(sp) == 0 &&
            set_nonblock_cloexec(sp[0]) == 0 &&
            set_nonblock_cloexec(sp[1]) == 0) {
            state.signal_fd = sp[0];
            g_signal_pipe_w = sp[1];

            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = signal_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            sigaction(SIGTERM, &sa, NULL);
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGHUP, &sa, NULL);

            state.evt_signal.type = EVT_SIGNAL;
            state.evt_signal.fd = state.signal_fd;
        } else {
            fprintf(stderr, "monitor: signal pipe setup failed: %s\n",
                    strerror(errno));
        }
    }

    /* setup hot-plug */
    state.hotplug_fd = hotplug_init();
    if (state.hotplug_fd >= 0) {
        state.evt_hotplug.type = EVT_HOTPLUG;
        state.evt_hotplug.fd = state.hotplug_fd;
    }

#ifndef __APPLE__
    /* inotify instance for PTY slave close-heal (proxy mode). Must be
     * set up before the add_port loop below so startup ports get
     * watches. */
    if (state.proxy_mode) {
        state.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (state.inotify_fd >= 0) {
            state.evt_inotify.type = EVT_PTY_WATCH;
            state.evt_inotify.fd = state.inotify_fd;
        } else {
            fprintf(stderr, "monitor: inotify init: %s "
                    "(PTY excl heal via reconcile only)\n",
                    strerror(errno));
        }
    }
#endif

    /* arm the periodic reconcile deadline (poll() timeout drives it) */
    clock_gettime(CLOCK_MONOTONIC, &state.reconcile_deadline);
    state.reconcile_deadline.tv_sec += RECONCILE_INTERVAL_SEC;

    /* setup control socket */
    state.control_fd = control_init(CONTROL_SOCK_PATH);
    if (state.control_fd >= 0) {
        state.evt_control.type = EVT_CONTROL;
        state.evt_control.fd = state.control_fd;
    }

    /* start the identify worker. Its self-pipe becomes readable when
     * resolved identities are pending. */
    state.iw = iw_start(&state.identify_fd);
    if (state.iw != NULL && state.identify_fd >= 0) {
        state.evt_identify.type = EVT_IDENTIFY_DONE;
        state.evt_identify.fd = state.identify_fd;
    } else {
        fprintf(stderr, "monitor: identify worker failed to start; "
                        "ambiguous boards keep generic labels\n");
    }

    /* open all serial ports */
    for (int i = 0; i < nports; i++)
        add_port(&state, &ports[i]);

    /* resolve ambiguous board names off-thread (no settle delay: these
     * devices are already enumerated at startup). Only probe ports we are
     * actually monitoring, so a --only filter is not bypassed. */
    for (int i = 0; i < nports; i++) {
        if (port_needs_probe(&ports[i]) &&
            find_port_by_path(&state, ports[i].dev_path) >= 0)
            iw_submit(state.iw, ports[i].dev_path, 0);
    }

    /* write initial status */
    write_status_json(&state);

    if (state.port_count == 0) {
        printf("No matching serial ports to monitor "
               "(will detect hot-plugged devices)\n");
    }

    /* notify systemd we're ready */
    if (state.systemd_mode)
        sd_notify_send("READY=1");

    printf("Monitoring... (Ctrl-C to stop)\n");
    if (!foreground)
        printf("Logs: %s/latest/*.log\n", LOG_BASE_DIR);
    if (state.proxy_mode)
        printf("PTY devices: %s/*\n", PTY_DIR);

    /* ---- main event loop ---- */
    struct pollfd pfds[MAX_POLL_FDS];
    event_ctx_t  *pctx[MAX_POLL_FDS];
    char read_buf[READ_BUF_SIZE];

    while (state.running) {
        int npfd = 0;
        struct timespec now;
        long recon_ms;
        int timeout_ms;
        int stop;

        /* ---- build the poll set fresh each iteration ---- */
        if (state.signal_fd >= 0) {
            pfds[npfd].fd = state.signal_fd;
            pfds[npfd].events = POLLIN;
            pctx[npfd] = &state.evt_signal;
            npfd++;
        }
        if (state.hotplug_fd >= 0) {
            pfds[npfd].fd = state.hotplug_fd;
            pfds[npfd].events = POLLIN;
            pctx[npfd] = &state.evt_hotplug;
            npfd++;
        }
        if (state.control_fd >= 0) {
            pfds[npfd].fd = state.control_fd;
            pfds[npfd].events = POLLIN;
            pctx[npfd] = &state.evt_control;
            npfd++;
        }
        if (state.identify_fd >= 0) {
            pfds[npfd].fd = state.identify_fd;
            pfds[npfd].events = POLLIN;
            pctx[npfd] = &state.evt_identify;
            npfd++;
        }
        if (state.inotify_fd >= 0) {
            pfds[npfd].fd = state.inotify_fd;
            pfds[npfd].events = POLLIN;
            pctx[npfd] = &state.evt_inotify;
            npfd++;
        }
        for (int i = 0; i < state.port_count && npfd < MAX_POLL_FDS - 1;
             i++) {
            monitored_port_t *mp = &state.ports[i];
            if (mp->serial.fd >= 0 && !mp->yielded) {
                mp->evt.type = EVT_SERIAL;
                mp->evt.index = i;
                mp->evt.fd = mp->serial.fd;
                pfds[npfd].fd = mp->serial.fd;
                pfds[npfd].events = POLLIN;
                pctx[npfd] = &mp->evt;
                npfd++;
            }
            if (mp->serial.pty_master >= 0) {
                mp->evt_pty.type = EVT_PTY;
                mp->evt_pty.index = i;
                mp->evt_pty.fd = mp->serial.pty_master;
                pfds[npfd].fd = mp->serial.pty_master;
                pfds[npfd].events = POLLIN;
                pctx[npfd] = &mp->evt_pty;
                npfd++;
            }
        }

        /* ---- timeout: min of reconcile deadline and 200ms line flush ---- */
        clock_gettime(CLOCK_MONOTONIC, &now);
        recon_ms = (state.reconcile_deadline.tv_sec - now.tv_sec) * 1000L +
                   (state.reconcile_deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (recon_ms < 0)
            recon_ms = 0;
        timeout_ms = (int)recon_ms;
        for (int i = 0; i < state.port_count; i++) {
            if (state.ports[i].log.linebuf_len > 0) {
                if (timeout_ms > 200)
                    timeout_ms = 200;
                break;
            }
        }

        int nready = poll(pfds, (nfds_t)npfd, timeout_ms);

        if (nready < 0) {
            if (errno == EINTR)
                continue; /* signal delivered; drain via pipe next loop */
            /* Transient poll errors must not kill the daemon. Log and
             * retry after a short back-off to avoid a tight error loop. */
            fprintf(stderr, "monitor: poll: %s (continuing)\n",
                    strerror(errno));
            usleep(100000); /* 100ms */
            continue;
        }

        stop = 0;
        for (int i = 0; i < npfd && !stop; i++) {
            event_ctx_t *ctx;

            if (pfds[i].revents == 0)
                continue;
            ctx = pctx[i];
            if (!ctx)
                continue;

            switch (ctx->type) {
            case EVT_SIGNAL:
                handle_signal(&state);
                break;

            case EVT_HOTPLUG:
                /* add/relabel/remove may shift ports[] and invalidate the
                 * rest of this batch's ctx pointers -- stop after it. */
                handle_hotplug(&state);
                stop = 1;
                break;

            case EVT_IDENTIFY_DONE:
                /* relabel shifts ports[] -- stop after applying. */
                handle_identify_done(&state);
                stop = 1;
                break;

            case EVT_PTY_WATCH:
#ifndef __APPLE__
                /* never shifts ports[], no stop needed */
                handle_pty_watch(&state);
#endif
                break;

            case EVT_CONTROL: {
                /* accept new control client (portable: accept + fcntl) */
                int cfd = accept(state.control_fd, NULL, NULL);
                if (cfd >= 0) {
                    set_nonblock_cloexec(cfd);
                    handle_control_cmd(&state, cfd);
                }
                break;
            }

            case EVT_SERIAL: {
                int idx = ctx->index;
                if (idx < 0 || idx >= state.port_count)
                    break;

                monitored_port_t *mp = &state.ports[idx];
                ssize_t nr = read(mp->serial.fd, read_buf,
                                  sizeof(read_buf));

                if (nr > 0) {
                    log_write(&mp->log, read_buf, (size_t)nr);
                    mp->bytes_read += (size_t)nr;

                    /* proxy mode: forward serial data to PTY master
                     * so anyone reading the PTY slave sees the output.
                     * EPIPE/EAGAIN/EIO are expected (no reader / buffer
                     * full / slave closed) and are silently dropped --
                     * SIGPIPE is ignored so the daemon stays alive. */
                    if (mp->serial.pty_master >= 0) {
                        ssize_t nw = write(mp->serial.pty_master,
                                           read_buf, (size_t)nr);
                        if (nw < 0 && errno != EPIPE &&
                            errno != EAGAIN && errno != EWOULDBLOCK &&
                            errno != EIO) {
                            fprintf(stderr,
                                "monitor: PTY write %s: %s\n",
                                mp->identity.dev_path, strerror(errno));
                        }
                    }
                } else if (nr == 0 ||
                           (nr < 0 && errno != EAGAIN &&
                            errno != EWOULDBLOCK)) {
                    /* port disconnected or error (also reached via
                     * POLLHUP/POLLERR, which make read return 0 / error) */
                    fprintf(stderr, "monitor: read %s: %s\n",
                            mp->identity.dev_path,
                            nr == 0 ? "EOF" : strerror(errno));
                    remove_port(&state, idx);
                    write_status_json(&state);
                    /* ports[] shifted -- stop processing this batch */
                    stop = 1;
                }
                break;
            }

            case EVT_PTY: {
                /* proxy mode: user wrote to PTY slave, forward to serial */
                int idx = ctx->index;
                if (idx < 0 || idx >= state.port_count)
                    break;

                monitored_port_t *mp = &state.ports[idx];
                if (mp->serial.pty_master < 0 || mp->serial.fd < 0)
                    break;

                ssize_t nr = read(mp->serial.pty_master, read_buf,
                                  sizeof(read_buf));

                if (nr > 0) {
                    /* forward to real serial port. EPIPE / EIO / EAGAIN
                     * are silently dropped; other errors get logged. */
                    ssize_t nw = write(mp->serial.fd,
                                       read_buf, (size_t)nr);
                    if (nw < 0 && errno != EPIPE &&
                        errno != EAGAIN && errno != EWOULDBLOCK &&
                        errno != EIO) {
                        fprintf(stderr,
                            "monitor: serial write %s: %s\n",
                            mp->identity.dev_path, strerror(errno));
                    }
                } else if (nr == 0 ||
                           (nr < 0 && errno != EAGAIN &&
                            errno != EWOULDBLOCK && errno != EIO)) {
                    /* PTY slave was closed -- this is normal */
                }
                break;
            }
            }
        }

        /* periodic reconcile once the deadline passes */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > state.reconcile_deadline.tv_sec ||
            (now.tv_sec == state.reconcile_deadline.tv_sec &&
             now.tv_nsec >= state.reconcile_deadline.tv_nsec)) {
            reconcile_ports(&state);
            clock_gettime(CLOCK_MONOTONIC, &state.reconcile_deadline);
            state.reconcile_deadline.tv_sec += RECONCILE_INTERVAL_SEC;
        }

        /* flush partial lines older than 200ms */
        flush_stale_lines(&state);
    }

    /* ---- cleanup ---- */
    printf("Shutting down...\n");

    /* stop the identify worker first so no relabel races the teardown.
     * iw_stop() bounds its wait so a wedged probe cannot hang shutdown. */
    iw_stop(state.iw);
    state.iw = NULL;

    for (int i = state.port_count - 1; i >= 0; i--) {
        monitored_port_t *mp = &state.ports[i];
        if (mp->serial.pty_master >= 0)
            pty_remove_symlink(mp->identity.label);
        log_marker(&mp->log, "MONITOR STOPPED");
        log_close(&mp->log);
        serial_close(&mp->serial);
    }

    if (state.hotplug_fd >= 0)
        hotplug_close(state.hotplug_fd);
    if (state.inotify_fd >= 0)
        close(state.inotify_fd); /* frees all remaining watches */
    control_close(state.control_fd, CONTROL_SOCK_PATH);
    if (state.signal_fd >= 0)
        close(state.signal_fd);
    if (g_signal_pipe_w >= 0) {
        close(g_signal_pipe_w);
        g_signal_pipe_w = -1;
    }

    pidfile_remove();
    unlink(STATUS_FILE);

    /* clean up PTY directory if empty */
    if (state.proxy_mode)
        rmdir(PTY_DIR);

    if (state.systemd_mode)
        sd_notify_send("STOPPING=1");

    printf("Stopped.\n");
    return 0;
}
