/* identify.h -- USB serial port scanning and identification */
#ifndef IDENTIFY_H
#define IDENTIFY_H

#include "devices.h"
#include <stdint.h>

#define MAX_PORTS       64
#define MAX_GROUPS      32
#define MAX_PORTS_PER_GROUP 8

typedef struct {
    char dev_path[256];
    char tty_name[32];
    uint16_t vid;
    uint16_t pid;
    int interface_num;
    char serial[64];
    char manufacturer[128];
    char product[128];
    char usb_path[128];
    const known_device_t *known;
    const char *function_name;
    const char *board_override;
    char label[64];      /* filesystem-safe name, e.g. "VMK180_UART1" */
} tty_port_t;

typedef struct {
    char group_key[256];
    tty_port_t *ports[MAX_PORTS_PER_GROUP];
    int port_count;
} device_group_t;

/* Board config entry loaded from ~/.boards */
typedef struct {
    char serial[64];
    char board_name[128];
} board_id_t;

#define MAX_BOARD_IDS 32

/* Scan all /dev/ttyUSB*, ttyACM*, ttyUART* ports. Returns count. */
int scan_all_ports(tty_port_t *ports, int max_ports);

/* Identify a single port by reading sysfs. Returns 0 on success. */
int identify_port(const char *dev_path, tty_port_t *port);

/* Group ports by parent USB device. Returns number of groups. */
int group_ports(tty_port_t *ports, int nports,
                device_group_t *groups, int max_groups);

/* Generate a filesystem-safe label for a port's log directory. */
void get_device_label(tty_port_t *port);

/* Print formatted table of ports grouped by device. */
void print_port_table(device_group_t *groups, int ngroups, int verbose);

/* Load board identifications from ~/.boards. Returns count. */
int load_board_config(board_id_t *ids, int max_ids);

/* Apply board overrides from config to scanned ports. */
void apply_board_config(tty_port_t *ports, int nports,
                        board_id_t *ids, int nids);

/* The identify subcommand. */
int cmd_identify(int argc, char *argv[]);

#endif /* IDENTIFY_H */
