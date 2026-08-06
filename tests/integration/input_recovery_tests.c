#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/supervisor.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    atomic_bool saw_input_failure;
    atomic_bool saw_recovered_running;
    atomic_bool healthy_restarted;
} recovery_observations;

static int failures;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void make_channel(gw_channel_config *channel, const char *id,
                         const char *input_path)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->id, sizeof(channel->id), "%s", id);
    channel->enabled = true;
    snprintf(channel->input.type, sizeof(channel->input.type), "%s", "rtsp");
    snprintf(channel->input.url, sizeof(channel->input.url),
             "rtsp://fixture-user:fixture-password@camera/%s", input_path);
    snprintf(channel->input.transport, sizeof(channel->input.transport), "%s",
             "tcp");
    snprintf(channel->video.decoder, sizeof(channel->video.decoder), "%s",
             "h264_rkmpp");
    channel->video.width = 1280;
    channel->video.height = 720;
    snprintf(channel->video.encoder, sizeof(channel->video.encoder), "%s",
             "h264_rkmpp");
    channel->video.bitrate_kbps = 4000;
    channel->video.fps = 25;
    snprintf(channel->output.path, sizeof(channel->output.path), "%s", id);
}

static void make_config(gw_config *config)
{
    gw_config_init(config);
    config->defaults.startup_timeout_sec = 1;
    config->defaults.progress_timeout_sec = 5;
    config->defaults.stable_run_sec = 5;
    config->defaults.stop_timeout_sec = 1;
    config->defaults.max_retries = 2;
    config->defaults.max_backoff_sec = 1;
    config->channel_count = 2U;
    make_channel(&config->channels[0], "cam01", "hold-healthy");
    make_channel(&config->channels[1], "cam02", "recover-once");
}

static void observe_recovery(const gw_channel_snapshot *snapshot, void *context)
{
    recovery_observations *observations = context;

    if (strcmp(snapshot->channel_id, "cam01") == 0 &&
        snapshot->total_restarts != 0U) {
        atomic_store(&observations->healthy_restarted, true);
    }
    if (strcmp(snapshot->channel_id, "cam02") != 0) {
        return;
    }
    if (snapshot->state == GW_CHANNEL_BACKOFF &&
        strcmp(snapshot->last_event, "worker_failure") == 0 &&
        snapshot->has_exit_code && snapshot->last_exit_code == 9) {
        atomic_store(&observations->saw_input_failure, true);
    }
    if (snapshot->state == GW_CHANNEL_RUNNING &&
        snapshot->total_restarts == 1U) {
        atomic_store(&observations->saw_recovered_running, true);
    }
}

static bool wait_for_recovery(gw_channel_manager *manager,
                              gw_channel_snapshot *snapshot)
{
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 20000000L};
    gw_error error = {0};
    int attempt;

    for (attempt = 0; attempt < 200; ++attempt) {
        if (gw_channel_manager_get_snapshot(manager, "cam02", snapshot, &error) ==
                GW_OK &&
            snapshot->state == GW_CHANNEL_RUNNING &&
            snapshot->total_restarts == 1U) {
            return true;
        }
        nanosleep(&pause_time, NULL);
    }
    return false;
}

int main(int argc, char **argv)
{
    char marker_path[] = "/tmp/gateway-input-recovery-XXXXXX";
    recovery_observations observations;
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_snapshot healthy;
    gw_channel_snapshot recovered;
    gw_config config;
    gw_error error = {0};
    gw_status status;
    int marker_descriptor;
    bool manager_started = false;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    marker_descriptor = mkstemp(marker_path);
    if (marker_descriptor < 0) {
        perror("cannot reserve recovery marker path");
        return 1;
    }
    close(marker_descriptor);
    unlink(marker_path);
    if (setenv("GW_FIXTURE_RECOVERY_FILE", marker_path, 1) < 0) {
        perror("cannot configure recovery marker path");
        return 1;
    }

    atomic_init(&observations.saw_input_failure, false);
    atomic_init(&observations.saw_recovered_running, false);
    atomic_init(&observations.healthy_restarted, false);
    make_config(&config);
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = argv[1];
    options.ffmpeg_binary = argv[1];
    options.observer = observe_recovery;
    options.observer_context = &observations;

    status = gw_channel_manager_create(&manager, &config, &options, &error);
    CHECK(status == GW_OK);
    if (status == GW_OK) {
        status = gw_channel_manager_start(manager, &error);
        CHECK(status == GW_OK);
        manager_started = status == GW_OK;
    }
    if (manager_started) {
        CHECK(wait_for_recovery(manager, &recovered));
        CHECK(access(marker_path, F_OK) == 0);
        CHECK(atomic_load(&observations.saw_input_failure));
        CHECK(atomic_load(&observations.saw_recovered_running));
        CHECK(!atomic_load(&observations.healthy_restarted));
        CHECK(gw_channel_manager_get_snapshot(manager, "cam01", &healthy,
                                              &error) == GW_OK);
        CHECK(healthy.state == GW_CHANNEL_RUNNING);
        CHECK(healthy.total_restarts == 0U);
        CHECK(healthy.configuration_generation == 1U);
        CHECK(recovered.state == GW_CHANNEL_RUNNING);
        CHECK(recovered.total_restarts == 1U);
        CHECK(recovered.configuration_generation == 1U);

        gw_channel_manager_request_stop(manager, SIGTERM);
        CHECK(gw_channel_manager_wait(manager) == 0);
    }

    gw_channel_manager_destroy(manager);
    unsetenv("GW_FIXTURE_RECOVERY_FILE");
    unlink(marker_path);
    if (failures != 0) {
        fprintf(stderr, "%d input recovery test(s) failed.\n", failures);
        return 1;
    }
    printf("Input recovery isolation test passed.\n");
    return 0;
}
