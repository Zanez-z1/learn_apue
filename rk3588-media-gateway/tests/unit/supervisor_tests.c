#include "gateway/config.h"
#include "gateway/supervisor.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static int failures;

typedef struct {
    gw_channel_snapshot last;
    int updates;
    int saw_starting;
    int saw_running;
    int saw_stopped;
    int saw_worker_process;
} snapshot_capture;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void make_channel(gw_channel_config *channel)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->id, sizeof(channel->id), "%s", "cam01");
    channel->enabled = true;
    snprintf(channel->input.url, sizeof(channel->input.url), "%s",
             "rtsp://fixture-user:fixture-password@camera/live");
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
    snprintf(channel->output.path, sizeof(channel->output.path), "%s", "cam01");
}

static void capture_snapshot(const gw_channel_snapshot *snapshot, void *context)
{
    snapshot_capture *capture = context;

    capture->last = *snapshot;
    ++capture->updates;
    capture->saw_starting |= snapshot->state == GW_CHANNEL_STARTING;
    capture->saw_running |= snapshot->state == GW_CHANNEL_RUNNING;
    capture->saw_stopped |= snapshot->state == GW_CHANNEL_STOPPED;
    capture->saw_worker_process |=
        snapshot->process_kind == GW_CHANNEL_PROCESS_WORKER;
}

static void test_default_options(void)
{
    gw_supervisor_options options;

    gw_supervisor_options_init(&options);
    CHECK(strcmp(options.ffmpeg_binary, "ffmpeg") == 0);
    CHECK(options.stop_signal == NULL);
    CHECK(options.stop_check == NULL);
    CHECK(options.stop_context == NULL);
    CHECK(!options.stop_on_clean_exit);
    CHECK(!options.exit_on_retry_exhaustion);
    CHECK(options.observer == NULL);
    CHECK(options.observer_context == NULL);
}

static void test_argument_validation(void)
{
    gw_supervisor_options options;
    gw_config config;
    gw_channel_config channel;

    gw_supervisor_options_init(&options);
    gw_config_init(&config);
    make_channel(&channel);
    CHECK(gw_supervisor_run(NULL, &channel, &options) == 2);
    CHECK(gw_supervisor_run(&config, NULL, &options) == 2);
    options.ffmpeg_binary = "";
    CHECK(gw_supervisor_run(&config, &channel, &options) == 2);
}

static void test_stop_during_worker(const char *fixture)
{
    volatile sig_atomic_t stop_signal = SIGTERM;
    snapshot_capture capture = {0};
    gw_supervisor_options options;
    gw_config config;
    gw_channel_config channel;

    gw_supervisor_options_init(&options);
    options.ffmpeg_binary = fixture;
    options.stop_signal = &stop_signal;
    options.observer = capture_snapshot;
    options.observer_context = &capture;
    gw_config_init(&config);
    config.defaults.stop_timeout_sec = 1;
    make_channel(&channel);
    CHECK(gw_supervisor_run(&config, &channel, &options) == 0);
    CHECK(capture.saw_starting);
    CHECK(capture.saw_worker_process);
    CHECK(capture.last.state == GW_CHANNEL_STOPPED);
    CHECK(strcmp(capture.last.last_event, "stop_requested") == 0);
    CHECK(capture.last.process_kind == GW_CHANNEL_PROCESS_NONE);
}

static void test_success_snapshots(const char *fixture)
{
    snapshot_capture capture = {0};
    gw_supervisor_options options;
    gw_config config;
    gw_channel_config channel;

    gw_supervisor_options_init(&options);
    options.ffmpeg_binary = fixture;
    options.stop_on_clean_exit = true;
    options.observer = capture_snapshot;
    options.observer_context = &capture;
    gw_config_init(&config);
    make_channel(&channel);
    CHECK(gw_supervisor_run(&config, &channel, &options) == 0);
    CHECK(capture.updates >= 5);
    CHECK(capture.saw_starting);
    CHECK(capture.saw_running);
    CHECK(capture.saw_stopped);
    CHECK(capture.saw_worker_process);
    CHECK(strcmp(capture.last.channel_id, "cam01") == 0);
    CHECK(strcmp(capture.last.last_event, "clean_exit") == 0);
    CHECK(capture.last.has_progress);
    CHECK(capture.last.progress.frame == 42U);
    CHECK(capture.last.has_exit_code);
    CHECK(capture.last.last_exit_code == 0);
    CHECK(capture.last.process_kind == GW_CHANNEL_PROCESS_NONE);
}

static void test_unsolicited_zero_exit_retries(const char *fixture)
{
    snapshot_capture capture = {0};
    gw_supervisor_options options;
    gw_config config;
    gw_channel_config channel;

    gw_supervisor_options_init(&options);
    options.ffmpeg_binary = fixture;
    options.observer = capture_snapshot;
    options.observer_context = &capture;
    gw_config_init(&config);
    config.defaults.max_retries = 0;
    options.exit_on_retry_exhaustion = true;
    make_channel(&channel);
    CHECK(gw_supervisor_run(&config, &channel, &options) == 1);
    CHECK(capture.last.state == GW_CHANNEL_FAILED);
    CHECK(strcmp(capture.last.last_event, "worker_failure") == 0);
    CHECK(capture.last.has_exit_code);
    CHECK(capture.last.last_exit_code == 0);
    CHECK(capture.last.consecutive_failures == 1U);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    test_default_options();
    test_argument_validation();
    test_stop_during_worker(argv[1]);
    test_success_snapshots(argv[1]);
    test_unsolicited_zero_exit_retries(argv[1]);
    if (failures != 0) {
        fprintf(stderr, "%d supervisor test(s) failed.\n", failures);
        return 1;
    }
    printf("All supervisor tests passed.\n");
    return 0;
}
