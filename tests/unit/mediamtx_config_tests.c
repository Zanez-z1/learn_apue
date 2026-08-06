#include "gateway/config.h"
#include "gateway/mediamtx_config.h"

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
                         const char *output_path, bool enabled)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->id, sizeof(channel->id), "%s", id);
    channel->enabled = enabled;
    snprintf(channel->input.type, sizeof(channel->input.type), "%s", "rtsp");
    snprintf(channel->input.url, sizeof(channel->input.url),
             "rtsp://fixture-user:fixture-password@camera/%s", id);
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
    snprintf(channel->output.path, sizeof(channel->output.path), "%s",
             output_path);
}

static void test_render_recording_config(void)
{
    char output[GW_MEDIAMTX_CONFIG_CAP];
    gw_config config;
    gw_error error = {0};

    gw_config_init(&config);
    config.mediamtx.recording.enabled = true;
    snprintf(config.mediamtx.recording.directory,
             sizeof(config.mediamtx.recording.directory), "%s",
             "/srv/gateway-recordings");
    config.mediamtx.recording.segment_duration_sec = 600;
    config.mediamtx.recording.delete_after_sec = 604800;
    config.channel_count = 2U;
    make_channel(&config.channels[0], "cam01", "front-door", true);
    make_channel(&config.channels[1], "cam02", "warehouse", false);

    CHECK(gw_mediamtx_render_config(&config, output, sizeof(output), &error) ==
          GW_OK);
    CHECK(strstr(output, "rtspTransports: [tcp]") != NULL);
    CHECK(strstr(output, "rtspAddress: '127.0.0.1:8554'") != NULL);
    CHECK(strstr(output, "rtmp: false") != NULL);
    CHECK(strstr(output, "hls: false") != NULL);
    CHECK(strstr(output, "webrtc: true") != NULL);
    CHECK(strstr(output, "srt: false") != NULL);
    CHECK(strstr(output, "moq: false") != NULL);
    CHECK(strstr(output, "playback: true") != NULL);
    CHECK(strstr(output, "playbackAddress: '127.0.0.1:9996'") != NULL);
    CHECK(strstr(output, "  front-door:\n    record: true") != NULL);
    CHECK(strstr(output, "  warehouse:\n    record: false") != NULL);
    CHECK(strstr(output,
                 "recordPath: '/srv/gateway-recordings/%path/%Y-%m-%d_%H-%M-%S-%f'") !=
          NULL);
    CHECK(strstr(output, "recordSegmentDuration: 600s") != NULL);
    CHECK(strstr(output, "recordDeleteAfter: 604800s") != NULL);
    CHECK(strstr(output, "fixture-password") == NULL);
    CHECK(strstr(output, "rtsp://") == NULL);
}

static void test_render_rejects_invalid_input(void)
{
    char output[64];
    gw_config config;
    gw_error error = {0};

    gw_config_init(&config);
    config.channel_count = 1U;
    make_channel(&config.channels[0], "cam01", "cam01", true);
    CHECK(gw_mediamtx_render_config(&config, output, sizeof(output), &error) ==
          GW_ERR_OVERFLOW);
    snprintf(config.mediamtx.recording.directory,
             sizeof(config.mediamtx.recording.directory), "%s", "relative/path");
    CHECK(gw_mediamtx_render_config(&config, output, sizeof(output), &error) ==
          GW_ERR_VALIDATION);
    snprintf(config.mediamtx.recording.directory,
             sizeof(config.mediamtx.recording.directory), "%s", "/safe/path");
    config.mediamtx.recording.delete_after_sec = -1;
    CHECK(gw_mediamtx_render_config(&config, output, sizeof(output), &error) ==
          GW_ERR_VALIDATION);
    CHECK(gw_mediamtx_render_config(NULL, output, sizeof(output), &error) ==
          GW_ERR_ARGUMENT);
}

int main(void)
{
    test_render_recording_config();
    test_render_rejects_invalid_input();
    if (failures != 0) {
        fprintf(stderr, "%d MediaMTX config test(s) failed.\n", failures);
        return 1;
    }
    printf("All MediaMTX config tests passed.\n");
    return 0;
}
