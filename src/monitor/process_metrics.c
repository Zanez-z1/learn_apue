#define _POSIX_C_SOURCE 200809L

#include "gateway/process_metrics.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GW_PROC_TEXT_CAPACITY 65536U

static int read_text_file(const char *path, char *buffer, size_t capacity)
{
    size_t used = 0U;
    int fd;

    if (path == NULL || buffer == NULL || capacity < 2U) {
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    while (used + 1U < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used - 1U);

        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR) {
            int saved_errno = errno;

            close(fd);
            errno = saved_errno;
            return -1;
        }
    }

    if (used + 1U == capacity) {
        char extra;
        ssize_t count;

        do {
            count = read(fd, &extra, 1U);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            close(fd);
            errno = EOVERFLOW;
            return -1;
        }
    }

    if (close(fd) != 0) {
        return -1;
    }
    buffer[used] = '\0';
    return 0;
}

int gw_process_metrics_parse_stat(const char *text, uint64_t *cpu_ticks)
{
    const char *cursor;
    const char *close_paren;
    unsigned long long user_ticks = 0ULL;
    unsigned long long system_ticks = 0ULL;
    int field;

    if (text == NULL || cpu_ticks == NULL) {
        return -1;
    }

    close_paren = strrchr(text, ')');
    if (close_paren == NULL || close_paren[1] != ' ' || close_paren[2] == '\0' ||
        close_paren[3] != ' ') {
        return -1;
    }
    cursor = close_paren + 4;

    for (field = 4; field <= 15; field++) {
        char *end = NULL;

        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0' || *cursor == '\n') {
            return -1;
        }
        errno = 0;
        if (field < 14) {
            strtoll(cursor, &end, 10);
        } else {
            unsigned long long value;

            if (*cursor == '-') {
                return -1;
            }
            value = strtoull(cursor, &end, 10);

            if (field == 14) {
                user_ticks = value;
            } else {
                system_ticks = value;
            }
        }
        if (errno != 0 || end == cursor ||
            (*end != ' ' && *end != '\n' && *end != '\0')) {
            return -1;
        }
        cursor = end;
    }

    if (UINT64_MAX - (uint64_t)user_ticks < (uint64_t)system_ticks) {
        return -1;
    }
    *cpu_ticks = (uint64_t)user_ticks + (uint64_t)system_ticks;
    return 0;
}

int gw_process_metrics_parse_status(const char *text, long *rss_kib)
{
    const char *line = text;

    if (text == NULL || rss_kib == NULL) {
        return -1;
    }

    while (*line != '\0') {
        const char *next = strchr(line, '\n');

        if (strncmp(line, "VmRSS:", 6U) == 0) {
            const char *cursor = line + 6;
            char *end = NULL;
            long value;

            while (*cursor == ' ' || *cursor == '\t') {
                cursor++;
            }
            errno = 0;
            value = strtol(cursor, &end, 10);
            if (errno != 0 || end == cursor || value < 0L) {
                return -1;
            }
            while (*end == ' ' || *end == '\t') {
                end++;
            }
            if (strncmp(end, "kB", 2U) != 0 ||
                (end[2] != '\0' && end[2] != '\n' && end[2] != '\r')) {
                return -1;
            }
            *rss_kib = value;
            return 0;
        }
        if (next == NULL) {
            break;
        }
        line = next + 1;
    }
    return -1;
}

static int count_descriptors(const char *path, size_t *fd_count)
{
    size_t count = 0U;
    DIR *directory;
    struct dirent *entry;

    directory = opendir(path);
    if (directory == NULL) {
        return -1;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
        errno = 0;
    }
    if (errno != 0) {
        int saved_errno = errno;

        closedir(directory);
        errno = saved_errno;
        return -1;
    }
    if (closedir(directory) != 0) {
        return -1;
    }
    *fd_count = count;
    return 0;
}

int gw_process_metrics_read(pid_t pid, gw_process_metrics *metrics)
{
    char path[64];
    char text[GW_PROC_TEXT_CAPACITY];
    gw_process_metrics candidate;
    int written;

    if (pid <= 0 || metrics == NULL) {
        errno = EINVAL;
        return -1;
    }

    written = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(path) ||
        read_text_file(path, text, sizeof(text)) != 0 ||
        gw_process_metrics_parse_stat(text, &candidate.cpu_ticks) != 0) {
        return -1;
    }

    written = snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(path) ||
        read_text_file(path, text, sizeof(text)) != 0 ||
        gw_process_metrics_parse_status(text, &candidate.rss_kib) != 0) {
        return -1;
    }

    written = snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(path) ||
        count_descriptors(path, &candidate.fd_count) != 0) {
        return -1;
    }

    *metrics = candidate;
    return 0;
}

void gw_process_metrics_tracker_init(gw_process_metrics_tracker *tracker)
{
    if (tracker == NULL) {
        return;
    }
    memset(tracker, 0, sizeof(*tracker));
    tracker->pid = (pid_t)-1;
}

void gw_process_metrics_tracker_update(gw_process_metrics_tracker *tracker,
                                       pid_t pid,
                                       const gw_process_metrics *metrics,
                                       int64_t sample_time_ns,
                                       long clock_ticks_per_second,
                                       gw_process_metrics_snapshot *snapshot)
{
    bool same_baseline;

    if (tracker == NULL || snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->pid = pid;
    if (pid <= 0 || metrics == NULL || sample_time_ns <= 0 ||
        clock_ticks_per_second <= 0) {
        gw_process_metrics_tracker_init(tracker);
        return;
    }

    snapshot->available = true;
    snapshot->rss_kib = metrics->rss_kib;
    snapshot->fd_count = metrics->fd_count;
    same_baseline = tracker->baseline_valid && tracker->pid == pid &&
                    sample_time_ns > tracker->previous_time_ns &&
                    metrics->cpu_ticks >= tracker->previous_cpu_ticks;
    if (same_baseline) {
        uint64_t delta_ticks = metrics->cpu_ticks - tracker->previous_cpu_ticks;
        int64_t delta_time_ns = sample_time_ns - tracker->previous_time_ns;

        snapshot->cpu_percent =
            (double)delta_ticks * 100.0 * 1000000000.0 /
            ((double)clock_ticks_per_second * (double)delta_time_ns);
        snapshot->cpu_available = true;
    }

    tracker->pid = pid;
    tracker->baseline_valid = true;
    tracker->previous_cpu_ticks = metrics->cpu_ticks;
    tracker->previous_time_ns = sample_time_ns;
}
