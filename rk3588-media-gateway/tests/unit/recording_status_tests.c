#include "gateway/config.h"
#include "gateway/recording_status.h"

#include <limits.h>
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

static void test_disabled_and_available(void)
{
    gw_recording_config config;
    gw_recording_snapshot snapshot;
    gw_config gateway_config;
    gw_error error = {0};

    gw_config_init(&gateway_config);
    config = gateway_config.mediamtx.recording;
    CHECK(gw_recording_snapshot_read(&config, &snapshot, &error) == GW_OK);
    CHECK(snapshot.state == GW_RECORDING_DISABLED);
    CHECK(!snapshot.filesystem_available);

    config.enabled = true;
    snprintf(config.directory, sizeof(config.directory), "%s", "/tmp");
    config.min_free_mb = 0;
    CHECK(gw_recording_snapshot_read(&config, &snapshot, &error) == GW_OK);
    CHECK(snapshot.state == GW_RECORDING_OK);
    CHECK(snapshot.filesystem_available);
    CHECK(snapshot.total_bytes > 0U);
    CHECK(snapshot.available_bytes <= snapshot.total_bytes);

    config.min_free_mb = INT_MAX;
    CHECK(gw_recording_snapshot_read(&config, &snapshot, &error) == GW_OK);
    CHECK(snapshot.state == GW_RECORDING_LOW_SPACE);
}

static void test_unavailable_and_arguments(void)
{
    gw_recording_snapshot snapshot;
    gw_config config;
    gw_error error = {0};

    gw_config_init(&config);
    config.mediamtx.recording.enabled = true;
    snprintf(config.mediamtx.recording.directory,
             sizeof(config.mediamtx.recording.directory), "%s",
             "/tmp/gateway-recording-directory-that-does-not-exist");
    CHECK(gw_recording_snapshot_read(&config.mediamtx.recording, &snapshot,
                                     &error) == GW_OK);
    CHECK(snapshot.state == GW_RECORDING_UNAVAILABLE);
    CHECK(!snapshot.filesystem_available);
    CHECK(gw_recording_snapshot_read(NULL, &snapshot, &error) == GW_ERR_ARGUMENT);
}

int main(void)
{
    test_disabled_and_available();
    test_unavailable_and_arguments();
    if (failures != 0) {
        fprintf(stderr, "%d recording status test(s) failed.\n", failures);
        return 1;
    }
    printf("All recording status tests passed.\n");
    return 0;
}
