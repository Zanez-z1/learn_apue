#define _POSIX_C_SOURCE 200809L

#include "gateway/config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/progress_parser.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,               \
                          #condition);                                              \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void test_environment_expansion(void)
{
    char output[256];
    gw_error error = {0};

    CHECK(setenv("GW_TEST_HOST", "camera.local", 1) == 0);
    CHECK(gw_expand_environment("rtsp://${GW_TEST_HOST}/live", output,
                                sizeof(output), &error) == GW_OK);
    CHECK(strcmp(output, "rtsp://camera.local/live") == 0);
    CHECK(gw_expand_environment("${GW_VARIABLE_THAT_MUST_NOT_EXIST}", output,
                                sizeof(output), &error) == GW_ERR_ENV);
}

static void test_url_redaction(void)
{
    char output[256];

    CHECK(gw_redact_url("rtsp://alice:secret@camera.local/live", output,
                        sizeof(output)) == GW_OK);
    CHECK(strcmp(output, "rtsp://alice:***@camera.local/live") == 0);
    CHECK(strstr(output, "secret") == NULL);
    CHECK(gw_redact_url("rtsp://camera.local/live", output, sizeof(output)) == GW_OK);
    CHECK(strcmp(output, "rtsp://camera.local/live") == 0);
}

static void make_channel(gw_channel_config *channel)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->id, sizeof(channel->id), "%s", "cam01");
    channel->enabled = true;
    snprintf(channel->input.type, sizeof(channel->input.type), "%s", "rtsp");
    snprintf(channel->input.url, sizeof(channel->input.url), "%s",
             "rtsp://alice:secret@camera.local/live");
    snprintf(channel->input.transport, sizeof(channel->input.transport), "%s", "tcp");
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

static void test_validation(void)
{
    gw_config config;
    gw_error error = {0};

    gw_config_init(&config);
    config.channel_count = 1U;
    make_channel(&config.channels[0]);
    CHECK(gw_config_validate(&config, &error) == GW_OK);
    config.defaults.stable_run_sec = 0;
    CHECK(gw_config_validate(&config, &error) == GW_ERR_VALIDATION);
    config.defaults.stable_run_sec = 60;
    snprintf(config.channels[0].id, sizeof(config.channels[0].id), "%s", "bad/id");
    CHECK(gw_config_validate(&config, &error) == GW_ERR_VALIDATION);
}

static void test_config_loader(void)
{
    gw_config config;
    gw_error error = {0};
    char path[1024];

    CHECK(setenv("CAM01_RTSP_URL", "rtsp://user:password@camera/live", 1) == 0);
    snprintf(path, sizeof(path), "%s/config/gateway.example.yaml", GW_TEST_SOURCE_DIR);
    CHECK(gw_config_load_file(path, &config, &error) == GW_OK);
    CHECK(config.channel_count == 1U);
    CHECK(config.defaults.stable_run_sec == 60);
    CHECK(strcmp(config.channels[0].id, "cam01") == 0);
    CHECK(strcmp(config.channels[0].input.url,
                 "rtsp://user:password@camera/live") == 0);
}

static void test_pipeline(void)
{
    gw_channel_config channel;
    gw_mediamtx_config mediamtx;
    gw_pipeline_argv arguments;
    gw_error error = {0};
    char command[8192];

    make_channel(&channel);
    snprintf(mediamtx.publish_base_url, sizeof(mediamtx.publish_base_url), "%s",
             "rtsp://127.0.0.1:8554");
    CHECK(gw_pipeline_build(&channel, &mediamtx, "ffmpeg", &arguments, &error) ==
          GW_OK);
    CHECK(arguments.items[arguments.count] == NULL);
    CHECK(gw_pipeline_render_redacted(&arguments, command, sizeof(command), &error) ==
          GW_OK);
    CHECK(strstr(command, "scale_rkrga=w=1280:h=720:format=nv12") != NULL);
    CHECK(strstr(command, "rtsp://alice:***@camera.local/live") != NULL);
    CHECK(strstr(command, "secret") == NULL);
    CHECK(strstr(command, "rtsp://127.0.0.1:8554/cam01") != NULL);
    gw_pipeline_argv_free(&arguments);
}

static void test_progress_parser(void)
{
    static const char first[] = "frame=125\nfps=24.98\nbitrate=4012.3kbits/s\nout_";
    static const char second[] =
        "time_us=5000000\ndrop_frames=2\nspeed=0.999x\nprogress=continue\n";
    gw_progress_parser parser;
    gw_worker_progress progress;
    gw_error error = {0};
    bool completed = false;

    gw_progress_parser_init(&parser);
    CHECK(gw_progress_parser_consume(&parser, first, strlen(first), &progress,
                                     &completed, &error) == GW_OK);
    CHECK(!completed);
    CHECK(gw_progress_parser_consume(&parser, second, strlen(second), &progress,
                                     &completed, &error) == GW_OK);
    CHECK(completed);
    CHECK(progress.frame == 125U);
    CHECK(fabs(progress.fps - 24.98) < 0.001);
    CHECK(strcmp(progress.bitrate, "4012.3kbits/s") == 0);
    CHECK(progress.out_time_us == 5000000);
    CHECK(progress.drop_frames == 2U);
    CHECK(fabs(progress.speed - 0.999) < 0.001);
    CHECK(strcmp(progress.status, "continue") == 0);
}

int main(void)
{
    test_environment_expansion();
    test_url_redaction();
    test_validation();
    test_config_loader();
    test_pipeline();
    test_progress_parser();

    if (failures == 0) {
        printf("All gateway unit tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
