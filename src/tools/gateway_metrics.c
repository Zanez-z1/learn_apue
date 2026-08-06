#define _POSIX_C_SOURCE 200809L

#include "gateway/process_metrics.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GW_METRICS_MAX_TARGETS 16U
#define GW_METRICS_LABEL_CAPACITY 32U

typedef struct {
    char label[GW_METRICS_LABEL_CAPACITY];
    pid_t pid;
    bool previous_valid;
    uint64_t previous_ticks;
} metrics_target;

typedef struct {
    metrics_target targets[GW_METRICS_MAX_TARGETS];
    size_t target_count;
    long duration_sec;
    long interval_ms;
} metrics_options;

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: gateway-metrics --target LABEL=PID [--target LABEL=PID ...] "
            "[--duration-sec N] [--interval-ms N]\n"
            "       PID may be 'self' for a tool self-test. CSV is written to stdout.\n");
}

static int parse_long(const char *text, long minimum, long maximum, long *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static bool label_is_safe(const char *label, size_t length)
{
    size_t index;

    if (length == 0U || length >= GW_METRICS_LABEL_CAPACITY) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)label[index];

        if (!isalnum(character) && character != '_' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

static int add_target(metrics_options *options, const char *argument)
{
    const char *separator = strchr(argument, '=');
    metrics_target *target;
    size_t label_length;
    size_t index;
    long parsed_pid;

    if (separator == NULL || strchr(separator + 1, '=') != NULL ||
        options->target_count >= GW_METRICS_MAX_TARGETS) {
        return -1;
    }
    label_length = (size_t)(separator - argument);
    if (!label_is_safe(argument, label_length)) {
        return -1;
    }

    for (index = 0U; index < options->target_count; index++) {
        if (strlen(options->targets[index].label) == label_length &&
            strncmp(options->targets[index].label, argument, label_length) == 0) {
            return -1;
        }
    }

    target = &options->targets[options->target_count];
    memcpy(target->label, argument, label_length);
    target->label[label_length] = '\0';
    if (strcmp(separator + 1, "self") == 0) {
        target->pid = getpid();
    } else {
        if (parse_long(separator + 1, 1L, 2147483647L, &parsed_pid) != 0) {
            return -1;
        }
        target->pid = (pid_t)parsed_pid;
    }
    target->previous_valid = false;
    target->previous_ticks = 0U;
    options->target_count++;
    return 0;
}

static int parse_options(int argc, char **argv, metrics_options *options)
{
    int index;

    memset(options, 0, sizeof(*options));
    options->duration_sec = 5L;
    options->interval_ms = 1000L;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--help") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(argv[index], "--target") == 0 && index + 1 < argc) {
            index++;
            if (add_target(options, argv[index]) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--duration-sec") == 0 && index + 1 < argc) {
            index++;
            if (parse_long(argv[index], 1L, 86400L, &options->duration_sec) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[index], "--interval-ms") == 0 && index + 1 < argc) {
            index++;
            if (parse_long(argv[index], 100L, 60000L, &options->interval_ms) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }

    return options->target_count == 0U ? -1 : 0;
}

static int64_t elapsed_milliseconds(const struct timespec *start, const struct timespec *now)
{
    int64_t seconds = (int64_t)now->tv_sec - (int64_t)start->tv_sec;
    int64_t nanoseconds = (int64_t)now->tv_nsec - (int64_t)start->tv_nsec;

    return seconds * 1000LL + nanoseconds / 1000000LL;
}

static void add_milliseconds(struct timespec *time, long milliseconds)
{
    time->tv_sec += milliseconds / 1000L;
    time->tv_nsec += (milliseconds % 1000L) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        time->tv_sec++;
        time->tv_nsec -= 1000000000L;
    }
}

static int sleep_until(const struct timespec *deadline)
{
    int result;

    do {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (result == EINTR);
    if (result != 0) {
        errno = result;
    }
    return result;
}

static bool sample_target(metrics_target *target, int64_t elapsed_ms,
                          int64_t sample_delta_ms, long ticks_per_second)
{
    gw_process_metrics metrics;
    double cpu_percent = 0.0;

    if (gw_process_metrics_read(target->pid, &metrics) != 0) {
        printf("%" PRId64 ",%s,%ld,unavailable,,,\n",
               elapsed_ms, target->label, (long)target->pid);
        target->previous_valid = false;
        return false;
    }

    if (target->previous_valid && metrics.cpu_ticks >= target->previous_ticks &&
        sample_delta_ms > 0) {
        uint64_t delta_ticks = metrics.cpu_ticks - target->previous_ticks;

        cpu_percent = (double)delta_ticks * 100000.0 /
                      ((double)ticks_per_second * (double)sample_delta_ms);
        printf("%" PRId64 ",%s,%ld,ok,%.2f,%ld,%zu\n",
               elapsed_ms, target->label, (long)target->pid, cpu_percent,
               metrics.rss_kib, metrics.fd_count);
    } else {
        printf("%" PRId64 ",%s,%ld,ok,,%ld,%zu\n",
               elapsed_ms, target->label, (long)target->pid,
               metrics.rss_kib, metrics.fd_count);
    }
    target->previous_ticks = metrics.cpu_ticks;
    target->previous_valid = true;
    return true;
}

static int collect_metrics(metrics_options *options)
{
    struct timespec start;
    struct timespec now;
    struct timespec deadline;
    int64_t previous_elapsed = 0LL;
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    bool all_available = true;

    if (ticks_per_second <= 0L || clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return -1;
    }
    deadline = start;
    printf("elapsed_ms,target,pid,status,cpu_percent,rss_kib,fd_count\n");

    for (;;) {
        int64_t elapsed;
        int64_t delta;
        size_t index;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return -1;
        }
        elapsed = elapsed_milliseconds(&start, &now);
        delta = elapsed - previous_elapsed;
        for (index = 0U; index < options->target_count; index++) {
            if (!sample_target(&options->targets[index], elapsed, delta,
                               ticks_per_second)) {
                all_available = false;
            }
        }
        if (fflush(stdout) != 0) {
            return -1;
        }
        if (elapsed >= (int64_t)options->duration_sec * 1000LL) {
            break;
        }
        previous_elapsed = elapsed;
        add_milliseconds(&deadline, options->interval_ms);
        if (sleep_until(&deadline) != 0) {
            return -1;
        }
    }
    return all_available ? 0 : 2;
}

int main(int argc, char **argv)
{
    metrics_options options;
    int parse_result = parse_options(argc, argv, &options);
    int result;

    if (parse_result > 0) {
        return EXIT_SUCCESS;
    }
    if (parse_result < 0) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }

    result = collect_metrics(&options);
    if (result < 0) {
        fprintf(stderr, "gateway-metrics: sampling failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (result > 0) {
        fprintf(stderr, "gateway-metrics: one or more targets became unavailable\n");
        return result;
    }
    return EXIT_SUCCESS;
}
