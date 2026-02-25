/* log.c -- Session-based log file management.
 *
 * Creates /tmp/uart-monitor/session-<timestamp>/ directories with
 * per-port log files. Each line gets a [timestamp] prefix.
 * A "latest" symlink always points to the current session.
 */
#include "log.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int
log_create_session(char *session_path, size_t sz)
{
    /* ensure base dir exists */
    if (mkdirp(LOG_BASE_DIR) < 0) {
        fprintf(stderr, "log: cannot create %s: %s\n",
                LOG_BASE_DIR, strerror(errno));
        return -1;
    }

    /* create session directory */
    char ts[32];
    timestamp_filename(ts, sizeof(ts));

    char session_name[64];
    snprintf(session_name, sizeof(session_name), "session-%s", ts);
    snprintf(session_path, sz, "%s/%s", LOG_BASE_DIR, session_name);

    if (mkdir(session_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "log: cannot create %s: %s\n",
                session_path, strerror(errno));
        return -1;
    }

    /* update "latest" symlink */
    char linkpath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/latest", LOG_BASE_DIR);
    symlink_update(session_name, linkpath);

    return 0;
}

int
log_open(log_file_t *lf, const char *session_path,
         const char *tty_name, const char *header)
{
    memset(lf, 0, sizeof(*lf));

    snprintf(lf->filepath, sizeof(lf->filepath),
             "%s/%s.log", session_path, tty_name);

    lf->fp = fopen(lf->filepath, "a");
    if (!lf->fp) {
        fprintf(stderr, "log: cannot open %s: %s\n",
                lf->filepath, strerror(errno));
        return -1;
    }

    /* line-buffered for tail -f friendliness */
    setvbuf(lf->fp, NULL, _IOLBF, 0);

    lf->session_start = time(NULL);
    clock_gettime(CLOCK_MONOTONIC, &lf->last_flush);

    /* write header */
    if (header && header[0]) {
        fprintf(lf->fp, "=== UART Monitor Session ===\n");
        fprintf(lf->fp, "%s", header);
        char ts[32];
        timestamp_now(ts, sizeof(ts));
        fprintf(lf->fp, "Started: %s\n", ts);
        fprintf(lf->fp, "===\n\n");
        fflush(lf->fp);
    }

    return 0;
}

/* Write a timestamp prefix for the current partial line. */
static void
write_timestamp(log_file_t *lf)
{
    char ts[32];
    timestamp_now(ts, sizeof(ts));
    fprintf(lf->fp, "[%s] ", ts);
}

int
log_write(log_file_t *lf, const char *data, size_t len)
{
    if (!lf->fp || len == 0)
        return 0;

    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        /* skip bare \r (handle \r\n and \r as just newlines) */
        if (c == '\r') {
            /* peek ahead: if next is \n, skip the \r */
            if (i + 1 < len && data[i + 1] == '\n')
                continue;
            /* bare \r -> treat as newline */
            c = '\n';
        }

        if (lf->linebuf_len == 0 && c != '\n') {
            /* starting a new line: write timestamp */
            write_timestamp(lf);
        }

        if (c == '\n') {
            /* flush the line */
            if (lf->linebuf_len > 0) {
                fwrite(lf->linebuf, 1, (size_t)lf->linebuf_len, lf->fp);
                lf->bytes_written += (size_t)lf->linebuf_len;
                lf->linebuf_len = 0;
            }
            fputc('\n', lf->fp);
            lf->bytes_written++;
        } else {
            /* buffer the character */
            if (lf->linebuf_len < LOG_LINE_BUF_SIZE - 1) {
                lf->linebuf[lf->linebuf_len++] = c;
            }
            /* if buffer full, force flush */
            if (lf->linebuf_len >= LOG_LINE_BUF_SIZE - 1) {
                fwrite(lf->linebuf, 1, (size_t)lf->linebuf_len, lf->fp);
                lf->bytes_written += (size_t)lf->linebuf_len;
                fputc('\n', lf->fp);
                lf->bytes_written++;
                lf->linebuf_len = 0;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &lf->last_flush);
    return 0;
}

void
log_flush(log_file_t *lf)
{
    if (!lf->fp)
        return;
    if (lf->linebuf_len > 0) {
        /* flush partial line with timestamp already written */
        fwrite(lf->linebuf, 1, (size_t)lf->linebuf_len, lf->fp);
        lf->bytes_written += (size_t)lf->linebuf_len;
        fputc('\n', lf->fp);
        lf->bytes_written++;
        lf->linebuf_len = 0;
    }
    fflush(lf->fp);
}

void
log_marker(log_file_t *lf, const char *msg)
{
    if (!lf->fp)
        return;

    /* flush any pending partial line first */
    if (lf->linebuf_len > 0) {
        fwrite(lf->linebuf, 1, (size_t)lf->linebuf_len, lf->fp);
        fputc('\n', lf->fp);
        lf->linebuf_len = 0;
    }

    char ts[32];
    timestamp_now(ts, sizeof(ts));
    fprintf(lf->fp, "\n--- %s [%s] ---\n\n", msg, ts);
    fflush(lf->fp);
}

void
log_close(log_file_t *lf)
{
    if (lf->fp) {
        log_flush(lf);
        fclose(lf->fp);
        lf->fp = NULL;
    }
}

/* Compare function for sorting session directory names. */
static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

int
log_prune_sessions(int keep)
{
    DIR *dir = opendir(LOG_BASE_DIR);
    if (!dir)
        return -1;

    char *sessions[256];
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < 256) {
        if (strncmp(ent->d_name, "session-", 8) == 0) {
            sessions[count] = strdup(ent->d_name);
            if (sessions[count])
                count++;
        }
    }
    closedir(dir);

    if (count <= keep) {
        for (int i = 0; i < count; i++)
            free(sessions[i]);
        return 0;
    }

    /* sort alphabetically (timestamp-based names sort chronologically) */
    qsort(sessions, (size_t)count, sizeof(char *), cmp_str);

    /* remove the oldest ones */
    int to_remove = count - keep;
    for (int i = 0; i < to_remove; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", LOG_BASE_DIR, sessions[i]);

        /* remove all files in the session directory */
        DIR *sdir = opendir(path);
        if (sdir) {
            struct dirent *sent;
            while ((sent = readdir(sdir)) != NULL) {
                if (sent->d_name[0] == '.')
                    continue;
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", path, sent->d_name);
                unlink(fpath);
            }
            closedir(sdir);
        }
        rmdir(path);
    }

    for (int i = 0; i < count; i++)
        free(sessions[i]);

    return 0;
}
