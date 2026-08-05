#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/supervisor.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

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

static void make_config(gw_config *config, bool inject_failure)
{
    gw_config_init(config);
    config->defaults.max_retries = 0;
    config->defaults.stop_timeout_sec = 1;
    config->channel_count = 2U;
    make_channel(&config->channels[0], "cam01", "live");
    make_channel(&config->channels[1], "cam02",
                 inject_failure ? "worker-fail" : "live-two");
}

static void test_two_channel_success(const char *fixture)
{
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_snapshot first;
    gw_channel_snapshot second;
    gw_config config;
    gw_error error = {0};

    make_config(&config, false);
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam01", &first, &error) ==
          GW_OK);
    CHECK(first.state == GW_CHANNEL_STOPPED);
    CHECK(gw_channel_manager_start(manager, &error) == GW_OK);
    CHECK(gw_channel_manager_wait(manager) == 0);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam01", &first, &error) ==
          GW_OK);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam02", &second, &error) ==
          GW_OK);
    CHECK(first.state == GW_CHANNEL_STOPPED);
    CHECK(second.state == GW_CHANNEL_STOPPED);
    CHECK(strcmp(first.last_event, "clean_exit") == 0);
    CHECK(strcmp(second.last_event, "clean_exit") == 0);
    CHECK(first.has_progress && second.has_progress);
    CHECK(gw_channel_manager_get_snapshot(manager, "missing", &first, &error) ==
          GW_ERR_VALIDATION);
    gw_channel_manager_destroy(manager);
}

static void test_channel_failure_isolation(const char *fixture)
{
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_snapshot healthy;
    gw_channel_snapshot failed;
    gw_config config;
    gw_error error = {0};

    make_config(&config, true);
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    CHECK(gw_channel_manager_start(manager, &error) == GW_OK);
    CHECK(gw_channel_manager_wait(manager) == 1);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam01", &healthy, &error) ==
          GW_OK);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam02", &failed, &error) ==
          GW_OK);
    CHECK(healthy.state == GW_CHANNEL_STOPPED);
    CHECK(strcmp(healthy.last_event, "clean_exit") == 0);
    CHECK(healthy.last_exit_code == 0);
    CHECK(failed.state == GW_CHANNEL_FAILED);
    CHECK(strcmp(failed.last_event, "worker_failure") == 0);
    CHECK(failed.last_exit_code == 9);
    CHECK(failed.consecutive_failures == 1U);
    gw_channel_manager_destroy(manager);
}

static void test_stop_all_channels(const char *fixture)
{
    volatile sig_atomic_t stop_signal = SIGTERM;
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_snapshot snapshot;
    gw_config config;
    gw_error error = {0};

    make_config(&config, false);
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    options.stop_signal = &stop_signal;
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    CHECK(gw_channel_manager_start(manager, &error) == GW_OK);
    CHECK(gw_channel_manager_wait(manager) == 0);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam01", &snapshot, &error) ==
          GW_OK);
    CHECK(snapshot.state == GW_CHANNEL_STOPPED);
    CHECK(strcmp(snapshot.last_event, "stop_requested") == 0);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam02", &snapshot, &error) ==
          GW_OK);
    CHECK(snapshot.state == GW_CHANNEL_STOPPED);
    CHECK(strcmp(snapshot.last_event, "stop_requested") == 0);
    gw_channel_manager_destroy(manager);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    test_two_channel_success(argv[1]);
    test_channel_failure_isolation(argv[1]);
    test_stop_all_channels(argv[1]);
    if (failures != 0) {
        fprintf(stderr, "%d channel manager test(s) failed.\n", failures);
        return 1;
    }
    printf("All channel manager tests passed.\n");
    return 0;
}
