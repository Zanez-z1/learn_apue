#ifndef GATEWAY_RECORDING_STATUS_H
#define GATEWAY_RECORDING_STATUS_H

/* On-demand filesystem capacity snapshot for the MediaMTX recording directory. */

#include "gateway/config.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GW_RECORDING_DISABLED = 0,
    GW_RECORDING_OK,
    GW_RECORDING_LOW_SPACE,
    GW_RECORDING_UNAVAILABLE
} gw_recording_state;

typedef struct {
    gw_recording_state state;
    bool enabled;
    bool filesystem_available;
    uint64_t total_bytes;
    uint64_t available_bytes;
    uint64_t min_free_bytes;
} gw_recording_snapshot;

const char *gw_recording_state_string(gw_recording_state state);

/* Missing/inaccessible storage is represented as UNAVAILABLE, not a call failure. */
gw_status gw_recording_snapshot_read(const gw_recording_config *config,
                                     gw_recording_snapshot *snapshot,
                                     gw_error *error);

#endif
