#ifndef GATEWAY_PROCESS_METRICS_H
#define GATEWAY_PROCESS_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint64_t cpu_ticks;
    long rss_kib;
    size_t fd_count;
} gw_process_metrics;

int gw_process_metrics_parse_stat(const char *text, uint64_t *cpu_ticks);
int gw_process_metrics_parse_status(const char *text, long *rss_kib);
int gw_process_metrics_read(pid_t pid, gw_process_metrics *metrics);

#endif
