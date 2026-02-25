/* control.c -- Unix domain socket for daemon control.
 *
 * Protocol: newline-delimited text commands.
 *   YIELD /dev/ttyUSB0\n  -> OK yielded /dev/ttyUSB0\n
 *   RECLAIM /dev/ttyUSB0\n -> OK reclaimed /dev/ttyUSB0\n
 *   STATUS\n               -> JSON blob\n
 *   QUIT\n                 -> OK shutting down\n
 */
#include "control.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int
control_init(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy_safe(addr.sun_path, sock_path, sizeof(addr.sun_path));

    /* remove stale socket */
    unlink(sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "control: bind %s: %s\n", sock_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        fprintf(stderr, "control: listen: %s\n", strerror(errno));
        close(fd);
        unlink(sock_path);
        return -1;
    }

    return fd;
}

void
control_close(int listen_fd, const char *sock_path)
{
    if (listen_fd >= 0)
        close(listen_fd);
    if (sock_path)
        unlink(sock_path);
}

int
control_send_cmd(const char *sock_path, const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Cannot create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy_safe(addr.sun_path, sock_path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to daemon at %s: %s\n",
                sock_path, strerror(errno));
        fprintf(stderr, "Is uart-monitor running? "
                "Start with: uart-monitor monitor -f\n");
        close(fd);
        return -1;
    }

    /* send command */
    size_t cmdlen = strlen(cmd);
    if (write(fd, cmd, cmdlen) < 0) {
        fprintf(stderr, "write: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* read response */
    char buf[CONTROL_MAX_MSG];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
        if (buf[n - 1] != '\n')
            printf("\n");
    }

    close(fd);
    return (n > 0 && strncmp(buf, "OK", 2) == 0) ? 0 : 1;
}

int
cmd_status(int argc, char *argv[])
{
    (void)argc; (void)argv;
    return control_send_cmd(CONTROL_SOCK_PATH, "STATUS\n");
}

int
cmd_yield(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: uart-monitor yield <device>\n");
        fprintf(stderr, "Example: uart-monitor yield /dev/ttyUSB0\n");
        return 1;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "YIELD %s\n", argv[1]);
    return control_send_cmd(CONTROL_SOCK_PATH, cmd);
}

int
cmd_reclaim(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: uart-monitor reclaim <device>\n");
        fprintf(stderr, "Example: uart-monitor reclaim /dev/ttyUSB0\n");
        return 1;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "RECLAIM %s\n", argv[1]);
    return control_send_cmd(CONTROL_SOCK_PATH, cmd);
}

int
cmd_tail(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: uart-monitor tail <device|label>\n");
        fprintf(stderr, "Example: uart-monitor tail ttyUSB0\n");
        fprintf(stderr, "Example: uart-monitor tail VMK180_UART1\n");
        return 1;
    }

    const char *name = argv[1];

    /* strip /dev/ prefix if present */
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;

    /* try direct path: /tmp/uart-monitor/latest/<name>.log */
    char logpath[512];
    snprintf(logpath, sizeof(logpath),
             "%s/latest/%s.log", LOG_BASE_DIR, name);

    if (access(logpath, R_OK) != 0) {
        /* not found -- try scanning for a label match */
        fprintf(stderr, "Log file not found: %s\n", logpath);
        fprintf(stderr, "Available logs in %s/latest/:\n", LOG_BASE_DIR);

        /* list available log files */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "ls -1 %s/latest/*.log 2>/dev/null", LOG_BASE_DIR);
        int ret = system(cmd);
        (void)ret;
        return 1;
    }

    printf("Tailing %s (Ctrl-C to stop)...\n\n", logpath);
    fflush(stdout);

    char tailcmd[600];
    snprintf(tailcmd, sizeof(tailcmd), "tail -f '%s'", logpath);
    return system(tailcmd);
}
