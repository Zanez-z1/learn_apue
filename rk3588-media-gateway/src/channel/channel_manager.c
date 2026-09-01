/* Reloadable POSIX-thread channel ownership and locked snapshot registry. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/error.h"
#include "gateway/process_metrics.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


typedef struct {
    struct gw_channel_manager *manager;
    bool occupied;
    pthread_t thread;
    bool thread_started;
    atomic_bool thread_running;
    int result;
    atomic_int stop_signal;
    uint64_t generation;
    gw_supervisor_options options;
    gw_config run_config;
    gw_channel_snapshot snapshot;
    gw_process_metrics_tracker metrics_tracker;
    gw_process_metrics_snapshot worker_metrics;
    uint64_t metrics_generation;
}gw_channel_entry; 

struct gw_channel_manager {
    gw_config config;
    gw_supervisor_options caller_options;
    pthread_mutex_t lifecycle_lock;
    pthread_rwlock_t snapshot_lock;
    atomic_int stop_signal;
    atomic_size_t active_threads;
    atomic_bool metrics_stop_requested;
    pthread_t metrics_thread;
    bool metrics_thread_started;
    gw_channel_entry entries[GW_MAX_CHANNELS];
    bool started;
    int result;
};

typedef struct {
    bool occupied;
    uint64_t generation;
    gw_channel_process_kind process_kind;
    pid_t process_pid;
} gw_metrics_target;

static int entry_stop_requested(void *context)
{
    gw_channel_entry *entry = context;
    gw_channel_manager *manager = entry->manager;
    int requested = atomic_load(&entry->stop_signal);

    if (requested != 0) {
        return requested;
    }
    requested = atomic_load(&manager->stop_signal);
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
    gw_channel_snapshot published = *snapshot;

    published.configuration_generation = entry->generation;
    pthread_rwlock_wrlock(&manager->snapshot_lock);
    entry->snapshot = published;
    pthread_rwlock_unlock(&manager->snapshot_lock);
    if (manager->caller_options.observer != NULL) {
        manager->caller_options.observer(
            &published, manager->caller_options.observer_context);
    }
}

static int64_t monotonic_time_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * INT64_C(1000000000) + (int64_t)now.tv_nsec;
}

static void wait_for_next_metrics_sample(gw_channel_manager *manager)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
    unsigned int slice;

    for (slice = 0U; slice < 10U; ++slice) {
        struct timespec remaining = delay;

        while (!atomic_load(&manager->metrics_stop_requested) &&
               nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        }
        if (atomic_load(&manager->metrics_stop_requested)) {
            return;
        }
    }
}

static void *sample_process_metrics(void *context)
{
    gw_channel_manager *manager = context;
    long clock_ticks_per_second = sysconf(_SC_CLK_TCK);

    while (!atomic_load(&manager->metrics_stop_requested)) {
        gw_metrics_target targets[GW_MAX_CHANNELS];
        size_t index;

        pthread_rwlock_rdlock(&manager->snapshot_lock);
        for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
            targets[index].occupied = manager->entries[index].occupied;
            targets[index].generation = manager->entries[index].generation;
            targets[index].process_kind =
                manager->entries[index].snapshot.process_kind;
            targets[index].process_pid =
                manager->entries[index].snapshot.process_pid;
        }
        pthread_rwlock_unlock(&manager->snapshot_lock);

        for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
            gw_channel_entry *entry = &manager->entries[index];
            gw_process_metrics raw_metrics;
            const gw_process_metrics *raw = NULL;
            gw_process_metrics_snapshot sampled;
            pid_t pid = (pid_t)-1;
            int64_t sample_time_ns = monotonic_time_ns();

            if (entry->metrics_generation != targets[index].generation) {
                gw_process_metrics_tracker_init(&entry->metrics_tracker);
                entry->metrics_generation = targets[index].generation;
            }
            if (targets[index].occupied &&
                targets[index].process_kind == GW_CHANNEL_PROCESS_WORKER &&
                targets[index].process_pid > 0) {
                pid = targets[index].process_pid;
                if (gw_process_metrics_read(pid, &raw_metrics) == 0) {
                    raw = &raw_metrics;
                }
            }
            gw_process_metrics_tracker_update(
                &entry->metrics_tracker, pid, raw, sample_time_ns,
                clock_ticks_per_second, &sampled);

            pthread_rwlock_wrlock(&manager->snapshot_lock);
            if (entry->occupied == targets[index].occupied &&
                entry->generation == targets[index].generation &&
                entry->snapshot.process_kind == targets[index].process_kind &&
                entry->snapshot.process_pid == targets[index].process_pid) {
                entry->worker_metrics = sampled;
            } else {
                memset(&entry->worker_metrics, 0,
                       sizeof(entry->worker_metrics));
                entry->worker_metrics.pid = (pid_t)-1;
            }
            pthread_rwlock_unlock(&manager->snapshot_lock);
        }
        wait_for_next_metrics_sample(manager);
    }
    return NULL;
}

static void attach_worker_metrics(const gw_channel_entry *entry,
                                  gw_channel_snapshot *snapshot)
{
    memset(&snapshot->worker_metrics, 0, sizeof(snapshot->worker_metrics));
    snapshot->worker_metrics.pid = snapshot->process_pid;
    if (snapshot->process_kind == GW_CHANNEL_PROCESS_WORKER &&
        entry->worker_metrics.pid == snapshot->process_pid) {
        snapshot->worker_metrics = entry->worker_metrics;
    }
}

static void *run_channel(void *context)
{
    gw_channel_entry *entry = context;

    entry->result = gw_supervisor_run(&entry->run_config,
                                      &entry->run_config.channels[0],
                                      &entry->options);
    atomic_store(&entry->thread_running, false);
    atomic_fetch_sub(&entry->manager->active_threads, 1U);
    return NULL;
}

static bool retry_policy_equal(const gw_retry_policy *first,
                               const gw_retry_policy *second)
{
    return first->startup_timeout_sec == second->startup_timeout_sec &&
           first->progress_timeout_sec == second->progress_timeout_sec &&
           first->stable_run_sec == second->stable_run_sec &&
           first->stop_timeout_sec == second->stop_timeout_sec &&
           first->max_retries == second->max_retries &&
           first->max_backoff_sec == second->max_backoff_sec;
}

static bool channel_config_equal(const gw_channel_config *first,
                                 const gw_channel_config *second)
{
    return first->enabled == second->enabled &&
           strcmp(first->id, second->id) == 0 &&
           strcmp(first->input.type, second->input.type) == 0 &&
           strcmp(first->input.url, second->input.url) == 0 &&
           strcmp(first->input.transport, second->input.transport) == 0 &&
           strcmp(first->video.decoder, second->video.decoder) == 0 &&
           first->video.width == second->video.width &&
           first->video.height == second->video.height &&
           strcmp(first->video.encoder, second->video.encoder) == 0 &&
           first->video.bitrate_kbps == second->video.bitrate_kbps &&
           first->video.fps == second->video.fps &&
           strcmp(first->output.path, second->output.path) == 0;
}

static bool worker_globals_equal(const gw_config *first, const gw_config *second)
{
    return strcmp(first->mediamtx.publish_base_url,
                  second->mediamtx.publish_base_url) == 0 &&
           retry_policy_equal(&first->defaults, &second->defaults);
}

static const gw_channel_config *find_enabled_channel(const gw_config *config,
                                                     const char *id)
{
    size_t index;

    for (index = 0U; index < config->channel_count; ++index) {
        if (config->channels[index].enabled &&
            strcmp(config->channels[index].id, id) == 0) {
            return &config->channels[index];
        }
    }
    return NULL;
}

static gw_channel_entry *find_entry(gw_channel_manager *manager, const char *id)
{
    size_t index;

    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        if (manager->entries[index].occupied &&
            strcmp(manager->entries[index].run_config.channels[0].id, id) == 0) {
            return &manager->entries[index];
        }
    }
    return NULL;
}

static gw_channel_entry *find_free_entry(gw_channel_manager *manager)
{
    size_t index;

    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        if (!manager->entries[index].occupied) {
            return &manager->entries[index];
        }
    }
    return NULL;
}

static void configure_entry(gw_channel_entry *entry,
                            const gw_channel_config *channel,
                            const gw_config *config)
{
    if (!entry->occupied) {
        entry->generation = 0U;
    }
    gw_config_init(&entry->run_config);
    entry->run_config.server = config->server;
    entry->run_config.mediamtx = config->mediamtx;
    entry->run_config.defaults = config->defaults;
    entry->run_config.channels[0] = *channel;
    entry->run_config.channel_count = 1U;
    entry->options = entry->manager->caller_options;
    entry->options.stop_signal = NULL;
    entry->options.stop_check = entry_stop_requested;
    entry->options.stop_context = entry;
    entry->options.observer = manager_observe;
    entry->options.observer_context = entry;
    entry->result = 0;
    atomic_store(&entry->stop_signal, 0);
    ++entry->generation;
    gw_channel_snapshot_init(&entry->snapshot, channel);
    entry->snapshot.configuration_generation = entry->generation;
    memset(&entry->worker_metrics, 0, sizeof(entry->worker_metrics));
    entry->worker_metrics.pid = (pid_t)-1;
    entry->occupied = true;
}

static gw_status start_entry(gw_channel_entry *entry, gw_error *error)
{
    int result;

    entry->result = 0;
    atomic_store(&entry->stop_signal, 0);
    atomic_store(&entry->thread_running, true);
    atomic_fetch_add(&entry->manager->active_threads, 1U);
    result = pthread_create(&entry->thread, NULL, run_channel, entry);
    if (result != 0) {
        atomic_fetch_sub(&entry->manager->active_threads, 1U);
        atomic_store(&entry->thread_running, false);
        gw_error_set(error, GW_ERR_IO, "cannot start channel '%s': %s",
                  entry->run_config.channels[0].id, strerror(result));
        return GW_ERR_IO;
    }
    entry->thread_started = true;
    return GW_OK;
}

static void stop_entry(gw_channel_entry *entry)
{
    if (entry->thread_started) {
        atomic_store(&entry->stop_signal, SIGTERM);
        pthread_join(entry->thread, NULL);
        entry->thread_started = false;
    }
}

gw_status gw_channel_manager_create(gw_channel_manager **manager_output,
                                    const gw_config *config,
                                    const gw_supervisor_options *options,
                                    gw_error *error)
{
    gw_channel_manager *manager;
    size_t index;
    int result;
    size_t enabled_count = 0U;

    if (manager_output == NULL || config == NULL || options == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT,
                  "manager output, configuration, and options are required");
        return GW_ERR_ARGUMENT;
    }
    *manager_output = NULL;
    if (options->ffmpeg_binary == NULL || options->ffmpeg_binary[0] == '\0') {
        gw_error_set(error, GW_ERR_VALIDATION,
                  "manager executable names must not be empty");
        return GW_ERR_VALIDATION;
    }
    if (gw_config_validate(config, error) != GW_OK) {
        return error != NULL ? error->code : GW_ERR_VALIDATION;
    }
    for (index = 0U; index < config->channel_count; ++index) {
        enabled_count += config->channels[index].enabled ? 1U : 0U;
    }
    if (enabled_count == 0U) {
        gw_error_set(error, GW_ERR_VALIDATION,
                  "at least one enabled channel is required");
        return GW_ERR_VALIDATION;
    }

    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        gw_error_set(error, GW_ERR_NO_MEMORY, "cannot allocate channel manager");
        return GW_ERR_NO_MEMORY;
    }
    manager->config = *config;
    manager->caller_options = *options;
    atomic_init(&manager->stop_signal, 0);
    atomic_init(&manager->active_threads, 0U);
    atomic_init(&manager->metrics_stop_requested, false);
    result = pthread_rwlock_init(&manager->snapshot_lock, NULL);
    if (result != 0) {
        free(manager);
        gw_error_set(error, GW_ERR_IO, "cannot initialize snapshot lock: %s",
                  strerror(result));
        return GW_ERR_IO;
    }
    result = pthread_mutex_init(&manager->lifecycle_lock, NULL);
    if (result != 0) {
        pthread_rwlock_destroy(&manager->snapshot_lock);
        free(manager);
        gw_error_set(error, GW_ERR_IO, "cannot initialize lifecycle lock: %s",
                  strerror(result));
        return GW_ERR_IO;
    }
    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        manager->entries[index].manager = manager;
        atomic_init(&manager->entries[index].stop_signal, 0);
        atomic_init(&manager->entries[index].thread_running, false);
        gw_process_metrics_tracker_init(
            &manager->entries[index].metrics_tracker);
        manager->entries[index].worker_metrics.pid = (pid_t)-1;
    }
    for (index = 0U; index < config->channel_count; ++index) {
        gw_channel_entry *entry;

        if (!config->channels[index].enabled) {
            continue;
        }
        entry = find_free_entry(manager);
        configure_entry(entry, &config->channels[index], config);
    }
    *manager_output = manager;
    gw_error_clear(error);
    return GW_OK;
}

gw_status gw_channel_manager_start(gw_channel_manager *manager, gw_error *error)
{
    size_t index;
    gw_status status;

    if (manager == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT, "channel manager is required");
        return GW_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&manager->lifecycle_lock);
    if (manager->started) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        gw_error_set(error, GW_ERR_VALIDATION, "channel manager is already started");
        return GW_ERR_VALIDATION;
    }
    manager->started = true;
    atomic_store(&manager->metrics_stop_requested, false);
    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        if (!manager->entries[index].occupied) {
            continue;
        }
        status = start_entry(&manager->entries[index], error);
        if (status != GW_OK) {
            gw_channel_manager_request_stop(manager, SIGTERM);
            for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
                stop_entry(&manager->entries[index]);
            }
            pthread_mutex_unlock(&manager->lifecycle_lock);
            return status;
        }
    }
    {
        int result = pthread_create(&manager->metrics_thread, NULL,
                                    sample_process_metrics, manager);

        if (result != 0) {
            gw_channel_manager_request_stop(manager, SIGTERM);
            for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
                stop_entry(&manager->entries[index]);
            }
            manager->started = false;
            pthread_mutex_unlock(&manager->lifecycle_lock);
            gw_error_set(error, GW_ERR_IO, "cannot start metrics sampler: %s",
                      strerror(result));
            return GW_ERR_IO;
        }
        manager->metrics_thread_started = true;
    }
    pthread_mutex_unlock(&manager->lifecycle_lock);
    gw_error_clear(error);
    return GW_OK;
}

gw_status gw_channel_manager_reload(gw_channel_manager *manager,
                                    const gw_config *candidate,
                                    gw_channel_reload_summary *summary,
                                    gw_error *error)
{
    gw_channel_reload_summary changes = {0};
    bool globals_equal;
    size_t index;
    gw_status final_status = GW_OK;

    if (manager == NULL || candidate == NULL || summary == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT,
                  "manager, candidate configuration, and summary are required");
        return GW_ERR_ARGUMENT;
    }
    memset(summary, 0, sizeof(*summary));
    pthread_mutex_lock(&manager->lifecycle_lock);
    if (!manager->started || atomic_load(&manager->stop_signal) != 0) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        gw_error_set(error, GW_ERR_VALIDATION,
                  "channel manager is not available for reload");
        return GW_ERR_VALIDATION;
    }
    if (gw_config_validate(candidate, error) != GW_OK) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        return error != NULL ? error->code : GW_ERR_VALIDATION;
    }
    globals_equal = worker_globals_equal(&manager->config, candidate);

    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        gw_channel_entry *entry = &manager->entries[index];
        const gw_channel_config *replacement;

        if (!entry->occupied) {
            continue;
        }
        replacement = find_enabled_channel(
            candidate, entry->run_config.channels[0].id);
        if (replacement == NULL) {
            stop_entry(entry);
            pthread_rwlock_wrlock(&manager->snapshot_lock);
            entry->occupied = false;
            pthread_rwlock_unlock(&manager->snapshot_lock);
            ++changes.removed;
        } else if (globals_equal &&
                   channel_config_equal(&entry->run_config.channels[0],
                                        replacement)) {
            ++changes.unchanged;
        } else {
            stop_entry(entry);
            pthread_rwlock_wrlock(&manager->snapshot_lock);
            configure_entry(entry, replacement, candidate);
            pthread_rwlock_unlock(&manager->snapshot_lock);
            if (start_entry(entry, error) != GW_OK) {
                final_status = GW_ERR_IO;
            }
            ++changes.restarted;
        }
    }

    for (index = 0U; index < candidate->channel_count; ++index) {
        gw_channel_entry *entry;

        if (!candidate->channels[index].enabled ||
            find_entry(manager, candidate->channels[index].id) != NULL) {
            continue;
        }
        entry = find_free_entry(manager);
        if (entry == NULL) {
            gw_error_set(error, GW_ERR_OVERFLOW, "channel manager capacity exceeded");
            final_status = GW_ERR_OVERFLOW;
            break;
        }
        pthread_rwlock_wrlock(&manager->snapshot_lock);
        configure_entry(entry, &candidate->channels[index], candidate);
        pthread_rwlock_unlock(&manager->snapshot_lock);
        if (start_entry(entry, error) != GW_OK) {
            final_status = GW_ERR_IO;
        }
        ++changes.added;
    }
    manager->config = *candidate;
    *summary = changes;
    if (final_status == GW_OK) {
        gw_error_clear(error);
    }
    pthread_mutex_unlock(&manager->lifecycle_lock);
    return final_status;
}

gw_status gw_channel_manager_get_snapshot(gw_channel_manager *manager,
                                          const char *channel_id,
                                          gw_channel_snapshot *snapshot,
                                          gw_error *error)
{
    size_t index;

    if (manager == NULL || channel_id == NULL || snapshot == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT,
                  "manager, channel id, and snapshot output are required");
        return GW_ERR_ARGUMENT;
    }
    pthread_rwlock_rdlock(&manager->snapshot_lock);
    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        if (manager->entries[index].occupied &&
            strcmp(manager->entries[index].snapshot.channel_id, channel_id) == 0) {
            *snapshot = manager->entries[index].snapshot;
            attach_worker_metrics(&manager->entries[index], snapshot);
            pthread_rwlock_unlock(&manager->snapshot_lock);
            gw_error_clear(error);
            return GW_OK;
        }
    }
    pthread_rwlock_unlock(&manager->snapshot_lock);
    gw_error_set(error, GW_ERR_NOT_FOUND, "channel '%s' is not managed", channel_id);
    return GW_ERR_NOT_FOUND;
}

gw_status gw_channel_manager_list_snapshots(gw_channel_manager *manager,
                                            gw_channel_snapshot *snapshots,
                                            size_t capacity, size_t *count,
                                            gw_error *error)
{
    size_t index;
    size_t copied = 0U;

    if (manager == NULL || snapshots == NULL || count == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT,
                  "manager, snapshot array, and count output are required");
        return GW_ERR_ARGUMENT;
    }
    *count = 0U;
    pthread_rwlock_rdlock(&manager->snapshot_lock);
    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        if (!manager->entries[index].occupied) {
            continue;
        }
        if (copied >= capacity) {
            pthread_rwlock_unlock(&manager->snapshot_lock);
            gw_error_set(error, GW_ERR_OVERFLOW,
                      "snapshot array capacity is too small");
            return GW_ERR_OVERFLOW;
        }
        snapshots[copied] = manager->entries[index].snapshot;
        attach_worker_metrics(&manager->entries[index], &snapshots[copied]);
        ++copied;
    }
    pthread_rwlock_unlock(&manager->snapshot_lock);
    *count = copied;
    gw_error_clear(error);
    return GW_OK;
}

static gw_channel_state entry_state(gw_channel_entry *entry)
{
    gw_channel_state state;

    pthread_rwlock_rdlock(&entry->manager->snapshot_lock);
    state = entry->snapshot.state;
    pthread_rwlock_unlock(&entry->manager->snapshot_lock);
    return state;
}

static gw_status command_entry(gw_channel_manager *manager,
                               const char *channel_id,
                               gw_channel_entry **entry_output,
                               gw_error *error)
{
    gw_channel_entry *entry;

    if (!manager->started || atomic_load(&manager->stop_signal) != 0) {
        gw_error_set(error, GW_ERR_CONFLICT,
                  "channel manager is not available for control");
        return GW_ERR_CONFLICT;
    }
    entry = find_entry(manager, channel_id);
    if (entry == NULL) {
        gw_error_set(error, GW_ERR_NOT_FOUND, "channel '%s' is not managed",
                  channel_id);
        return GW_ERR_NOT_FOUND;
    }
    *entry_output = entry;
    return GW_OK;
}

gw_status gw_channel_manager_start_channel(gw_channel_manager *manager,
                                           const char *channel_id,
                                           gw_error *error)
{
    gw_channel_entry *entry;
    gw_channel_state state;
    gw_status status;

    if (manager == NULL || channel_id == NULL || channel_id[0] == '\0') {
        gw_error_set(error, GW_ERR_ARGUMENT, "manager and channel id are required");
        return GW_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&manager->lifecycle_lock);
    status = command_entry(manager, channel_id, &entry, error);
    if (status != GW_OK) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        return status;
    }
    if (entry->thread_started && !atomic_load(&entry->thread_running)) {
        pthread_join(entry->thread, NULL);
        entry->thread_started = false;
    }
    if (entry->thread_started) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        gw_error_set(error, GW_ERR_CONFLICT, "channel '%s' is already active",
                  channel_id);
        return GW_ERR_CONFLICT;
    }
    state = entry_state(entry);
    if (state != GW_CHANNEL_STOPPED && state != GW_CHANNEL_FAILED) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        gw_error_set(error, GW_ERR_CONFLICT, "channel '%s' cannot be started from %s",
                  channel_id, gw_channel_state_string(state));
        return GW_ERR_CONFLICT;
    }
    status = start_entry(entry, error);
    pthread_mutex_unlock(&manager->lifecycle_lock);
    if (status == GW_OK) {
        gw_error_clear(error);
    }
    return status;
}

gw_status gw_channel_manager_stop_channel(gw_channel_manager *manager,
                                          const char *channel_id,
                                          gw_error *error)
{
    gw_channel_entry *entry;
    gw_status status;

    if (manager == NULL || channel_id == NULL || channel_id[0] == '\0') {
        gw_error_set(error, GW_ERR_ARGUMENT, "manager and channel id are required");
        return GW_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&manager->lifecycle_lock);
    status = command_entry(manager, channel_id, &entry, error);
    if (status != GW_OK) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        return status;
    }
    if (!entry->thread_started || !atomic_load(&entry->thread_running)) {
        if (entry->thread_started) {
            pthread_join(entry->thread, NULL);
            entry->thread_started = false;
        }
        pthread_mutex_unlock(&manager->lifecycle_lock);
        gw_error_set(error, GW_ERR_CONFLICT, "channel '%s' is not active", channel_id);
        return GW_ERR_CONFLICT;
    }
    stop_entry(entry);
    pthread_mutex_unlock(&manager->lifecycle_lock);
    gw_error_clear(error);
    return GW_OK;
}

gw_status gw_channel_manager_restart_channel(gw_channel_manager *manager,
                                             const char *channel_id,
                                             gw_error *error)
{
    gw_channel_entry *entry;
    gw_status status;

    if (manager == NULL || channel_id == NULL || channel_id[0] == '\0') {
        gw_error_set(error, GW_ERR_ARGUMENT, "manager and channel id are required");
        return GW_ERR_ARGUMENT;
    }
    pthread_mutex_lock(&manager->lifecycle_lock);
    status = command_entry(manager, channel_id, &entry, error);
    if (status != GW_OK) {
        pthread_mutex_unlock(&manager->lifecycle_lock);
        return status;
    }
    stop_entry(entry);
    status = start_entry(entry, error);
    pthread_mutex_unlock(&manager->lifecycle_lock);
    if (status == GW_OK) {
        gw_error_clear(error);
    }
    return status;
}

void gw_channel_manager_request_stop(gw_channel_manager *manager,
                                     int signal_number)
{
    if (manager != NULL) {
        atomic_store(&manager->stop_signal,
                     signal_number != 0 ? signal_number : SIGTERM);
        atomic_store(&manager->metrics_stop_requested, true);
    }
}

bool gw_channel_manager_is_finished(const gw_channel_manager *manager)
{
    return manager != NULL && manager->started &&
           atomic_load(&manager->active_threads) == 0U;
}

int gw_channel_manager_wait(gw_channel_manager *manager)
{
    size_t index;

    if (manager == NULL || !manager->started) {
        return 2;
    }
    pthread_mutex_lock(&manager->lifecycle_lock);
    manager->result = 0;
    for (index = 0U; index < GW_MAX_CHANNELS; ++index) {
        gw_channel_entry *entry = &manager->entries[index];

        if (entry->occupied && entry->thread_started) {
            pthread_join(entry->thread, NULL);
            entry->thread_started = false;
        }
        if (entry->occupied && entry->result != 0) {
            manager->result = 1;
        }
    }
    atomic_store(&manager->metrics_stop_requested, true);
    if (manager->metrics_thread_started) {
        pthread_join(manager->metrics_thread, NULL);
        manager->metrics_thread_started = false;
    }
    pthread_mutex_unlock(&manager->lifecycle_lock);
    return manager->result;
}

void gw_channel_manager_destroy(gw_channel_manager *manager)
{
    if (manager == NULL) {
        return;
    }
    if (manager->started && !gw_channel_manager_is_finished(manager)) {
        gw_channel_manager_request_stop(manager, SIGTERM);
    }
    if (manager->started) {
        gw_channel_manager_wait(manager);
    }
    pthread_mutex_destroy(&manager->lifecycle_lock);
    pthread_rwlock_destroy(&manager->snapshot_lock);
    free(manager);
}
