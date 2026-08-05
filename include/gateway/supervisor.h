#ifndef GATEWAY_SUPERVISOR_H
#define GATEWAY_SUPERVISOR_H

/* Single-channel probe, worker lifecycle, timeout, and retry coordination. */

#include "gateway/config.h"

#include <signal.h>

typedef struct {
    /* All pointers are borrowed and must remain valid until gw_supervisor_run ends. */
    const char *ffprobe_binary;
    const char *ffmpeg_binary;
    /* Optional signal value published by the CLI's async signal handler. */
    const volatile sig_atomic_t *stop_signal;
} gw_supervisor_options;

/* Initialize executable names and disable external stop requests. */
void gw_supervisor_options_init(gw_supervisor_options *options);

/* Return 0 for clean stop, 1 for terminal channel failure, or 2 for bad arguments. */
int gw_supervisor_run(const gw_config *config,
                      const gw_channel_config *channel,
                      const gw_supervisor_options *options);

#endif
