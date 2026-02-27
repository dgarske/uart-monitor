/* serial.h -- Serial port access (read-only or PTY proxy) */
#ifndef SERIAL_H
#define SERIAL_H

#include <termios.h>

#define PTY_DIR  LOG_BASE_DIR "/pty"

typedef struct {
    int     fd;              /* real serial port fd */
    int     pty_master;      /* PTY master fd (-1 if not proxying) */
    char    pty_path[256];   /* PTY slave path (e.g. /dev/pts/5) */
    char    dev_path[256];
    speed_t baudrate;
} serial_port_t;

/* Open a serial port read-only (O_RDONLY | O_NOCTTY | O_NONBLOCK).
 * Configures termios for the given baud, 8N1, raw mode.
 * Returns 0 on success, -1 on error. */
int serial_open(serial_port_t *sp, const char *dev_path, speed_t baud);

/* Open a serial port in proxy mode (O_RDWR) and create a PTY pair.
 * The PTY slave acts as a virtual serial port that other tools can use.
 * Data from the real port is forwarded to the PTY master (and logged).
 * Data written to the PTY slave is forwarded to the real port.
 * Returns 0 on success, -1 on error. */
int serial_open_proxy(serial_port_t *sp, const char *dev_path, speed_t baud);

/* Close a serial port (and PTY master if proxying).
 * Safe to call on already-closed port. */
void serial_close(serial_port_t *sp);

/* Map a numeric baud rate (e.g. 115200) to a speed_t constant. */
speed_t baud_to_speed(int baud);

#endif /* SERIAL_H */
