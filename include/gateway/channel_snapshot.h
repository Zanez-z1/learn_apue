#ifndef GATEWAY_CHANNEL_SNAPSHOT_H
#define GATEWAY_CHANNEL_SNAPSHOT_H

/* Read-only channel status model for observers and future control APIs. */

#include "gateway/channel_state.h"
#include "gateway/process_metrics.h"
#include "gateway/probe.h"
#include "gateway/progress_parser.h"

#include <stdbool.h>
#include <sys/types.h>

#define GW_EVENT_CAP 64U

typedef enum {
    GW_CHANNEL_PROCESS_NONE = 0,
    GW_CHANNEL_PROCESS_PROBE,
    GW_CHANNEL_PROCESS_WORKER
} gw_channel_process_kind;

typedef struct {
    char channel_id[GW_ID_CAP];
    gw_channel_state state;
    char last_event[GW_EVENT_CAP];
    gw_channel_process_kind process_kind;
    pid_t process_pid;
    bool has_exit_code;
    int last_exit_code;
    unsigned int consecutive_failures;
    uint64_t total_restarts;
    /* Incremented by the manager when this channel's configuration is replaced. */
    uint64_t configuration_generation;
    int backoff_sec;
    bool has_probe;
    gw_probe_info probe;
    bool has_progress;
    gw_worker_progress progress;
    gw_process_metrics_snapshot worker_metrics;
} gw_channel_snapshot;

void gw_channel_snapshot_init(gw_channel_snapshot *snapshot,
                              const gw_channel_config *channel);

void gw_channel_snapshot_update_runtime(gw_channel_snapshot *snapshot,
                                        const gw_channel_runtime *runtime,
                                        const char *event);

void gw_channel_snapshot_set_process(gw_channel_snapshot *snapshot,
                                     gw_channel_process_kind kind, pid_t pid,
                                     const char *event);

void gw_channel_snapshot_clear_process(gw_channel_snapshot *snapshot,
                                       int exit_code, const char *event);

/* Clear process-derived input/progress/resource data before a retry wait. */
void gw_channel_snapshot_clear_live_data(gw_channel_snapshot *snapshot);

void gw_channel_snapshot_set_probe(gw_channel_snapshot *snapshot,
                                   const gw_probe_info *probe);

void gw_channel_snapshot_set_progress(gw_channel_snapshot *snapshot,
                                      const gw_worker_progress *progress,
                                      const char *event);

const char *gw_channel_process_kind_string(gw_channel_process_kind kind);

#endif
