/* control.h -- Unix domain socket for daemon control */
#ifndef CONTROL_H
#define CONTROL_H

#define CONTROL_SOCK_PATH LOG_BASE_DIR "/uart-monitor.sock"
#define CONTROL_MAX_MSG   4096

/* Initialize the control socket server.
 * Returns listening fd, or -1 on error. */
int control_init(const char *sock_path);

/* Close the control socket and remove the socket file. */
void control_close(int listen_fd, const char *sock_path);

/* Send a command to the daemon's control socket and print the response.
 * Used by CLI client subcommands. Returns 0 on success. */
int control_send_cmd(const char *sock_path, const char *cmd);

/* CLI subcommands that talk to the running daemon */
int cmd_status(int argc, char *argv[]);
int cmd_yield(int argc, char *argv[]);
int cmd_reclaim(int argc, char *argv[]);
int cmd_tail(int argc, char *argv[]);

#endif /* CONTROL_H */
