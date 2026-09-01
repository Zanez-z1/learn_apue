#ifndef GATEWAY_CHANNEL_STATE_H
#define GATEWAY_CHANNEL_STATE_H

/* Deterministic lifecycle state machine for a supervised media channel. */

#include "gateway/config.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GW_CHANNEL_DISABLED = 0,
    GW_CHANNEL_STOPPED,
    GW_CHANNEL_PROBING,
    GW_CHANNEL_STARTING,
    GW_CHANNEL_RUNNING,
    GW_CHANNEL_BACKOFF,
    GW_CHANNEL_FAILED
} gw_channel_state;

typedef enum {
    GW_CHANNEL_EVENT_ENABLE = 0,
    GW_CHANNEL_EVENT_DISABLE,
    GW_CHANNEL_EVENT_START,
    GW_CHANNEL_EVENT_PROBE_SUCCEEDED,
    GW_CHANNEL_EVENT_PROGRESS,
    GW_CHANNEL_EVENT_FAILURE,
    GW_CHANNEL_EVENT_BACKOFF_ELAPSED,
    GW_CHANNEL_EVENT_STABLE,
    GW_CHANNEL_EVENT_STOP
} gw_channel_event;

typedef struct {
    gw_channel_state state;
    /* Failures since the last stable period or manual restart. */
    unsigned int consecutive_failures;
    /* Automatic restarts performed after backoff elapsed. */
    uint64_t total_restarts;
    /* Delay selected for the current BACKOFF or recoverable FAILED state. */
    int backoff_sec;
} gw_channel_runtime;

/* Initialize runtime state from the channel's configured enabled flag. */
void gw_channel_runtime_init(gw_channel_runtime *runtime, bool enabled);

/* Apply one event atomically, rejecting transitions not valid in the current state. */
gw_status gw_channel_transition(gw_channel_runtime *runtime,
                                gw_channel_event event,
                                const gw_retry_policy *policy,
                                gw_error *error);

/* Stable names used by diagnostics and tests. */
const char *gw_channel_state_string(gw_channel_state state);
const char *gw_channel_event_string(gw_channel_event event);

#endif
