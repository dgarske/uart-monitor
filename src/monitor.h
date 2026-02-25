/* monitor.h -- Main epoll-based monitoring daemon */
#ifndef MONITOR_H
#define MONITOR_H

#include "identify.h"
#include "serial.h"
#include "log.h"

/* Event source types for epoll dispatch */
typedef enum {
    EVT_SIGNAL,
    EVT_SERIAL,
    EVT_HOTPLUG,
    EVT_CONTROL,
    EVT_CONTROL_CLIENT,
} event_type_t;

typedef struct {
    event_type_t type;
    int          index;     /* for EVT_SERIAL: index into ports[] */
    int          fd;        /* for EVT_CONTROL_CLIENT */
} event_ctx_t;

/* State for a single monitored port */
typedef struct {
    tty_port_t   identity;
    serial_port_t serial;
    log_file_t   log;
    event_ctx_t  evt;
    int          yielded;
    size_t       bytes_read;
} monitored_port_t;

/* Overall daemon state */
typedef struct {
    int              epoll_fd;
    int              signal_fd;
    int              hotplug_fd;
    int              control_fd;
    char             session_path[512];
    monitored_port_t ports[MAX_PORTS];
    int              port_count;
    event_ctx_t      evt_signal;
    event_ctx_t      evt_hotplug;
    event_ctx_t      evt_control;
    volatile int     running;
    int              systemd_mode;
    speed_t          baudrate;
    char             only_filter[512];  /* comma-separated device filter */
} monitor_state_t;

/* The monitor subcommand entry point. */
int cmd_monitor(int argc, char *argv[]);

#endif /* MONITOR_H */
