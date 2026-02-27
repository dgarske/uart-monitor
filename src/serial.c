/* serial.c -- Serial port access (read-only or PTY proxy).
 *
 * Read-only mode: O_RDONLY | O_NOCTTY | O_NONBLOCK.
 *   NEVER writes to the port. Does NOT set TIOCEXCL.
 *
 * Proxy mode: O_RDWR | O_NOCTTY | O_NONBLOCK + openpty().
 *   Creates a PTY pair. Sets TIOCEXCL on the real port to prevent
 *   other processes from opening it. All access goes through the PTY slave.
 */
#include "serial.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Configure termios for raw 8N1 at the given baud rate. */
static int
configure_raw(int fd, speed_t baud, const char *label)
{
    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);

    tty.c_cflag = baud | CS8 | CREAD | CLOCAL;
    tty.c_iflag = 0;       /* no input processing */
    tty.c_oflag = 0;       /* no output processing */
    tty.c_lflag = 0;       /* raw mode */

    tty.c_cc[VMIN]  = 0;   /* non-blocking */
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        fprintf(stderr, "serial: tcsetattr %s: %s\n",
                label, strerror(errno));
        return -1;
    }
    return 0;
}

int
serial_open(serial_port_t *sp, const char *dev_path, speed_t baud)
{
    sp->fd = -1;
    sp->pty_master = -1;
    sp->pty_path[0] = '\0';
    strlcpy_safe(sp->dev_path, dev_path, sizeof(sp->dev_path));
    sp->baudrate = baud;

    int fd = open(dev_path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "serial: cannot open %s: %s\n",
                dev_path, strerror(errno));
        return -1;
    }

    if (configure_raw(fd, baud, dev_path) < 0) {
        close(fd);
        return -1;
    }

    sp->fd = fd;
    return 0;
}

int
serial_open_proxy(serial_port_t *sp, const char *dev_path, speed_t baud)
{
    sp->fd = -1;
    sp->pty_master = -1;
    sp->pty_path[0] = '\0';
    strlcpy_safe(sp->dev_path, dev_path, sizeof(sp->dev_path));
    sp->baudrate = baud;

    /* open real port O_RDWR for bidirectional proxy */
    int fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "serial: cannot open %s (proxy): %s\n",
                dev_path, strerror(errno));
        return -1;
    }

    if (configure_raw(fd, baud, dev_path) < 0) {
        close(fd);
        return -1;
    }

    /* set exclusive access on real port -- all access goes through PTY */
    if (ioctl(fd, TIOCEXCL) < 0) {
        fprintf(stderr, "serial: TIOCEXCL %s: %s (continuing)\n",
                dev_path, strerror(errno));
        /* non-fatal: continue without exclusive lock */
    }

    /* create PTY pair */
    int master, slave;
    char slave_name[256];
    if (openpty(&master, &slave, slave_name, NULL, NULL) < 0) {
        fprintf(stderr, "serial: openpty for %s: %s\n",
                dev_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* configure PTY slave for raw mode matching the serial config */
    if (configure_raw(slave, baud, "pty-slave") < 0) {
        /* non-fatal: PTY slave may not fully support all termios */
    }

    close(slave); /* users open the slave path themselves */

    /* set PTY master to non-blocking for epoll */
    int flags = fcntl(master, F_GETFL);
    if (flags >= 0)
        fcntl(master, F_SETFL, flags | O_NONBLOCK);

    sp->fd = fd;
    sp->pty_master = master;
    strlcpy_safe(sp->pty_path, slave_name, sizeof(sp->pty_path));

    return 0;
}

void
serial_close(serial_port_t *sp)
{
    if (sp->pty_master >= 0) {
        close(sp->pty_master);
        sp->pty_master = -1;
    }
    if (sp->fd >= 0) {
        close(sp->fd);
        sp->fd = -1;
    }
    sp->pty_path[0] = '\0';
}

speed_t
baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default:      return B115200;
    }
}
