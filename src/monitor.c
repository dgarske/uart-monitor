/* monitor.c -- Main epoll-based monitoring daemon.
 *
 * Single-threaded event loop multiplexing:
 *   - Serial port reads (one per monitored device)
 *   - Netlink/inotify hot-plug events
 *   - Unix domain socket control commands
 *   - signalfd for SIGTERM/SIGINT/SIGHUP
 *
 * NEVER writes to serial ports.
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EPOLL_EVENTS  (MAX_PORTS + 16)
#define READ_BUF_SIZE     4096
#define PID_FILE          LOG_BASE_DIR "/uart-monitor.pid"
#define STATUS_FILE       LOG_BASE_DIR "/status.json"

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
        fprintf(fp, "      \"bytes_logged\": %zu\n", mp->log.bytes_written);
        fprintf(fp, "    }%s\n",
                (i < state->port_count - 1) ? "," : "");
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

    /* open serial port */
    if (serial_open(&mp->serial, identity->dev_path,
                    state->baudrate) < 0) {
        return -1;
    }

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
             115200);

    /* open log file */
    if (log_open(&mp->log, state->session_path,
                 identity->tty_name, header) < 0) {
        serial_close(&mp->serial);
        return -1;
    }

    /* add to epoll */
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

    state->port_count++;
    printf("  Monitoring: %s [%s] -> %s\n",
           identity->dev_path, identity->label, mp->log.filepath);

    return idx;
}

static void
remove_port(monitor_state_t *state, int idx)
{
    if (idx < 0 || idx >= state->port_count)
        return;

    monitored_port_t *mp = &state->ports[idx];

    /* remove from epoll */
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
        /* re-register with epoll using updated index */
        if (state->ports[i].serial.fd >= 0 && !state->ports[i].yielded) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.ptr = &state->ports[i].evt;
            epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD,
                      state->ports[i].serial.fd, &ev);
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

    /* remove from epoll and close serial fd */
    if (mp->serial.fd >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, mp->serial.fd, NULL);
        serial_close(&mp->serial);
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
    if (serial_open(&mp->serial, mp->identity.dev_path,
                    state->baudrate) < 0) {
        snprintf(resp, resp_sz, "ERROR cannot reopen %s\n",
                 mp->identity.dev_path);
        return;
    }

    /* re-add to epoll */
    mp->evt.fd = mp->serial.fd;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &mp->evt;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD,
                  mp->serial.fd, &ev) < 0) {
        serial_close(&mp->serial);
        snprintf(resp, resp_sz, "ERROR epoll add failed for %s\n",
                 mp->identity.dev_path);
        return;
    }

    mp->yielded = 0;
    log_marker(&mp->log, "PORT RECLAIMED (monitoring resumed)");

    printf("  Reclaimed: %s [%s]\n",
           mp->identity.dev_path, mp->identity.label);

    write_status_json(state);

    snprintf(resp, resp_sz, "OK reclaimed %s\n", mp->identity.dev_path);
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
    } else if (strcmp(buf, "QUIT") == 0) {
        snprintf(resp, sizeof(resp), "OK shutting down\n");
        state->running = 0;
    } else {
        snprintf(resp, sizeof(resp),
                 "ERROR unknown command: %s\n", buf);
    }

    /* send response (best effort) and close */
    ssize_t written = write(client_fd, resp, strlen(resp));
    (void)written;
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

        /* wait for device to settle */
        usleep(200000);

        tty_port_t port;
        if (identify_port(hev.devpath, &port) == 0) {
            /* apply board config */
            board_id_t bids[MAX_BOARD_IDS];
            int nbids = load_board_config(bids, MAX_BOARD_IDS);
            if (nbids > 0)
                apply_board_config(&port, 1, bids, nbids);

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

    int foreground = 0;

    /* parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 ||
            strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--systemd") == 0) {
            state.systemd_mode = 1;
            foreground = 1;
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

    /* prune old sessions */
    log_prune_sessions(LOG_MAX_SESSIONS);

    printf("uart-monitor starting...\n");
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

    /* ---- main event loop ---- */
    struct epoll_event events[MAX_EPOLL_EVENTS];
    char read_buf[READ_BUF_SIZE];

    while (state.running) {
        int nfds = epoll_wait(state.epoll_fd, events,
                              MAX_EPOLL_EVENTS, 500);

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "monitor: epoll_wait: %s\n", strerror(errno));
            break;
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
            }
        }

        /* flush partial lines older than 200ms */
        flush_stale_lines(&state);
    }

    /* ---- cleanup ---- */
    printf("Shutting down...\n");

    for (int i = state.port_count - 1; i >= 0; i--) {
        monitored_port_t *mp = &state.ports[i];
        log_marker(&mp->log, "MONITOR STOPPED");
        log_close(&mp->log);
        serial_close(&mp->serial);
    }

    if (state.hotplug_fd >= 0)
        hotplug_close(state.hotplug_fd);
    control_close(state.control_fd, CONTROL_SOCK_PATH);
    if (state.signal_fd >= 0)
        close(state.signal_fd);
    if (state.epoll_fd >= 0)
        close(state.epoll_fd);

    pidfile_remove();
    unlink(STATUS_FILE);

    if (state.systemd_mode)
        sd_notify_send("STOPPING=1");

    printf("Stopped.\n");
    return 0;
}
