/* Channel status snapshot construction and bounded field updates. */
#include "gateway/channel_snapshot.h"

#include <stdio.h>
#include <string.h>

static void set_event(gw_channel_snapshot *snapshot, const char *event)
{
    snprintf(snapshot->last_event, sizeof(snapshot->last_event), "%s",
             event != NULL ? event : "");
}

void gw_channel_snapshot_init(gw_channel_snapshot *snapshot,
                              const gw_channel_config *channel)
{
    if (snapshot == NULL || channel == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snprintf(snapshot->channel_id, sizeof(snapshot->channel_id), "%s",
             channel->id);
    snapshot->state = channel->enabled ? GW_CHANNEL_STOPPED : GW_CHANNEL_DISABLED;
    snapshot->process_kind = GW_CHANNEL_PROCESS_NONE;
    snapshot->process_pid = -1;
    snapshot->worker_metrics.pid = (pid_t)-1;
    snapshot->last_exit_code = -1;
    set_event(snapshot, "initialized");
}

void gw_channel_snapshot_update_runtime(gw_channel_snapshot *snapshot,
                                        const gw_channel_runtime *runtime,
                                        const char *event)
{
    if (snapshot == NULL || runtime == NULL) {
        return;
    }
    snapshot->state = runtime->state;
    snapshot->consecutive_failures = runtime->consecutive_failures;
    snapshot->total_restarts = runtime->total_restarts;
    snapshot->backoff_sec = runtime->backoff_sec;
    set_event(snapshot, event);
}

void gw_channel_snapshot_set_process(gw_channel_snapshot *snapshot,
                                     gw_channel_process_kind kind, pid_t pid,
                                     const char *event)
{
    if (snapshot == NULL) {
        return;
    }
    snapshot->process_kind = kind;
    snapshot->process_pid = pid;
    snapshot->has_exit_code = false;
    snapshot->last_exit_code = -1;
    set_event(snapshot, event);
}

void gw_channel_snapshot_clear_process(gw_channel_snapshot *snapshot,
                                       int exit_code, const char *event)
{
    if (snapshot == NULL) {
        return;
    }
    snapshot->process_kind = GW_CHANNEL_PROCESS_NONE;
    snapshot->process_pid = -1;
    snapshot->has_exit_code = true;
    snapshot->last_exit_code = exit_code;
    set_event(snapshot, event);
}

void gw_channel_snapshot_set_probe(gw_channel_snapshot *snapshot,
                                   const gw_probe_info *probe)
{
    if (snapshot == NULL || probe == NULL) {
        return;
    }
    snapshot->probe = *probe;
    snapshot->has_probe = true;
}

void gw_channel_snapshot_set_progress(gw_channel_snapshot *snapshot,
                                      const gw_worker_progress *progress,
                                      const char *event)
{
    if (snapshot == NULL || progress == NULL) {
        return;
    }
    snapshot->progress = *progress;
    snapshot->has_progress = true;
    set_event(snapshot, event);
}

const char *gw_channel_process_kind_string(gw_channel_process_kind kind)
{
    switch (kind) {
    case GW_CHANNEL_PROCESS_NONE:
        return "NONE";
    case GW_CHANNEL_PROCESS_PROBE:
        return "PROBE";
    case GW_CHANNEL_PROCESS_WORKER:
        return "WORKER";
    }
    return "UNKNOWN";
}
