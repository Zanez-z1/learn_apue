#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/supervisor.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    options.stop_on_clean_exit = true;
    options.exit_on_retry_exhaustion = true;
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
          GW_ERR_NOT_FOUND);
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
    options.stop_on_clean_exit = true;
    options.exit_on_retry_exhaustion = true;
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

static int wait_for_state(gw_channel_manager *manager, const char *channel_id,
                          gw_channel_state state, uint64_t generation,
                          gw_channel_snapshot *snapshot)
{
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 20000000L};
    gw_error error = {0};
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        if (gw_channel_manager_get_snapshot(manager, channel_id, snapshot, &error) ==
                GW_OK &&
            snapshot->state == state &&
            snapshot->configuration_generation == generation) {
            return 1;
        }
        nanosleep(&pause_time, NULL);
    }
    return 0;
}

static int wait_for_process_metrics(gw_channel_manager *manager,
                                    const char *channel_id,
                                    gw_channel_snapshot *snapshot)
{
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 20000000L};
    gw_error error = {0};
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        if (gw_channel_manager_get_snapshot(manager, channel_id, snapshot,
                                            &error) == GW_OK &&
            snapshot->worker_metrics.available &&
            snapshot->worker_metrics.cpu_available &&
            snapshot->worker_metrics.pid == snapshot->process_pid) {
            return 1;
        }
        nanosleep(&pause_time, NULL);
    }
    return 0;
}

typedef struct {
    gw_channel_manager *manager;
    const char *channel_id;
    int failed;
} snapshot_reader;

static void *read_snapshots(void *context)
{
    snapshot_reader *reader = context;
    gw_error error = {0};
    int iteration;

    for (iteration = 0; iteration < 1000; ++iteration) {
        gw_channel_snapshot snapshot;

        if (gw_channel_manager_get_snapshot(reader->manager, reader->channel_id,
                                            &snapshot, &error) != GW_OK) {
            reader->failed = 1;
            break;
        }
        if (snapshot.worker_metrics.available &&
            (snapshot.process_kind != GW_CHANNEL_PROCESS_WORKER ||
             snapshot.worker_metrics.pid != snapshot.process_pid)) {
            reader->failed = 1;
            break;
        }
    }
    return NULL;
}

static void check_concurrent_snapshot_reads(gw_channel_manager *manager)
{
    snapshot_reader readers[4];
    pthread_t threads[4];
    int created[4];
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        readers[index].manager = manager;
        readers[index].channel_id = index % 2U == 0U ? "cam01" : "cam02";
        readers[index].failed = 0;
        created[index] = pthread_create(&threads[index], NULL, read_snapshots,
                                        &readers[index]);
        CHECK(created[index] == 0);
    }
    for (index = 0U; index < 4U; ++index) {
        if (created[index] == 0) {
            pthread_join(threads[index], NULL);
            CHECK(readers[index].failed == 0);
        }
    }
}

typedef struct {
    gw_channel_manager *manager;
    const char *channel_id;
    gw_status status;
} start_request;

static void *start_channel(void *context)
{
    start_request *request = context;
    gw_error error = {0};

    request->status = gw_channel_manager_start_channel(
        request->manager, request->channel_id, &error);
    return NULL;
}

static void test_channel_controls(const char *fixture)
{
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_snapshot snapshot;
    gw_config config;
    gw_error error = {0};
    start_request first_request;
    start_request second_request;
    pthread_t first_thread;
    pthread_t second_thread;
    int first_created;
    int second_created;
    pid_t original_pid;

    make_config(&config, false);
    snprintf(config.channels[0].input.url, sizeof(config.channels[0].input.url),
             "%s", "rtsp://fixture-user:fixture-password@camera/hold");
    snprintf(config.channels[1].input.url, sizeof(config.channels[1].input.url),
             "%s", "rtsp://fixture-user:fixture-password@camera/hold-two");
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    CHECK(gw_channel_manager_start_channel(manager, "cam01", &error) ==
          GW_ERR_CONFLICT);
    CHECK(gw_channel_manager_start(manager, &error) == GW_OK);
    CHECK(wait_for_state(manager, "cam01", GW_CHANNEL_RUNNING, 1U, &snapshot));
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 1U, &snapshot));
    CHECK(wait_for_process_metrics(manager, "cam02", &snapshot));
    CHECK(snapshot.worker_metrics.rss_kib > 0L);
    original_pid = snapshot.process_pid;
    check_concurrent_snapshot_reads(manager);

    CHECK(gw_channel_manager_stop_channel(manager, "missing", &error) ==
          GW_ERR_NOT_FOUND);
    CHECK(gw_channel_manager_stop_channel(manager, "cam02", &error) == GW_OK);
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_STOPPED, 1U, &snapshot));
    CHECK(!snapshot.worker_metrics.available);
    CHECK(!snapshot.worker_metrics.cpu_available);
    CHECK(gw_channel_manager_stop_channel(manager, "cam02", &error) ==
          GW_ERR_CONFLICT);
    CHECK(gw_channel_manager_start_channel(manager, "cam02", &error) == GW_OK);
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 1U, &snapshot));
    CHECK(snapshot.process_pid != original_pid);
    CHECK(!snapshot.worker_metrics.cpu_available);
    CHECK(wait_for_process_metrics(manager, "cam02", &snapshot));
    CHECK(gw_channel_manager_start_channel(manager, "cam02", &error) ==
          GW_ERR_CONFLICT);
    CHECK(gw_channel_manager_restart_channel(manager, "cam02", &error) == GW_OK);
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 1U, &snapshot));

    CHECK(gw_channel_manager_stop_channel(manager, "cam02", &error) == GW_OK);
    first_request.manager = manager;
    first_request.channel_id = "cam02";
    first_request.status = GW_ERR_IO;
    second_request = first_request;
    first_created = pthread_create(&first_thread, NULL, start_channel,
                                   &first_request);
    second_created = pthread_create(&second_thread, NULL, start_channel,
                                    &second_request);
    CHECK(first_created == 0);
    CHECK(second_created == 0);
    if (first_created == 0) {
        pthread_join(first_thread, NULL);
    }
    if (second_created == 0) {
        pthread_join(second_thread, NULL);
    }
    if (first_created == 0 && second_created == 0) {
        CHECK((first_request.status == GW_OK &&
               second_request.status == GW_ERR_CONFLICT) ||
              (first_request.status == GW_ERR_CONFLICT &&
               second_request.status == GW_OK));
    }
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 1U, &snapshot));

    gw_channel_manager_request_stop(manager, SIGTERM);
    CHECK(gw_channel_manager_wait(manager) == 0);
    CHECK(gw_channel_manager_start_channel(manager, "cam01", &error) ==
          GW_ERR_CONFLICT);
    gw_channel_manager_destroy(manager);
}

static void test_differential_reload(const char *fixture)
{
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_channel_reload_summary summary;
    gw_channel_snapshot first;
    gw_channel_snapshot second;
    gw_config config;
    gw_config candidate;
    gw_config invalid;
    gw_error error = {0};

    make_config(&config, false);
    snprintf(config.channels[0].input.url, sizeof(config.channels[0].input.url),
             "%s", "rtsp://fixture-user:fixture-password@camera/hold");
    snprintf(config.channels[1].input.url, sizeof(config.channels[1].input.url),
             "%s", "rtsp://fixture-user:fixture-password@camera/hold-two");
    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    CHECK(gw_channel_manager_start(manager, &error) == GW_OK);
    CHECK(wait_for_state(manager, "cam01", GW_CHANNEL_RUNNING, 1U, &first));
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 1U, &second));

    candidate = config;
    candidate.channels[1].video.bitrate_kbps = 5000;
    CHECK(gw_channel_manager_reload(manager, &candidate, &summary, &error) == GW_OK);
    CHECK(summary.unchanged == 1U);
    CHECK(summary.restarted == 1U);
    CHECK(summary.added == 0U);
    CHECK(summary.removed == 0U);
    CHECK(wait_for_state(manager, "cam01", GW_CHANNEL_RUNNING, 1U, &first));
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 2U, &second));

    invalid = candidate;
    invalid.channels[0].video.bitrate_kbps = 0;
    CHECK(gw_channel_manager_reload(manager, &invalid, &summary, &error) ==
          GW_ERR_VALIDATION);
    CHECK(summary.unchanged == 0U && summary.restarted == 0U);
    CHECK(wait_for_state(manager, "cam01", GW_CHANNEL_RUNNING, 1U, &first));
    CHECK(wait_for_state(manager, "cam02", GW_CHANNEL_RUNNING, 2U, &second));

    candidate.channels[1].enabled = false;
    candidate.channel_count = 3U;
    make_channel(&candidate.channels[2], "cam03", "hold-three");
    CHECK(gw_channel_manager_reload(manager, &candidate, &summary, &error) == GW_OK);
    CHECK(summary.unchanged == 1U);
    CHECK(summary.restarted == 0U);
    CHECK(summary.added == 1U);
    CHECK(summary.removed == 1U);
    CHECK(gw_channel_manager_get_snapshot(manager, "cam02", &second, &error) ==
          GW_ERR_NOT_FOUND);
    CHECK(wait_for_state(manager, "cam01", GW_CHANNEL_RUNNING, 1U, &first));
    CHECK(wait_for_state(manager, "cam03", GW_CHANNEL_RUNNING, 1U, &second));

    gw_channel_manager_request_stop(manager, SIGTERM);
    CHECK(gw_channel_manager_wait(manager) == 0);
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
    test_channel_controls(argv[1]);
    test_differential_reload(argv[1]);
    if (failures != 0) {
        fprintf(stderr, "%d channel manager test(s) failed.\n", failures);
        return 1;
    }
    printf("All channel manager tests passed.\n");
    return 0;
}
