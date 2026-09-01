/* Pure channel state transitions and bounded exponential retry scheduling. */
#include "gateway/channel_state.h"
#include "gateway/error.h"

#include <limits.h>
#include <string.h>

void gw_channel_runtime_init(gw_channel_runtime *runtime, bool enabled)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->state = enabled ? GW_CHANNEL_STOPPED : GW_CHANNEL_DISABLED;
}

static int calculate_backoff(unsigned int failures, int maximum)
{
    unsigned int step;
    int delay = 1;

    /* Produce 1, 2, 4, ... seconds without overflowing past the configured cap. */
    for (step = 1U; step < failures && delay < maximum; ++step) {
        if (delay > maximum / 2) {
            delay = maximum;
        } else {
            delay *= 2;
        }
    }
    return delay > maximum ? maximum : delay;
}

static gw_status reject_transition(const gw_channel_runtime *runtime,
                                   gw_channel_event event, gw_error *error)
{
    gw_error_set(error, GW_ERR_VALIDATION, "event %s is invalid while channel is %s",
              gw_channel_event_string(event),
              gw_channel_state_string(runtime->state));
    return GW_ERR_VALIDATION;
}

static gw_status handle_failure(gw_channel_runtime *runtime,
                                const gw_retry_policy *policy,
                                gw_error *error)
{
    if (runtime->state != GW_CHANNEL_STARTING &&
        runtime->state != GW_CHANNEL_RUNNING) {
        return reject_transition(runtime, GW_CHANNEL_EVENT_FAILURE, error);
    }

    if (runtime->consecutive_failures < UINT_MAX) {
        ++runtime->consecutive_failures;
    }
    if (runtime->consecutive_failures > (unsigned int)policy->max_retries) {
        runtime->state = GW_CHANNEL_FAILED;
        runtime->backoff_sec = policy->max_backoff_sec;
    } else {
        runtime->state = GW_CHANNEL_BACKOFF;
        runtime->backoff_sec = calculate_backoff(runtime->consecutive_failures,
                                                 policy->max_backoff_sec);
    }
    gw_error_clear(error);
    return GW_OK;
}

gw_status gw_channel_transition(gw_channel_runtime *runtime,
                                gw_channel_event event,
                                const gw_retry_policy *policy,
                                gw_error *error)
{
    if (runtime == NULL || policy == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT, "channel runtime and retry policy are required");
        return GW_ERR_ARGUMENT;
    }
    if (policy->max_retries < 0 || policy->max_backoff_sec <= 0) {
        gw_error_set(error, GW_ERR_ARGUMENT, "retry policy is invalid");
        return GW_ERR_ARGUMENT;
    }

    /* State mutation is centralized here so supervisors cannot skip invariants. */
    switch (event) {
    case GW_CHANNEL_EVENT_ENABLE:
        if (runtime->state != GW_CHANNEL_DISABLED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STOPPED;
        break;
    case GW_CHANNEL_EVENT_DISABLE:
        runtime->state = GW_CHANNEL_DISABLED;
        runtime->consecutive_failures = 0;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_START:
        if (runtime->state != GW_CHANNEL_STOPPED &&
            runtime->state != GW_CHANNEL_FAILED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STARTING;
        runtime->consecutive_failures = 0;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_PROGRESS:
        if (runtime->state != GW_CHANNEL_STARTING &&
            runtime->state != GW_CHANNEL_RUNNING) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_RUNNING;
        break;
    case GW_CHANNEL_EVENT_FAILURE:
        return handle_failure(runtime, policy, error);
    case GW_CHANNEL_EVENT_BACKOFF_ELAPSED:
        if (runtime->state != GW_CHANNEL_BACKOFF &&
            runtime->state != GW_CHANNEL_FAILED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STARTING;
        runtime->backoff_sec = 0;
        runtime->total_restarts++;
        break;
    case GW_CHANNEL_EVENT_STABLE:
        if (runtime->state != GW_CHANNEL_RUNNING) {
            return reject_transition(runtime, event, error);
        }
        runtime->consecutive_failures = 0;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_STOP:
        if (runtime->state == GW_CHANNEL_DISABLED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STOPPED;
        runtime->consecutive_failures = 0;
        runtime->backoff_sec = 0;
        break;
    default:
        gw_error_set(error, GW_ERR_ARGUMENT, "unknown channel event");
        return GW_ERR_ARGUMENT;
    }

    gw_error_clear(error);
    return GW_OK;
}

const char *gw_channel_state_string(gw_channel_state state)
{
    switch (state) {
    case GW_CHANNEL_DISABLED:
        return "DISABLED";
    case GW_CHANNEL_STOPPED:
        return "STOPPED";
    case GW_CHANNEL_STARTING:
        return "STARTING";
    case GW_CHANNEL_RUNNING:
        return "RUNNING";
    case GW_CHANNEL_BACKOFF:
        return "BACKOFF";
    case GW_CHANNEL_FAILED:
        return "FAILED";
    }
    return "UNKNOWN";
}

const char *gw_channel_event_string(gw_channel_event event)
{
    switch (event) {
    case GW_CHANNEL_EVENT_ENABLE:
        return "ENABLE";
    case GW_CHANNEL_EVENT_DISABLE:
        return "DISABLE";
    case GW_CHANNEL_EVENT_START:
        return "START";
    case GW_CHANNEL_EVENT_PROGRESS:
        return "PROGRESS";
    case GW_CHANNEL_EVENT_FAILURE:
        return "FAILURE";
    case GW_CHANNEL_EVENT_BACKOFF_ELAPSED:
        return "BACKOFF_ELAPSED";
    case GW_CHANNEL_EVENT_STABLE:
        return "STABLE";
    case GW_CHANNEL_EVENT_STOP:
        return "STOP";
    }
    return "UNKNOWN";
}
