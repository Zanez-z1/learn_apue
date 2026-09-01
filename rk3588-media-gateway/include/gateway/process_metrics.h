#ifndef GATEWAY_PROCESS_METRICS_H
#define GATEWAY_PROCESS_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint64_t cpu_ticks;
    long rss_kib;
    size_t fd_count;
} gw_process_metrics;

typedef struct {
    pid_t pid;
    bool baseline_valid;
    uint64_t previous_cpu_ticks;
    int64_t previous_time_ns;
} gw_process_metrics_tracker;

typedef struct {
    pid_t pid;
    bool available;
    bool cpu_available;
    double cpu_percent;
    long rss_kib;
    size_t fd_count;
} gw_process_metrics_snapshot;

int gw_process_metrics_parse_stat(const char *text, uint64_t *cpu_ticks);
int gw_process_metrics_parse_status(const char *text, long *rss_kib);
int gw_process_metrics_read(pid_t pid, gw_process_metrics *metrics);

void gw_process_metrics_tracker_init(gw_process_metrics_tracker *tracker);

void gw_process_metrics_tracker_update(gw_process_metrics_tracker *tracker,
                                       pid_t pid,
                                       const gw_process_metrics *metrics,
                                       int64_t sample_time_ns,
                                       long clock_ticks_per_second,
                                       gw_process_metrics_snapshot *snapshot);

#endif
