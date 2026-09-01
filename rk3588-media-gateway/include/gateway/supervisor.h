#ifndef GATEWAY_SUPERVISOR_H
#define GATEWAY_SUPERVISOR_H

/* Manages a single worker thread: its startup, running, shutdown,
along with timeout checks and automatic retries on failure. */

#include "gateway/config.h"
#include "gateway/channel_snapshot.h"

#include <signal.h>

typedef struct {
    /* All pointers are borrowed and must remain valid until gw_supervisor_run ends. */
    const char *ffmpeg_binary;
    /* Optional caller-owned stop signal, primarily used by direct module tests. */
    const volatile sig_atomic_t *stop_signal;
    /* Optional manager-owned stop source; nonzero takes priority over stop_signal. */
    int (*stop_check)(void *context);
    void *stop_context;
    /* Test/one-shot mode: treat an unsolicited zero exit as terminal success. */
    bool stop_on_clean_exit;
    /* One-shot mode: terminate after retry exhaustion instead of polling FAILED. */
    bool exit_on_retry_exhaustion;
    /* Called synchronously; copy the snapshot if it must be retained. */
    void (*observer)(const gw_channel_snapshot *snapshot, void *context);
    void *observer_context;
} gw_supervisor_options;

/* Initialize executable names and disable external stop requests. */
void gw_supervisor_options_init(gw_supervisor_options *options);

/* Return 0 for clean stop, 1 for terminal channel failure, or 2 for bad arguments. */
int gw_supervisor_run(const gw_config *config,
                      const gw_channel_config *channel,
                      const gw_supervisor_options *options);

#endif
