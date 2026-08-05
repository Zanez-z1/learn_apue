#ifndef GATEWAY_CHANNEL_MANAGER_H
#define GATEWAY_CHANNEL_MANAGER_H

/* Concurrent ownership and status registry for all enabled channels. */

#include "gateway/channel_snapshot.h"
#include "gateway/config.h"
#include "gateway/supervisor.h"

typedef struct gw_channel_manager gw_channel_manager;

typedef struct {
    size_t unchanged;
    size_t added;
    size_t removed;
    size_t restarted;
} gw_channel_reload_summary;

/* If options has an observer, it may be called concurrently by channel threads. */
gw_status gw_channel_manager_create(gw_channel_manager **manager,
                                    const gw_config *config,
                                    const gw_supervisor_options *options,
                                    gw_error *error);

gw_status gw_channel_manager_start(gw_channel_manager *manager, gw_error *error);

/* Validate first, then add/remove/restart only channels affected by candidate. */
gw_status gw_channel_manager_reload(gw_channel_manager *manager,
                                    const gw_config *candidate,
                                    gw_channel_reload_summary *summary,
                                    gw_error *error);

/* Safe while channel threads are running; copies the locked registry entry. */
gw_status gw_channel_manager_get_snapshot(gw_channel_manager *manager,
                                          const char *channel_id,
                                          gw_channel_snapshot *snapshot,
                                          gw_error *error);

/* Thread-safe and idempotent; signal_number should normally be SIGINT/SIGTERM. */
void gw_channel_manager_request_stop(gw_channel_manager *manager,
                                     int signal_number);

/* True after all currently managed channel threads have returned. */
bool gw_channel_manager_is_finished(const gw_channel_manager *manager);

/* Join every channel; return 0 only when every supervisor stopped cleanly. */
int gw_channel_manager_wait(gw_channel_manager *manager);

/* Requests stop and joins first if the caller has not already done so. */
void gw_channel_manager_destroy(gw_channel_manager *manager);

#endif
