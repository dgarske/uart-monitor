/* serial.c -- Read-only serial port access.
 *
 * Opens ports with O_RDONLY | O_NOCTTY | O_NONBLOCK.
 * NEVER writes to the port. Does NOT set TIOCEXCL.
 */
#include "serial.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
serial_open(serial_port_t *sp, const char *dev_path, speed_t baud)
{
    sp->fd = -1;
    strlcpy_safe(sp->dev_path, dev_path, sizeof(sp->dev_path));
    sp->baudrate = baud;

    int fd = open(dev_path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "serial: cannot open %s: %s\n",
                dev_path, strerror(errno));
        return -1;
    }

    /* configure termios for raw 8N1 at the requested baud rate */
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
                dev_path, strerror(errno));
        close(fd);
        return -1;
    }

    sp->fd = fd;
    return 0;
}

void
serial_close(serial_port_t *sp)
{
    if (sp->fd >= 0) {
        close(sp->fd);
        sp->fd = -1;
    }
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
