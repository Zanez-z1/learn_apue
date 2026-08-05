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
}

static void test_default_options(void)
{
    gw_supervisor_options options;

    gw_supervisor_options_init(&options);
    CHECK(strcmp(options.ffprobe_binary, "ffprobe") == 0);
    CHECK(strcmp(options.ffmpeg_binary, "ffmpeg") == 0);
    CHECK(options.stop_signal == NULL);
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
    options.ffprobe_binary = "";
    CHECK(gw_supervisor_run(&config, &channel, &options) == 2);
}

static void test_stop_during_probe(const char *fixture)
{
    volatile sig_atomic_t stop_signal = SIGTERM;
    gw_supervisor_options options;
    gw_config config;
    gw_channel_config channel;

    gw_supervisor_options_init(&options);
    options.ffprobe_binary = fixture;
    options.ffmpeg_binary = fixture;
    options.stop_signal = &stop_signal;
    gw_config_init(&config);
    config.defaults.stop_timeout_sec = 1;
    make_channel(&channel);
    CHECK(gw_supervisor_run(&config, &channel, &options) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    test_default_options();
    test_argument_validation();
    test_stop_during_probe(argv[1]);
    if (failures != 0) {
        fprintf(stderr, "%d supervisor test(s) failed.\n", failures);
        return 1;
    }
    printf("All supervisor tests passed.\n");
    return 0;
}
