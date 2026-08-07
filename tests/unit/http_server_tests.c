#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/http_server.h"
#include "gateway/supervisor.h"

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
    gw_config route_config;
    gw_recording_config recording;
    gw_http_response response;
    gw_error error = {0};

    gw_config_init(&route_config);
    recording = route_config.mediamtx.recording;
    CHECK(manager != NULL);
    CHECK(gw_http_route(manager, &recording, "GET", "/v1/health", &response,
                        &error) ==
          GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"status\":\"ok\"") != NULL);
    CHECK(strstr(response.body, "\"channel_count\":2") != NULL);

    CHECK(gw_http_route(manager, &recording, "GET", "/v1/channels", &response,
                        &error) ==
          GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"id\":\"cam01\"") != NULL);
    CHECK(strstr(response.body, "\"id\":\"cam02\"") != NULL);
    CHECK(strstr(response.body, "unit-password") == NULL);
    CHECK(strstr(response.body, "rtsp://") == NULL);

    CHECK(gw_http_route(manager, &recording, "GET", "/v1/channels/cam02",
                        &response, &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"id\":\"cam02\"") != NULL);
    CHECK(strstr(response.body, "\"state\":\"STOPPED\"") != NULL);

    CHECK(gw_http_route(manager, &recording, "GET",
                        "/v1/channels/cam02/metrics", &response, &error) ==
          GW_OK);
    CHECK(response.status_code == 200);
    CHECK(response.content_type == GW_HTTP_CONTENT_JSON);
    CHECK(strstr(response.body, "\"ffmpeg_pid\":null") != NULL);
    CHECK(strstr(response.body, "\"status\":\"unavailable\"") != NULL);
    CHECK(strstr(response.body, "\"rss_kib\":null") != NULL);
    CHECK(strstr(response.body, "unit-password") == NULL);
    CHECK(strstr(response.body, "rtsp://") == NULL);

    CHECK(gw_http_route(manager, &recording, "GET", "/view/cam01", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(response.content_type == GW_HTTP_CONTENT_HTML);
    CHECK(strstr(response.body, "const pathParts = location.pathname") != NULL);
    CHECK(strstr(response.body, "MediaMTX WebRTC video") != NULL);
    CHECK(strstr(response.body, "setInterval(refresh, 1000)") != NULL);
    CHECK(strstr(response.body, "/metrics`") != NULL);
    CHECK(strstr(response.body, "textContent") != NULL);
    CHECK(strstr(response.body, "browser overlay") != NULL);
    CHECK(strstr(response.body, "normalizeMediaHost") != NULL);
    CHECK(strstr(response.body, "media_host") != NULL);
    CHECK(strstr(response.body, "location.hostname") == NULL);
    CHECK(strstr(response.body, "unit-password") == NULL);
    CHECK(strstr(response.body, "rtsp://") == NULL);

    CHECK(gw_http_route(manager, &recording, "GET",
                        "/view/cam01?media_host=192.168.1.45", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(response.content_type == GW_HTTP_CONTENT_HTML);
    CHECK(strstr(response.body, "192.168.1.45") == NULL);

    CHECK(gw_http_route(manager, &recording, "GET", "/v1/channels/missing",
                        &response, &error) == GW_OK);
    CHECK(response.status_code == 404);
    CHECK(strstr(response.body, "\"error\":\"not_found\"") != NULL);
    CHECK(gw_http_route(manager, &recording, "GET",
                        "/v1/channels/missing/metrics", &response, &error) ==
          GW_OK);
    CHECK(response.status_code == 404);
    CHECK(gw_http_route(manager, &recording, "GET", "/view/missing", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 404);
    CHECK(gw_http_route(manager, &recording, "GET",
                        "/view/cam01%22%3E%3Cscript%3E", &response, &error) ==
          GW_OK);
    CHECK(response.status_code == 404);
    CHECK(gw_http_route(manager, &recording, "POST", "/view/cam01", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);
    CHECK(gw_http_route(manager, &recording, "POST",
                        "/v1/channels/cam01/metrics", &response, &error) ==
          GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);

    CHECK(gw_http_route(manager, &recording, "POST", "/v1/channels/cam01",
                        &response, &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);
    CHECK(!response.allow_post);
    CHECK(gw_http_route(manager, &recording, "POST", "/v1/health", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);
    CHECK(gw_http_route(manager, &recording, "GET",
                        "/v1/channels/cam01/start", &response, &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(!response.allow_get);
    CHECK(response.allow_post);
    CHECK(gw_http_route(manager, &recording, "POST",
                        "/v1/channels/cam01/start", &response, &error) == GW_OK);
    CHECK(response.status_code == 409);
    CHECK(strstr(response.body, "\"error\":\"state_conflict\"") != NULL);
    CHECK(gw_http_route(manager, &recording, "POST",
                        "/v1/channels/missing/start", &response, &error) == GW_OK);
    CHECK(response.status_code == 409);
    CHECK(gw_http_route(manager, &recording, "GET",
                        "/v1/channels/cam01/extra", &response, &error) == GW_OK);
    CHECK(response.status_code == 404);

    recording.enabled = true;
    snprintf(recording.directory, sizeof(recording.directory), "%s", "/tmp");
    recording.min_free_mb = INT_MAX;
    CHECK(gw_http_route(manager, &recording, "GET", "/v1/recording", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 200);
    CHECK(strstr(response.body, "\"status\":\"low_space\"") != NULL);
    CHECK(strstr(response.body, "\"filesystem_available\":true") != NULL);
    CHECK(strstr(response.body, recording.directory) == NULL);
    CHECK(gw_http_route(manager, &recording, "POST", "/v1/recording", &response,
                        &error) == GW_OK);
    CHECK(response.status_code == 405);
    CHECK(response.allow_get);
    gw_channel_manager_destroy(manager);
}

static void test_static_page_and_json_escaping(void)
{
    gw_channel_snapshot snapshot;
    gw_channel_config channel;
    gw_http_response response;
    gw_error error = {0};

    CHECK(gw_http_render_view_page(&response, &error) == GW_OK);
    CHECK(response.content_type == GW_HTTP_CONTENT_HTML);
    CHECK(strstr(response.body, "data-channel=") == NULL);
    CHECK(strstr(response.body, "innerHTML") == NULL);
    CHECK(strstr(response.body, "textContent") != NULL);

    make_channel(&channel, "safe", "one");
    gw_channel_snapshot_init(&snapshot, &channel);
    snprintf(snapshot.channel_id, sizeof(snapshot.channel_id), "%s",
             "cam\"line\n");
    snapshot.state = GW_CHANNEL_RUNNING;
    snapshot.process_kind = GW_CHANNEL_PROCESS_WORKER;
    snapshot.process_pid = (pid_t)123;
    snapshot.has_probe = true;
    snprintf(snapshot.probe.codec_name, sizeof(snapshot.probe.codec_name), "%s",
             "h264\"codec");
    snapshot.probe.width = 1920;
    snapshot.probe.height = 1080;
    snapshot.has_progress = true;
    snapshot.progress.fps = 25.0;
    snprintf(snapshot.progress.bitrate, sizeof(snapshot.progress.bitrate), "%s",
             "4k\nbit");
    snapshot.worker_metrics.pid = (pid_t)123;
    snapshot.worker_metrics.available = true;
    snapshot.worker_metrics.cpu_available = true;
    snapshot.worker_metrics.cpu_percent = 12.5;
    snapshot.worker_metrics.rss_kib = 8192L;
    CHECK(gw_http_render_channel_metrics(&snapshot, &response, &error) == GW_OK);
    CHECK(response.content_type == GW_HTTP_CONTENT_JSON);
    CHECK(strstr(response.body, "\"id\":\"cam\\\"line\\n\"") != NULL);
    CHECK(strstr(response.body, "\"codec\":\"h264\\\"codec\"") != NULL);
    CHECK(strstr(response.body, "\"bitrate\":\"4k\\nbit\"") != NULL);
    CHECK(strstr(response.body, "\"cpu_percent\":12.500") != NULL);
    CHECK(strstr(response.body, "unit-password") == NULL);
    CHECK(strstr(response.body, "rtsp://") == NULL);

    snapshot.state = GW_CHANNEL_BACKOFF;
    snapshot.process_kind = GW_CHANNEL_PROCESS_NONE;
    snapshot.process_pid = (pid_t)-1;
    CHECK(gw_http_render_channel_metrics(&snapshot, &response, &error) == GW_OK);
    CHECK(strstr(response.body, "\"ffmpeg_pid\":null") != NULL);
    CHECK(strstr(response.body,
                 "\"input\":{\"status\":\"unavailable\",\"codec\":null") !=
          NULL);
    CHECK(strstr(response.body,
                 "\"progress\":{\"status\":\"unavailable\",\"fps\":null") !=
          NULL);
    CHECK(strstr(response.body, "\"cpu_percent\":null") != NULL);
    CHECK(strstr(response.body, "\"rss_kib\":null") != NULL);
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
    test_static_page_and_json_escaping();
    if (failures != 0) {
        fprintf(stderr, "%d HTTP API unit test(s) failed.\n", failures);
        return 1;
    }
    printf("All HTTP API unit tests passed.\n");
    return 0;
}
