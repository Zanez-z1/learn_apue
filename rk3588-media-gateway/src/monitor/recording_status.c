/* Read recording capacity with statvfs without scanning or modifying segments. */

#include "gateway/recording_status.h"
#include "gateway/error.h"

#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>

static bool multiply_u64(uint64_t first, uint64_t second, uint64_t *result)
{
    if (first != 0U && second > UINT64_MAX / first) {
        return false;
    }
    *result = first * second;
    return true;
}

const char *gw_recording_state_string(gw_recording_state state)
{
    switch (state) {
    case GW_RECORDING_DISABLED:
        return "disabled";
    case GW_RECORDING_OK:
        return "ok";
    case GW_RECORDING_LOW_SPACE:
        return "low_space";
    case GW_RECORDING_UNAVAILABLE:
        return "unavailable";
    }
    return "unknown";
}

gw_status gw_recording_snapshot_read(const gw_recording_config *config,
                                     gw_recording_snapshot *snapshot,
                                     gw_error *error)
{
    struct statvfs filesystem;
    uint64_t fragment_size;

    if (config == NULL || snapshot == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT,
                  "recording configuration and snapshot are required");
        return GW_ERR_ARGUMENT;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = config->enabled;
    snapshot->state = config->enabled ? GW_RECORDING_UNAVAILABLE
                                      : GW_RECORDING_DISABLED;
    if (!multiply_u64((uint64_t)config->min_free_mb, 1024U * 1024U,
                      &snapshot->min_free_bytes)) {
        gw_error_set(error, GW_ERR_OVERFLOW,
                  "recording free-space threshold overflows");
        return GW_ERR_OVERFLOW;
    }
    if (!config->enabled) {
        gw_error_clear(error);
        return GW_OK;
    }
    if (statvfs(config->directory, &filesystem) != 0) {
        gw_error_clear(error);
        return GW_OK;
    }
    fragment_size = filesystem.f_frsize != 0U
                        ? (uint64_t)filesystem.f_frsize
                        : (uint64_t)filesystem.f_bsize;
    if (!multiply_u64(fragment_size, (uint64_t)filesystem.f_blocks,
                      &snapshot->total_bytes) ||
        !multiply_u64(fragment_size, (uint64_t)filesystem.f_bavail,
                      &snapshot->available_bytes)) {
        gw_error_set(error, GW_ERR_OVERFLOW,
                  "recording filesystem capacity overflows");
        return GW_ERR_OVERFLOW;
    }
    snapshot->filesystem_available = true;
    snapshot->state = snapshot->available_bytes < snapshot->min_free_bytes
                          ? GW_RECORDING_LOW_SPACE
                          : GW_RECORDING_OK;
    gw_error_clear(error);
    return GW_OK;
}
