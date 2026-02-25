/* serial.h -- Read-only serial port access */
#ifndef SERIAL_H
#define SERIAL_H

#include <termios.h>

typedef struct {
    int  fd;
    char dev_path[256];
    speed_t baudrate;
} serial_port_t;

/* Open a serial port read-only (O_RDONLY | O_NOCTTY | O_NONBLOCK).
 * Configures termios for the given baud, 8N1, raw mode.
 * Returns 0 on success, -1 on error. */
int serial_open(serial_port_t *sp, const char *dev_path, speed_t baud);

/* Close a serial port. Safe to call on already-closed port. */
void serial_close(serial_port_t *sp);

/* Map a numeric baud rate (e.g. 115200) to a speed_t constant. */
speed_t baud_to_speed(int baud);

#endif /* SERIAL_H */
