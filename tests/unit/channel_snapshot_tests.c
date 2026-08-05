#include "gateway/channel_snapshot.h"

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

static void test_snapshot_updates(void)
{
    gw_channel_config channel = {0};
    gw_channel_runtime runtime = {0};
    gw_channel_snapshot snapshot;
    gw_probe_info probe = {0};
    gw_worker_progress progress = {0};

    snprintf(channel.id, sizeof(channel.id), "%s", "cam01");
    channel.enabled = true;
    gw_channel_snapshot_init(&snapshot, &channel);
    CHECK(strcmp(snapshot.channel_id, "cam01") == 0);
    CHECK(snapshot.state == GW_CHANNEL_STOPPED);
    CHECK(snapshot.process_kind == GW_CHANNEL_PROCESS_NONE);
    CHECK(snapshot.process_pid == -1);
    CHECK(!snapshot.has_probe);
    CHECK(!snapshot.has_progress);
    CHECK(!snapshot.has_exit_code);

    runtime.state = GW_CHANNEL_BACKOFF;
    runtime.consecutive_failures = 2U;
    runtime.total_restarts = 4U;
    runtime.backoff_sec = 8;
    gw_channel_snapshot_update_runtime(&snapshot, &runtime, "worker_failure");
    CHECK(snapshot.state == GW_CHANNEL_BACKOFF);
    CHECK(snapshot.consecutive_failures == 2U);
    CHECK(snapshot.total_restarts == 4U);
    CHECK(snapshot.backoff_sec == 8);
    CHECK(strcmp(snapshot.last_event, "worker_failure") == 0);

    gw_channel_snapshot_set_process(&snapshot, GW_CHANNEL_PROCESS_PROBE, 42,
                                    "probe_started");
    CHECK(snapshot.process_kind == GW_CHANNEL_PROCESS_PROBE);
    CHECK(snapshot.process_pid == 42);
    CHECK(!snapshot.has_exit_code);

    snprintf(probe.codec_name, sizeof(probe.codec_name), "%s", "h264");
    probe.width = 1920;
    probe.height = 1080;
    gw_channel_snapshot_set_probe(&snapshot, &probe);
    CHECK(snapshot.has_probe);
    CHECK(strcmp(snapshot.probe.codec_name, "h264") == 0);

    progress.frame = 125U;
    progress.fps = 25.0;
    snprintf(progress.status, sizeof(progress.status), "%s", "continue");
    gw_channel_snapshot_set_progress(&snapshot, &progress, "progress");
    CHECK(snapshot.has_progress);
    CHECK(snapshot.progress.frame == 125U);
    CHECK(strcmp(snapshot.last_event, "progress") == 0);

    gw_channel_snapshot_clear_process(&snapshot, 143, "probe_timeout");
    CHECK(snapshot.process_kind == GW_CHANNEL_PROCESS_NONE);
    CHECK(snapshot.process_pid == -1);
    CHECK(snapshot.has_exit_code);
    CHECK(snapshot.last_exit_code == 143);
    CHECK(strcmp(snapshot.last_event, "probe_timeout") == 0);
    CHECK(strcmp(gw_channel_process_kind_string(GW_CHANNEL_PROCESS_WORKER),
                 "WORKER") == 0);
}

int main(void)
{
    test_snapshot_updates();
    if (failures != 0) {
        fprintf(stderr, "%d snapshot test(s) failed.\n", failures);
        return 1;
    }
    printf("All channel snapshot tests passed.\n");
    return 0;
}
