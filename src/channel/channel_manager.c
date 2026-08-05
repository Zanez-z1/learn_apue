/* POSIX-thread channel ownership and a read/write-locked snapshot registry. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gw_channel_entry gw_channel_entry;

struct gw_channel_entry {
    struct gw_channel_manager *manager;
    size_t channel_index;
    pthread_t thread;
    bool thread_started;
    int result;
    gw_supervisor_options options;
    gw_channel_snapshot snapshot;
};

struct gw_channel_manager {
    gw_config config;
    gw_supervisor_options caller_options;
    pthread_rwlock_t snapshot_lock;
    atomic_int stop_signal;
    gw_channel_entry entries[GW_MAX_CHANNELS];
    size_t channel_count;
    bool started;
    bool joined;
    int result;
};

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

static int manager_stop_requested(void *context)
{
    gw_channel_manager *manager = context;
    int requested = atomic_load(&manager->stop_signal);

    if (requested != 0) {
        return requested;
    }
    if (manager->caller_options.stop_check != NULL) {
        requested = manager->caller_options.stop_check(
            manager->caller_options.stop_context);
        if (requested != 0) {
            return requested;
        }
    }
    if (manager->caller_options.stop_signal != NULL) {
        return (int)*manager->caller_options.stop_signal;
    }
    return 0;
}

static void manager_observe(const gw_channel_snapshot *snapshot, void *context)
{
    gw_channel_entry *entry = context;
    gw_channel_manager *manager = entry->manager;

    pthread_rwlock_wrlock(&manager->snapshot_lock);
    entry->snapshot = *snapshot;
    pthread_rwlock_unlock(&manager->snapshot_lock);
    if (manager->caller_options.observer != NULL) {
        manager->caller_options.observer(
            snapshot, manager->caller_options.observer_context);
    }
}

static void *run_channel(void *context)
{
    gw_channel_entry *entry = context;
    gw_channel_manager *manager = entry->manager;

    entry->result = gw_supervisor_run(
        &manager->config, &manager->config.channels[entry->channel_index],
        &entry->options);
    return NULL;
}

gw_status gw_channel_manager_create(gw_channel_manager **manager_output,
                                    const gw_config *config,
                                    const gw_supervisor_options *options,
                                    gw_error *error)
{
    gw_channel_manager *manager;
    size_t index;
    int result;

    if (manager_output == NULL || config == NULL || options == NULL) {
        set_error(error, GW_ERR_ARGUMENT,
                  "manager output, configuration, and options are required");
        return GW_ERR_ARGUMENT;
    }
    *manager_output = NULL;
    if (options->ffprobe_binary == NULL || options->ffprobe_binary[0] == '\0' ||
        options->ffmpeg_binary == NULL || options->ffmpeg_binary[0] == '\0') {
        set_error(error, GW_ERR_VALIDATION,
                  "manager executable names must not be empty");
        return GW_ERR_VALIDATION;
    }
    if (gw_config_validate(config, error) != GW_OK) {
        return error != NULL ? error->code : GW_ERR_VALIDATION;
    }

    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        set_error(error, GW_ERR_NO_MEMORY, "cannot allocate channel manager");
        return GW_ERR_NO_MEMORY;
    }
    manager->config = *config;
    manager->caller_options = *options;
    atomic_init(&manager->stop_signal, 0);
    result = pthread_rwlock_init(&manager->snapshot_lock, NULL);
    if (result != 0) {
        free(manager);
        set_error(error, GW_ERR_IO, "cannot initialize snapshot lock: %s",
                  strerror(result));
        return GW_ERR_IO;
    }

    for (index = 0U; index < config->channel_count; ++index) {
        gw_channel_entry *entry;

        if (!config->channels[index].enabled) {
            continue;
        }
        entry = &manager->entries[manager->channel_count++];
        entry->manager = manager;
        entry->channel_index = index;
        entry->options = *options;
        entry->options.stop_signal = NULL;
        entry->options.stop_check = manager_stop_requested;
        entry->options.stop_context = manager;
        entry->options.observer = manager_observe;
        entry->options.observer_context = entry;
        gw_channel_snapshot_init(&entry->snapshot, &config->channels[index]);
    }
    if (manager->channel_count == 0U) {
        pthread_rwlock_destroy(&manager->snapshot_lock);
        free(manager);
        set_error(error, GW_ERR_VALIDATION,
                  "at least one enabled channel is required");
        return GW_ERR_VALIDATION;
    }
    manager->result = 0;
    *manager_output = manager;
    clear_error(error);
    return GW_OK;
}

gw_status gw_channel_manager_start(gw_channel_manager *manager, gw_error *error)
{
    size_t index;
    int result;

    if (manager == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "channel manager is required");
        return GW_ERR_ARGUMENT;
    }
    if (manager->started) {
        set_error(error, GW_ERR_VALIDATION, "channel manager is already started");
        return GW_ERR_VALIDATION;
    }
    manager->started = true;
    for (index = 0U; index < manager->channel_count; ++index) {
        result = pthread_create(&manager->entries[index].thread, NULL,
                                run_channel, &manager->entries[index]);
        if (result != 0) {
            size_t started_index;

            atomic_store(&manager->stop_signal, SIGTERM);
            for (started_index = 0U; started_index < index; ++started_index) {
                pthread_join(manager->entries[started_index].thread, NULL);
            }
            manager->joined = true;
            manager->result = 1;
            set_error(error, GW_ERR_IO, "cannot start channel thread: %s",
                      strerror(result));
            return GW_ERR_IO;
        }
        manager->entries[index].thread_started = true;
    }
    clear_error(error);
    return GW_OK;
}

gw_status gw_channel_manager_get_snapshot(gw_channel_manager *manager,
                                          const char *channel_id,
                                          gw_channel_snapshot *snapshot,
                                          gw_error *error)
{
    size_t index;

    if (manager == NULL || channel_id == NULL || snapshot == NULL) {
        set_error(error, GW_ERR_ARGUMENT,
                  "manager, channel id, and snapshot output are required");
        return GW_ERR_ARGUMENT;
    }
    for (index = 0U; index < manager->channel_count; ++index) {
        size_t channel_index = manager->entries[index].channel_index;

        if (strcmp(manager->config.channels[channel_index].id, channel_id) == 0) {
            pthread_rwlock_rdlock(&manager->snapshot_lock);
            *snapshot = manager->entries[index].snapshot;
            pthread_rwlock_unlock(&manager->snapshot_lock);
            clear_error(error);
            return GW_OK;
        }
    }
    set_error(error, GW_ERR_VALIDATION, "channel '%s' is not managed", channel_id);
    return GW_ERR_VALIDATION;
}

void gw_channel_manager_request_stop(gw_channel_manager *manager,
                                     int signal_number)
{
    if (manager != NULL) {
        atomic_store(&manager->stop_signal,
                     signal_number != 0 ? signal_number : SIGTERM);
    }
}

int gw_channel_manager_wait(gw_channel_manager *manager)
{
    size_t index;

    if (manager == NULL || !manager->started) {
        return 2;
    }
    if (manager->joined) {
        return manager->result;
    }
    manager->result = 0;
    for (index = 0U; index < manager->channel_count; ++index) {
        if (manager->entries[index].thread_started) {
            pthread_join(manager->entries[index].thread, NULL);
            if (manager->entries[index].result != 0) {
                manager->result = 1;
            }
        }
    }
    manager->joined = true;
    return manager->result;
}

void gw_channel_manager_destroy(gw_channel_manager *manager)
{
    if (manager == NULL) {
        return;
    }
    if (manager->started && !manager->joined) {
        gw_channel_manager_request_stop(manager, SIGTERM);
        gw_channel_manager_wait(manager);
    }
    pthread_rwlock_destroy(&manager->snapshot_lock);
    free(manager);
}
