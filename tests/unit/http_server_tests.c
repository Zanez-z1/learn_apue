#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/http_server.h"
#include "gateway/supervisor.h"

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
                         const char *path)
{
    memset(channel, 0, sizeof(*channel));
    snprintf(channel->id, sizeof(channel->id), "%s", id);
    channel->enabled = true;
    snprintf(channel->input.type, sizeof(channel->input.type), "%s", "rtsp");
    snprintf(channel->input.url, sizeof(channel->input.url),
             "rtsp://user:unit-password@camera/%s", path);
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

static gw_channel_manager *make_manager(void)
{
    gw_channel_manager *manager = NULL;
    gw_supervisor_options options;
    gw_config config;
    gw_error error = {0};

    gw_config_init(&config);
    config.channel_count = 2U;
    make_channel(&config.channels[0], "cam01", "one");
    make_channel(&config.channels[1], "cam02", "two");
    gw_supervisor_options_init(&options);
    CHECK(gw_channel_manager_create(&manager, &config, &options, &error) == GW_OK);
    return manager;
}

static void test_read_only_routes(void)
{
    gw_channel_manager *manager = make_manager();
    gw_http_response response;
    gw_error error = {0};

    CHECK(manager != NULL);
    CHECK(gw_http_route_read_only(manager, "GET", "/v1/health", &response,
                                  &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"status\":\"ok\"") != NULL);
    CHECK(strstr(response.body, "\"channel_count\":2") != NULL);

    CHECK(gw_http_route_read_only(manager, "GET", "/v1/channels", &response,
                                  &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"id\":\"cam01\"") != NULL);
    CHECK(strstr(response.body, "\"id\":\"cam02\"") != NULL);
    CHECK(strstr(response.body, "unit-password") == NULL);
    CHECK(strstr(response.body, "rtsp://") == NULL);

    CHECK(gw_http_route_read_only(manager, "GET", "/v1/channels/cam02",
                                  &response, &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"id\":\"cam02\"") != NULL);
    CHECK(strstr(response.body, "\"state\":\"STOPPED\"") != NULL);

    CHECK(gw_http_route_read_only(manager, "GET", "/v1/channels/missing",
                                  &response, &error) == GW_OK);
    CHECK(response.status_code == 404);
    CHECK(strstr(response.body, "\"error\":\"not_found\"") != NULL);

    CHECK(gw_http_route_read_only(manager, "POST", "/v1/channels/cam01",
                                  &response, &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);
    CHECK(gw_http_route_read_only(manager, "GET", "/v1/channels/cam01/extra",
                                  &response, &error) == GW_OK);
    CHECK(response.status_code == 404);
    gw_channel_manager_destroy(manager);
}

static void test_snapshot_list_contract(void)
{
    gw_channel_manager *manager = make_manager();
    gw_channel_snapshot snapshots[2];
    gw_error error = {0};
    size_t count = 99U;

    CHECK(gw_channel_manager_list_snapshots(manager, snapshots, 2U, &count,
                                            &error) == GW_OK);
    CHECK(count == 2U);
    CHECK(gw_channel_manager_list_snapshots(manager, snapshots, 1U, &count,
                                            &error) == GW_ERR_OVERFLOW);
    CHECK(count == 0U);
    gw_channel_manager_destroy(manager);
}

int main(void)
{
    test_read_only_routes();
    test_snapshot_list_contract();
    if (failures != 0) {
        fprintf(stderr, "%d HTTP API unit test(s) failed.\n", failures);
        return 1;
    }
    printf("All HTTP API unit tests passed.\n");
    return 0;
}
