#include "gateway/probe.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void make_channel(gw_channel_config *channel)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->input.transport, sizeof(channel->input.transport), "%s", "tcp");
    snprintf(channel->input.url, sizeof(channel->input.url), "%s",
             "rtsp://user:password@camera/live");
}

static void test_argument_builder(void)
{
    gw_channel_config channel;
    gw_probe_argv arguments;
    gw_error error = {0};

    make_channel(&channel);
    CHECK(gw_probe_build(&channel, "custom-ffprobe", &arguments, &error) == GW_OK);
    CHECK(arguments.count == 12U);
    CHECK(arguments.items[arguments.count] == NULL);
    CHECK(strcmp(arguments.items[0], "custom-ffprobe") == 0);
    CHECK(strcmp(arguments.items[4], "tcp") == 0);
    CHECK(strcmp(arguments.items[11], channel.input.url) == 0);
    gw_probe_argv_free(&arguments);
}

static void test_parser_and_decoder_match(void)
{
    static const char output[] = "codec_name=h264\nwidth=1920\nheight=1080\n";
    gw_probe_info info;
    gw_error error = {0};

    CHECK(gw_probe_parse(output, &info, &error) == GW_OK);
    CHECK(strcmp(info.codec_name, "h264") == 0);
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK(gw_probe_matches_decoder(&info, "h264_rkmpp"));
    CHECK(!gw_probe_matches_decoder(&info, "hevc_rkmpp"));
    CHECK(gw_probe_parse("codec_name=hevc\r\nwidth=1280\r\nheight=720\r\n",
                         &info, &error) == GW_OK);
    CHECK(gw_probe_matches_decoder(&info, "hevc_rkmpp"));
}

static void test_invalid_output(void)
{
    gw_probe_info info;
    gw_error error = {0};

    CHECK(gw_probe_parse("codec_name=hevc\nwidth=1280\n", &info, &error) ==
          GW_ERR_PARSE);
    CHECK(gw_probe_parse("codec_name=hevc\nwidth=abc\nheight=720\n", &info,
                         &error) == GW_ERR_PARSE);
}

int main(void)
{
    test_argument_builder();
    test_parser_and_decoder_match();
    test_invalid_output();

    if (failures == 0) {
        printf("All probe tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d probe test(s) failed.\n", failures);
    return 1;
}
