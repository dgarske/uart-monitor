/* util.h -- Common utility functions */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Read a sysfs attribute file, strip trailing newline.
 * Returns bytes read (excluding trailing NUL), or -1 on error. */
int sysfs_read_attr(const char *path, char *buf, size_t bufsz);

/* Read a hex value from a sysfs attribute file (e.g. "10c4" -> 0x10c4).
 * Returns 0 on success, -1 on error. */
int sysfs_read_hex(const char *path, uint16_t *val);

/* Safe string copy with guaranteed NUL termination. */
void strlcpy_safe(char *dst, const char *src, size_t sz);

/* Get timestamp string "YYYY-MM-DD HH:MM:SS.mmm" into buf.
 * buf must be at least 24 bytes. */
void timestamp_now(char *buf, size_t bufsz);

/* Get timestamp string "YYYYMMDD-HHMMSS" for filenames.
 * buf must be at least 16 bytes. */
void timestamp_filename(char *buf, size_t bufsz);

/* Ensure a directory exists, creating it (and parents) if needed.
 * Returns 0 on success, -1 on error. */
int mkdirp(const char *path);

/* Atomically update a symlink (create tmp, rename). Returns 0 on success. */
int symlink_update(const char *target, const char *linkpath);

#endif /* UTIL_H */
