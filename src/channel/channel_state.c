#include "gateway/channel_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(gw_error *error, gw_status code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static void clear_error(gw_error *error)
{
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
}

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
    set_error(error, GW_ERR_VALIDATION, "event %s is invalid while channel is %s",
              gw_channel_event_string(event),
              gw_channel_state_string(runtime->state));
    return GW_ERR_VALIDATION;
}

static gw_status handle_failure(gw_channel_runtime *runtime,
                                const gw_retry_policy *policy,
                                gw_error *error)
{
    if (runtime->state != GW_CHANNEL_PROBING &&
        runtime->state != GW_CHANNEL_STARTING &&
        runtime->state != GW_CHANNEL_RUNNING) {
        return reject_transition(runtime, GW_CHANNEL_EVENT_FAILURE, error);
    }

    ++runtime->consecutive_failures;
    if (runtime->consecutive_failures > (unsigned int)policy->max_retries) {
        runtime->state = GW_CHANNEL_FAILED;
        runtime->backoff_sec = 0;
    } else {
        runtime->state = GW_CHANNEL_BACKOFF;
        runtime->backoff_sec = calculate_backoff(runtime->consecutive_failures,
                                                 policy->max_backoff_sec);
    }
    clear_error(error);
    return GW_OK;
}

gw_status gw_channel_transition(gw_channel_runtime *runtime,
                                gw_channel_event event,
                                const gw_retry_policy *policy,
                                gw_error *error)
{
    if (runtime == NULL || policy == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "channel runtime and retry policy are required");
        return GW_ERR_ARGUMENT;
    }
    if (policy->max_retries < 0 || policy->max_backoff_sec <= 0) {
        set_error(error, GW_ERR_ARGUMENT, "retry policy is invalid");
        return GW_ERR_ARGUMENT;
    }

    switch (event) {
    case GW_CHANNEL_EVENT_ENABLE:
        if (runtime->state != GW_CHANNEL_DISABLED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STOPPED;
        break;
    case GW_CHANNEL_EVENT_DISABLE:
        runtime->state = GW_CHANNEL_DISABLED;
        runtime->consecutive_failures = 0U;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_START:
        if (runtime->state != GW_CHANNEL_STOPPED &&
            runtime->state != GW_CHANNEL_FAILED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_PROBING;
        runtime->consecutive_failures = 0U;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_PROBE_SUCCEEDED:
        if (runtime->state != GW_CHANNEL_PROBING) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STARTING;
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
        if (runtime->state != GW_CHANNEL_BACKOFF) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_PROBING;
        runtime->backoff_sec = 0;
        ++runtime->total_restarts;
        break;
    case GW_CHANNEL_EVENT_STABLE:
        if (runtime->state != GW_CHANNEL_RUNNING) {
            return reject_transition(runtime, event, error);
        }
        runtime->consecutive_failures = 0U;
        runtime->backoff_sec = 0;
        break;
    case GW_CHANNEL_EVENT_STOP:
        if (runtime->state == GW_CHANNEL_DISABLED) {
            return reject_transition(runtime, event, error);
        }
        runtime->state = GW_CHANNEL_STOPPED;
        runtime->consecutive_failures = 0U;
        runtime->backoff_sec = 0;
        break;
    default:
        set_error(error, GW_ERR_ARGUMENT, "unknown channel event");
        return GW_ERR_ARGUMENT;
    }

    clear_error(error);
    return GW_OK;
}

const char *gw_channel_state_string(gw_channel_state state)
{
    switch (state) {
    case GW_CHANNEL_DISABLED:
        return "DISABLED";
    case GW_CHANNEL_STOPPED:
        return "STOPPED";
    case GW_CHANNEL_PROBING:
        return "PROBING";
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
    case GW_CHANNEL_EVENT_PROBE_SUCCEEDED:
        return "PROBE_SUCCEEDED";
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
