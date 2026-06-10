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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EPOLL_EVENTS  (MAX_PORTS * 2 + 16)
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

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;

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

/* forward declaration -- defined further down */
static int find_port_by_path(monitor_state_t *state, const char *dev_path);

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
        const char *board = "Unknown";
        if (mp->identity.board_override)
            board = mp->identity.board_override;
        else if (mp->identity.known && mp->identity.known->boards[0])
            board = mp->identity.known->boards[0];

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

    /* scan all ports and include those not currently monitored */
    tty_port_t all_ports[MAX_PORTS];
    int nall = scan_all_ports(all_ports, MAX_PORTS);
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
        const char *board = "Unknown";
        if (p->board_override)
            board = p->board_override;
        else if (p->board_match)
            board = p->board_match;
        else if (p->known && p->known->boards[0])
            board = p->known->boards[0];

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
    char header[512];
    const char *board = "Unknown";
    if (identity->board_override)
        board = identity->board_override;
    else if (identity->known && identity->known->boards[0])
        board = identity->known->boards[0];

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

    /* add serial fd to epoll */
    mp->evt.type = EVT_SERIAL;
    mp->evt.index = idx;
    mp->evt.fd = mp->serial.fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &mp->evt;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                  mp->serial.fd, &ev) < 0) {
        fprintf(stderr, "monitor: epoll_ctl add %s: %s\n",
                identity->dev_path, strerror(errno));
        log_close(&mp->log);
        serial_close(&mp->serial);
        return -1;
    }

    /* if proxy mode, also add PTY master to epoll */
    if (mp->serial.pty_master >= 0) {
        mp->evt_pty.type = EVT_PTY;
        mp->evt_pty.index = idx;
        mp->evt_pty.fd = mp->serial.pty_master;

        ev.events = EPOLLIN;
        ev.data.ptr = &mp->evt_pty;
        if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                      mp->serial.pty_master, &ev) < 0) {
            fprintf(stderr, "monitor: epoll_ctl add pty %s: %s\n",
                    identity->dev_path, strerror(errno));
            /* non-fatal: serial monitoring still works */
        }

        /* create PTY symlink */
        pty_create_symlink(identity->label, mp->serial.pty_path);
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

    /* remove PTY master from epoll */
    if (mp->serial.pty_master >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL,
                  mp->serial.pty_master, NULL);
        pty_remove_symlink(mp->identity.label);
    }

    /* remove serial fd from epoll */
    if (mp->serial.fd >= 0)
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, mp->serial.fd, NULL);

    log_marker(&mp->log, "PORT DISCONNECTED");
    log_close(&mp->log);
    serial_close(&mp->serial);

    printf("  Removed: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    /* shift remaining ports down */
    for (int i = idx; i < state->port_count - 1; i++) {
        state->ports[i] = state->ports[i + 1];
        state->ports[i].evt.index = i;
        state->ports[i].evt_pty.index = i;
        /* re-register with epoll using updated index */
        if (state->ports[i].serial.fd >= 0 && !state->ports[i].yielded) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.ptr = &state->ports[i].evt;
            epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD,
                      state->ports[i].serial.fd, &ev);
        }
        if (state->ports[i].serial.pty_master >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.ptr = &state->ports[i].evt_pty;
            epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD,
                      state->ports[i].serial.pty_master, &ev);
        }
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

    /* remove PTY master from epoll (keep PTY alive for reconnect) */
    if (mp->serial.pty_master >= 0)
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL,
                  mp->serial.pty_master, NULL);

    /* remove serial fd from epoll and close it */
    if (mp->serial.fd >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, mp->serial.fd, NULL);
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

    /* re-add serial fd to epoll */
    mp->evt.fd = mp->serial.fd;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &mp->evt;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                  mp->serial.fd, &ev) < 0) {
        close(mp->serial.fd);
        mp->serial.fd = -1;
        snprintf(resp, resp_sz, "ERROR epoll add failed for %s\n",
                 mp->identity.dev_path);
        return;
    }

    /* re-add PTY master to epoll if proxying */
    if (mp->serial.pty_master >= 0) {
        ev.events = EPOLLIN;
        ev.data.ptr = &mp->evt_pty;
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                  mp->serial.pty_master, &ev);
    }

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
                        cfsetispeed(&tty, spd);
                        cfsetospeed(&tty, spd);
                        tty.c_cflag = (tty.c_cflag & ~CBAUD) | spd;
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
    struct signalfd_siginfo si;
    ssize_t n = read(state->signal_fd, &si, sizeof(si));
    if (n != sizeof(si))
        return;

    switch (si.ssi_signo) {
    case SIGTERM:
    case SIGINT:
        printf("\nReceived SIG%s, shutting down...\n",
               si.ssi_signo == SIGTERM ? "TERM" : "INT");
        state->running = 0;
        break;

    case SIGHUP:
        printf("Received SIGHUP, rescanning ports...\n");

        /* rescan and add any new ports */
        tty_port_t ports[MAX_PORTS];
        int nports = scan_all_ports(ports, MAX_PORTS);

        board_id_t bids[MAX_BOARD_IDS];
        int nbids = load_board_config(bids, MAX_BOARD_IDS);
        if (nbids > 0)
            apply_board_config(ports, nports, bids, nbids);

        for (int i = 0; i < nports; i++) {
            /* add_port checks for duplicates internally */
            add_port(state, &ports[i]);
        }

        write_status_json(state);
        break;
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
 * new log / PTY under fresh->label. */
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

/* Periodic sweep: for each monitored port, cheaply re-read the sysfs USB
 * serial behind its tty node. If the node has gone, drop the port. If the
 * serial changed, the hardware behind the node was swapped -- run a full
 * re-identify (SWD probe) and relabel. Ports yielded for flashing are left
 * untouched. */
static void
reconcile_ports(monitor_state_t *state)
{
    char serial[64];
    tty_port_t fresh;
    board_id_t bids[MAX_BOARD_IDS];
    int i, rc, nbids, changed;

    changed = 0;
    for (i = 0; i < state->port_count; i++) {
        monitored_port_t *mp = &state->ports[i];

        if (mp->yielded)
            continue;

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

        /* hardware behind this node changed -- re-identify and relabel */
        identify_reset_probe_caches();
        if (identify_port(mp->identity.dev_path, &fresh) != 0)
            continue;
        nbids = load_board_config(bids, MAX_BOARD_IDS);
        if (nbids > 0)
            apply_board_config(&fresh, 1, bids, nbids);
        relabel_port(state, i, &fresh);
        i--; /* idx i removed; loop re-examines the shifted element */
        changed = 1;
    }

    if (changed)
        write_status_json(state);
}

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

        /* Wait for the device to settle. Older ST-LINK V2-1 firmware
         * (e.g. NUCLEO-L552ZE-Q with V2J38M27) can take longer than
         * 200ms before STM32_Programmer_CLI sees the new probe on the
         * USB bus, so give it a generous window. */
        usleep(800000);

        /* Drop any cached st-info / STM32_Programmer_CLI results. A
         * recent hot-plug for a different device may have populated
         * the cache without this one in it; we want a fresh probe. */
        identify_reset_probe_caches();

        tty_port_t port;
        int rc = identify_port(hev.devpath, &port);

        /* If we landed on the generic fallback label (ambiguous known
         * device with no resolved board), the probe likely raced the
         * USB enumeration. Wait a bit longer and try once more. */
        if (rc == 0 && port.known &&
            known_device_is_ambiguous(port.known) &&
            !port.board_match) {
            usleep(1500000);
            identify_reset_probe_caches();
            rc = identify_port(hev.devpath, &port);
        }

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
    state.epoll_fd = -1;
    state.signal_fd = -1;
    state.hotplug_fd = -1;
    state.control_fd = -1;
    state.reconcile_fd = -1;

    int foreground = 0;

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

    /* scan and identify ports */
    tty_port_t ports[MAX_PORTS];
    int nports = scan_all_ports(ports, MAX_PORTS);

    /* load board config */
    board_id_t bids[MAX_BOARD_IDS];
    int nbids = load_board_config(bids, MAX_BOARD_IDS);
    if (nbids > 0)
        apply_board_config(ports, nports, bids, nbids);

    printf("Found %d serial port(s)\n", nports);

    /* create epoll */
    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        fprintf(stderr, "monitor: epoll_create1: %s\n", strerror(errno));
        pidfile_remove();
        return 1;
    }

    /* setup signalfd */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    state.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (state.signal_fd >= 0) {
        state.evt_signal.type = EVT_SIGNAL;
        state.evt_signal.fd = state.signal_fd;
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = &state.evt_signal
        };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.signal_fd, &ev);
    }

    /* setup hot-plug */
    state.hotplug_fd = hotplug_init();
    if (state.hotplug_fd >= 0) {
        state.evt_hotplug.type = EVT_HOTPLUG;
        state.evt_hotplug.fd = state.hotplug_fd;
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = &state.evt_hotplug
        };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.hotplug_fd, &ev);
    }

    /* setup periodic reconcile timer */
    state.reconcile_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (state.reconcile_fd >= 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = RECONCILE_INTERVAL_SEC;
        its.it_interval.tv_sec = RECONCILE_INTERVAL_SEC;
        timerfd_settime(state.reconcile_fd, 0, &its, NULL);

        state.evt_reconcile.type = EVT_RECONCILE;
        state.evt_reconcile.fd = state.reconcile_fd;
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = &state.evt_reconcile
        };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.reconcile_fd, &ev);
    }

    /* setup control socket */
    state.control_fd = control_init(CONTROL_SOCK_PATH);
    if (state.control_fd >= 0) {
        state.evt_control.type = EVT_CONTROL;
        state.evt_control.fd = state.control_fd;
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = &state.evt_control
        };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.control_fd, &ev);
    }

    /* open all serial ports */
    for (int i = 0; i < nports; i++)
        add_port(&state, &ports[i]);

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
    struct epoll_event events[MAX_EPOLL_EVENTS];
    char read_buf[READ_BUF_SIZE];

    while (state.running) {
        /* Use short timeout only when a partial line needs flushing;
         * otherwise block indefinitely to avoid wasting CPU. */
        int timeout_ms = -1;
        for (int i = 0; i < state.port_count; i++) {
            if (state.ports[i].log.linebuf_len > 0) {
                timeout_ms = 200;
                break;
            }
        }
        int nfds = epoll_wait(state.epoll_fd, events,
                              MAX_EPOLL_EVENTS, timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            /* Transient epoll errors must not kill the daemon. Log and
             * retry after a short back-off to avoid a tight error loop. */
            fprintf(stderr, "monitor: epoll_wait: %s (continuing)\n",
                    strerror(errno));
            usleep(100000); /* 100ms */
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            event_ctx_t *ctx = events[i].data.ptr;
            if (!ctx)
                continue;

            switch (ctx->type) {
            case EVT_SIGNAL:
                handle_signal(&state);
                break;

            case EVT_HOTPLUG:
                handle_hotplug(&state);
                break;

            case EVT_RECONCILE: {
                /* drain the timerfd, then re-check device identities */
                uint64_t expirations;
                ssize_t tr = read(state.reconcile_fd, &expirations,
                                  sizeof(expirations));
                (void)tr;
                reconcile_ports(&state);
                /* reconcile may remove/relabel ports, shifting the array
                 * and invalidating the rest of this batch's event pointers
                 * -- stop processing further events this iteration. */
                i = nfds;
                break;
            }

            case EVT_CONTROL: {
                /* accept new control client */
                int cfd = accept4(state.control_fd, NULL, NULL,
                                  SOCK_CLOEXEC);
                if (cfd >= 0)
                    handle_control_cmd(&state, cfd);
                break;
            }

            case EVT_CONTROL_CLIENT:
                /* handled inline at accept time */
                break;

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
                    /* port disconnected or error */
                    fprintf(stderr, "monitor: read %s: %s\n",
                            mp->identity.dev_path,
                            nr == 0 ? "EOF" : strerror(errno));
                    remove_port(&state, idx);
                    write_status_json(&state);
                    /* adjust loop since we shifted ports */
                    i = nfds; /* break out of event loop iteration */
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

        /* flush partial lines older than 200ms */
        flush_stale_lines(&state);
    }

    /* ---- cleanup ---- */
    printf("Shutting down...\n");

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
    control_close(state.control_fd, CONTROL_SOCK_PATH);
    if (state.reconcile_fd >= 0)
        close(state.reconcile_fd);
    if (state.signal_fd >= 0)
        close(state.signal_fd);
    if (state.epoll_fd >= 0)
        close(state.epoll_fd);

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
