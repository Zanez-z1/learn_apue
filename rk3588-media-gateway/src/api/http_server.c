/* Bounded HTTP/1.x parsing, JSON serialization, and local socket service. */
#define _GNU_SOURCE

#include "gateway/http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_REQUEST_CAP 8192U
#define HTTP_TARGET_CAP 2048U

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool failed;
} json_writer;

struct gw_http_server {
    gw_server_config config;
    gw_recording_config recording;
    gw_channel_manager *manager;
    int listen_fd;
    uint16_t bound_port;
    pthread_t thread;
    bool thread_started;
    atomic_bool stop_requested;
};

static void set_error(gw_error *error, gw_status code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static void clear_error(gw_error *error)
{
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
}

static void json_append(json_writer *writer, const char *format, ...)
{
    va_list arguments;
    int written;
    size_t available;

    if (writer->failed) {
        return;
    }
    available = writer->capacity - writer->length;
    va_start(arguments, format);
    written = vsnprintf(writer->data + writer->length, available, format,
                        arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= available) {
        writer->failed = true;
        return;
    }
    writer->length += (size_t)written;
}

static void json_string(json_writer *writer, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    json_append(writer, "\"");
    while (!writer->failed && *cursor != '\0') {
        switch (*cursor) {
        case '"':
            json_append(writer, "\\\"");
            break;
        case '\\':
            json_append(writer, "\\\\");
            break;
        case '\b':
            json_append(writer, "\\b");
            break;
        case '\f':
            json_append(writer, "\\f");
            break;
        case '\n':
            json_append(writer, "\\n");
            break;
        case '\r':
            json_append(writer, "\\r");
            break;
        case '\t':
            json_append(writer, "\\t");
            break;
        default:
            if (*cursor < 0x20U || *cursor >= 0x80U) {
                json_append(writer, "\\u%04x", (unsigned int)*cursor);
            } else {
                json_append(writer, "%c", (int)*cursor);
            }
            break;
        }
        ++cursor;
    }
    json_append(writer, "\"");
}

static void json_snapshot(json_writer *writer,
                          const gw_channel_snapshot *snapshot)
{
    json_append(writer, "{\"id\":");
    json_string(writer, snapshot->channel_id);
    json_append(writer, ",\"state\":");
    json_string(writer, gw_channel_state_string(snapshot->state));
    json_append(writer, ",\"last_event\":");
    json_string(writer, snapshot->last_event);
    json_append(writer, ",\"process\":{\"kind\":");
    json_string(writer, gw_channel_process_kind_string(snapshot->process_kind));
    json_append(writer, ",\"pid\":%ld},\"consecutive_failures\":%u,"
                        "\"total_restarts\":%llu,"
                        "\"configuration_generation\":%llu,"
                        "\"backoff_sec\":%d,\"last_exit_code\":",
                (long)snapshot->process_pid, snapshot->consecutive_failures,
                (unsigned long long)snapshot->total_restarts,
                (unsigned long long)snapshot->configuration_generation,
                snapshot->backoff_sec);
    if (snapshot->has_exit_code) {
        json_append(writer, "%d", snapshot->last_exit_code);
    } else {
        json_append(writer, "null");
    }
    json_append(writer, ",\"probe\":");
    if (snapshot->has_probe) {
        json_append(writer, "{\"codec\":");
        json_string(writer, snapshot->probe.codec_name);
        json_append(writer, ",\"width\":%d,\"height\":%d}",
                    snapshot->probe.width, snapshot->probe.height);
    } else {
        json_append(writer, "null");
    }
    json_append(writer, ",\"progress\":");
    if (snapshot->has_progress) {
        json_append(writer, "{\"frame\":%llu,\"fps\":",
                    (unsigned long long)snapshot->progress.frame);
        if (isfinite(snapshot->progress.fps)) {
            json_append(writer, "%.3f", snapshot->progress.fps);
        } else {
            json_append(writer, "null");
        }
        json_append(writer, ",\"bitrate\":");
        json_string(writer, snapshot->progress.bitrate);
        json_append(writer,
                    ",\"out_time_us\":%lld,\"drop_frames\":%llu,"
                    "\"speed\":",
                    (long long)snapshot->progress.out_time_us,
                    (unsigned long long)snapshot->progress.drop_frames);
        if (isfinite(snapshot->progress.speed)) {
            json_append(writer, "%.3f", snapshot->progress.speed);
        } else {
            json_append(writer, "null");
        }
        json_append(writer, ",\"status\":");
        json_string(writer, snapshot->progress.status);
        json_append(writer, "}");
    } else {
        json_append(writer, "null");
    }
    json_append(writer, "}");
}

static gw_status finish_response(gw_http_response *response, json_writer *writer,
                                 gw_error *error)
{
    if (writer->failed) {
        set_error(error, GW_ERR_OVERFLOW, "HTTP JSON response exceeds capacity");
        return GW_ERR_OVERFLOW;
    }
    response->body_length = writer->length;
    clear_error(error);
    return GW_OK;
}

gw_status gw_http_render_channel_metrics(
    const gw_channel_snapshot *snapshot, gw_http_response *response,
    gw_error *error)
{
    json_writer writer;
    bool worker_active;

    if (snapshot == NULL || response == NULL) {
        set_error(error, GW_ERR_ARGUMENT,
                  "snapshot and HTTP response are required");
        return GW_ERR_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    response->status_code = 200;
    response->content_type = GW_HTTP_CONTENT_JSON;
    writer.data = response->body;
    writer.capacity = sizeof(response->body);
    writer.length = 0U;
    writer.failed = false;
    worker_active = snapshot->process_kind == GW_CHANNEL_PROCESS_WORKER &&
                    snapshot->process_pid > 0;

    json_append(&writer, "{\"id\":");
    json_string(&writer, snapshot->channel_id);
    json_append(&writer, ",\"state\":");
    json_string(&writer, gw_channel_state_string(snapshot->state));
    json_append(&writer, ",\"ffmpeg_pid\":");
    if (worker_active) {
        json_append(&writer, "%ld", (long)snapshot->process_pid);
    } else {
        json_append(&writer, "null");
    }
    json_append(&writer,
                ",\"consecutive_failures\":%u,\"total_restarts\":%llu,"
                "\"input\":{\"status\":",
                snapshot->consecutive_failures,
                (unsigned long long)snapshot->total_restarts);
    json_string(&writer, worker_active && snapshot->has_probe ? "available"
                                                             : "unavailable");
    json_append(&writer, ",\"codec\":");
    if (worker_active && snapshot->has_probe) {
        json_string(&writer, snapshot->probe.codec_name);
        json_append(&writer, ",\"width\":%d,\"height\":%d",
                    snapshot->probe.width, snapshot->probe.height);
    } else {
        json_append(&writer, "null,\"width\":null,\"height\":null");
    }
    json_append(&writer, "},\"progress\":{\"status\":");
    json_string(&writer, worker_active && snapshot->has_progress ? "available"
                                                                : "unavailable");
    if (worker_active && snapshot->has_progress) {
        json_append(&writer, ",\"fps\":");
        if (isfinite(snapshot->progress.fps)) {
            json_append(&writer, "%.3f", snapshot->progress.fps);
        } else {
            json_append(&writer, "null");
        }
        json_append(&writer, ",\"bitrate\":");
        json_string(&writer, snapshot->progress.bitrate);
        json_append(&writer, ",\"frames\":%llu,\"drop_frames\":%llu",
                    (unsigned long long)snapshot->progress.frame,
                    (unsigned long long)snapshot->progress.drop_frames);
    } else {
        json_append(&writer,
                    ",\"fps\":null,\"bitrate\":null,\"frames\":null,"
                    "\"drop_frames\":null");
    }
    json_append(&writer, "},\"process_metrics\":{\"status\":");
    json_string(&writer,
                worker_active && snapshot->worker_metrics.available
                    ? "available"
                    : "unavailable");
    json_append(&writer, ",\"cpu_percent\":");
    if (worker_active && snapshot->worker_metrics.available &&
        snapshot->worker_metrics.cpu_available &&
        isfinite(snapshot->worker_metrics.cpu_percent)) {
        json_append(&writer, "%.3f", snapshot->worker_metrics.cpu_percent);
    } else {
        json_append(&writer, "null");
    }
    if (worker_active && snapshot->worker_metrics.available) {
        json_append(&writer, ",\"rss_kib\":%ld",
                    snapshot->worker_metrics.rss_kib);
    } else {
        json_append(&writer, ",\"rss_kib\":null");
    }
    json_append(&writer, "}}\n");
    return finish_response(response, &writer, error);
}

static int load_page_file(const char *path, gw_http_response *response)
{
    size_t used = 0U;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);

    if (descriptor < 0) {
        return -1;
    }
    while (used + 1U < sizeof(response->body)) {
        ssize_t count = read(descriptor, response->body + used,
                             sizeof(response->body) - used - 1U);

        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR) {
            close(descriptor);
            return -1;
        }
    }
    if (used + 1U == sizeof(response->body)) {
        char extra;
        ssize_t count;

        do {
            count = read(descriptor, &extra, 1U);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            close(descriptor);
            return 1;
        }
    }
    if (close(descriptor) != 0) {
        return -1;
    }
    response->body[used] = '\0';
    response->body_length = used;
    return 0;
}

static bool build_executable_relative_path(char *path, size_t capacity,
                                           const char *suffix)
{
    char *separator;
    ssize_t length = readlink("/proc/self/exe", path, capacity - 1U);
    size_t directory_length;
    size_t suffix_length;

    if (suffix == NULL || length <= 0 || (size_t)length >= capacity) {
        return false;
    }
    path[length] = '\0';
    separator = strrchr(path, '/');
    if (separator == NULL) {
        return false;
    }
    directory_length = (size_t)(separator + 1 - path);
    suffix_length = strlen(suffix);
    if (directory_length + suffix_length + 1U > capacity) {
        return false;
    }
    memcpy(path + directory_length, suffix, suffix_length + 1U);
    return true;
}

gw_status gw_http_render_view_page(gw_http_response *response,
                                   gw_error *error)
{
    char sibling_path[PATH_MAX];
    int result = -1;

    if (response == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "HTTP response is required");
        return GW_ERR_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    if (build_executable_relative_path(sibling_path, sizeof(sibling_path),
                                       "web/diagnostic.html")) {
        result = load_page_file(sibling_path, response);
    }
    if (result < 0) {
        if (build_executable_relative_path(
                sibling_path, sizeof(sibling_path),
                "../share/rk-media-gateway/web/diagnostic.html")) {
            result = load_page_file(sibling_path, response);
        }
    }
    if (result > 0) {
        set_error(error, GW_ERR_OVERFLOW,
                  "diagnostic page exceeds HTTP response capacity");
        return GW_ERR_OVERFLOW;
    }
    if (result < 0) {
        set_error(error, GW_ERR_IO, "diagnostic page asset is unavailable");
        return GW_ERR_IO;
    }
    response->status_code = 200;
    response->content_type = GW_HTTP_CONTENT_HTML;
    clear_error(error);
    return GW_OK;
}

static gw_status error_response(gw_http_response *response, int status_code,
                                const char *code, const char *message,
                                gw_error *error)
{
    json_writer writer = {response->body, sizeof(response->body), 0U, false};

    response->status_code = status_code;
    json_append(&writer, "{\"error\":");
    json_string(&writer, code);
    json_append(&writer, ",\"message\":");
    json_string(&writer, message);
    json_append(&writer, "}\n");
    return finish_response(response, &writer, error);
}

static bool parse_action_target(const char *target, char *channel_id,
                                size_t channel_capacity, const char **action)
{
    const char *remainder;
    const char *separator;
    size_t id_length;

    if (strncmp(target, "/v1/channels/", 13U) != 0) {
        return false;
    }
    remainder = target + 13;
    separator = strchr(remainder, '/');
    if (separator == NULL || separator == remainder || separator[1] == '\0' ||
        strchr(separator + 1, '/') != NULL) {
        return false;
    }
    id_length = (size_t)(separator - remainder);
    if (id_length >= channel_capacity) {
        return false;
    }
    memcpy(channel_id, remainder, id_length);
    channel_id[id_length] = '\0';
    *action = separator + 1;
    return strcmp(*action, "start") == 0 || strcmp(*action, "stop") == 0 ||
           strcmp(*action, "restart") == 0;
}

static bool parse_channel_leaf(const char *target, const char *leaf,
                               char *channel_id, size_t channel_capacity)
{
    const char *remainder;
    const char *separator;
    size_t id_length;

    if (strncmp(target, "/v1/channels/", 13U) != 0) {
        return false;
    }
    remainder = target + 13;
    separator = strchr(remainder, '/');
    if (separator == NULL || separator == remainder ||
        strcmp(separator + 1, leaf) != 0) {
        return false;
    }
    id_length = (size_t)(separator - remainder);
    if (id_length >= channel_capacity) {
        return false;
    }
    memcpy(channel_id, remainder, id_length);
    channel_id[id_length] = '\0';
    return true;
}

static gw_status method_not_allowed(gw_http_response *response, bool allow_get,
                                    bool allow_post, gw_error *error)
{
    response->allow_get = allow_get;
    response->allow_post = allow_post;
    return error_response(response, 405, "method_not_allowed",
                          "method is not allowed for this route", error);
}

gw_status gw_http_route(gw_channel_manager *manager,
                        const gw_recording_config *recording,
                        const char *method, const char *target,
                        gw_http_response *response, gw_error *error)
{
    gw_channel_snapshot snapshots[GW_MAX_CHANNELS];
    gw_channel_snapshot snapshot;
    json_writer writer;
    size_t count = 0U;
    size_t index;
    gw_status status;
    char channel_id[GW_ID_CAP];
    char target_path[HTTP_TARGET_CAP];
    const char *action;
    const char *query;

    if (manager == NULL || recording == NULL || method == NULL || target == NULL ||
        response == NULL) {
        set_error(error, GW_ERR_ARGUMENT,
                  "manager, recording, method, target, and response are required");
        return GW_ERR_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    writer.data = response->body;
    writer.capacity = sizeof(response->body);
    writer.length = 0U;
    writer.failed = false;
    response->status_code = 200;

    query = strchr(target, '?');
    if (query != NULL) {
        size_t path_length = (size_t)(query - target);

        if (path_length == 0U || path_length >= sizeof(target_path)) {
            return error_response(response, 400, "bad_request",
                                  "request target is invalid", error);
        }
        memcpy(target_path, target, path_length);
        target_path[path_length] = '\0';
        target = target_path;
    }

    if (strcmp(target, "/v1/health") == 0) {
        size_t running = 0U;
        size_t failed = 0U;
        gw_recording_snapshot recording_snapshot;

        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_channel_manager_list_snapshots(
            manager, snapshots, GW_MAX_CHANNELS, &count, error);
        if (status != GW_OK) {
            return status;
        }
        status = gw_recording_snapshot_read(recording, &recording_snapshot,
                                            error);
        if (status != GW_OK) {
            return status;
        }
        for (index = 0U; index < count; ++index) {
            running += snapshots[index].state == GW_CHANNEL_RUNNING ? 1U : 0U;
            failed += snapshots[index].state == GW_CHANNEL_FAILED ? 1U : 0U;
        }
        json_append(&writer,
                    "{\"status\":\"%s\",\"channel_count\":%zu,"
                    "\"running\":%zu,\"failed\":%zu,\"recording\":",
                    failed == 0U && recording_snapshot.state !=
                                            GW_RECORDING_LOW_SPACE &&
                            recording_snapshot.state != GW_RECORDING_UNAVAILABLE
                        ? "ok"
                        : "degraded",
                    count, running, failed);
        json_string(&writer,
                    gw_recording_state_string(recording_snapshot.state));
        json_append(&writer, "}\n");
        return finish_response(response, &writer, error);
    }
    if (strcmp(target, "/v1/recording") == 0) {
        gw_recording_snapshot recording_snapshot;

        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_recording_snapshot_read(recording, &recording_snapshot,
                                            error);
        if (status != GW_OK) {
            return status;
        }
        json_append(&writer, "{\"enabled\":%s,\"status\":",
                    recording_snapshot.enabled ? "true" : "false");
        json_string(&writer,
                    gw_recording_state_string(recording_snapshot.state));
        json_append(&writer,
                    ",\"filesystem_available\":%s,\"total_bytes\":%llu,"
                    "\"available_bytes\":%llu,\"min_free_bytes\":%llu}\n",
                    recording_snapshot.filesystem_available ? "true" : "false",
                    (unsigned long long)recording_snapshot.total_bytes,
                    (unsigned long long)recording_snapshot.available_bytes,
                    (unsigned long long)recording_snapshot.min_free_bytes);
        return finish_response(response, &writer, error);
    }
    if (strcmp(target, "/v1/channels") == 0) {
        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_channel_manager_list_snapshots(
            manager, snapshots, GW_MAX_CHANNELS, &count, error);
        if (status != GW_OK) {
            return status;
        }
        json_append(&writer, "{\"channels\":[");
        for (index = 0U; index < count; ++index) {
            if (index != 0U) {
                json_append(&writer, ",");
            }
            json_snapshot(&writer, &snapshots[index]);
        }
        json_append(&writer, "]}\n");
        return finish_response(response, &writer, error);
    }
    if (strncmp(target, "/view/", 6U) == 0 && target[6] != '\0' &&
        strchr(target + 6, '/') == NULL) {
        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_channel_manager_get_snapshot(manager, target + 6, &snapshot,
                                                 error);
        if (status != GW_OK) {
            return error_response(response, 404, "not_found",
                                  "channel was not found", error);
        }
        return gw_http_render_view_page(response, error);
    }
    if (parse_channel_leaf(target, "metrics", channel_id,
                           sizeof(channel_id))) {
        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_channel_manager_get_snapshot(manager, channel_id, &snapshot,
                                                 error);
        if (status != GW_OK) {
            return error_response(response, 404, "not_found",
                                  "channel was not found", error);
        }
        return gw_http_render_channel_metrics(&snapshot, response, error);
    }
    if (strncmp(target, "/v1/channels/", 13U) == 0 && target[13] != '\0' &&
        strchr(target + 13, '/') == NULL) {
        if (strcmp(method, "GET") != 0) {
            return method_not_allowed(response, true, false, error);
        }
        status = gw_channel_manager_get_snapshot(manager, target + 13, &snapshot,
                                                 error);
        if (status != GW_OK) {
            return error_response(response, 404, "not_found",
                                  "channel was not found", error);
        }
        json_snapshot(&writer, &snapshot);
        json_append(&writer, "\n");
        return finish_response(response, &writer, error);
    }
    if (parse_action_target(target, channel_id, sizeof(channel_id), &action)) {
        if (strcmp(method, "POST") != 0) {
            return method_not_allowed(response, false, true, error);
        }
        if (strcmp(action, "start") == 0) {
            status = gw_channel_manager_start_channel(manager, channel_id, error);
        } else if (strcmp(action, "stop") == 0) {
            status = gw_channel_manager_stop_channel(manager, channel_id, error);
        } else {
            status = gw_channel_manager_restart_channel(manager, channel_id,
                                                        error);
        }
        if (status == GW_ERR_NOT_FOUND) {
            return error_response(response, 404, "not_found",
                                  "channel was not found", error);
        }
        if (status == GW_ERR_CONFLICT) {
            return error_response(response, 409, "state_conflict",
                                  "channel command conflicts with current state",
                                  error);
        }
        if (status != GW_OK) {
            return status;
        }
        response->status_code = 202;
        writer.length = 0U;
        writer.failed = false;
        json_append(&writer, "{\"status\":\"accepted\",\"channel_id\":");
        json_string(&writer, channel_id);
        json_append(&writer, ",\"action\":");
        json_string(&writer, action);
        json_append(&writer, "}\n");
        return finish_response(response, &writer, error);
    }
    return error_response(response, 404, "not_found", "route was not found",
                          error);
}

static const char *reason_phrase(int status_code)
{
    switch (status_code) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 431:
        return "Request Header Fields Too Large";
    default:
        return "Internal Server Error";
    }
}

static bool send_all(int descriptor, const char *data, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        ssize_t result;
#ifdef MSG_NOSIGNAL
        result = send(descriptor, data + sent, length - sent, MSG_NOSIGNAL);
#else
        result = send(descriptor, data + sent, length - sent, 0);
#endif
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

static void send_response(int descriptor, const gw_http_response *response)
{
    char headers[1024];
    const char *content_type = response->content_type == GW_HTTP_CONTENT_HTML
                                   ? "text/html; charset=utf-8"
                                   : "application/json";
    const char *content_security_policy =
        response->content_type == GW_HTTP_CONTENT_HTML
            ? "Content-Security-Policy: default-src 'none'; style-src "
              "'unsafe-inline'; script-src 'unsafe-inline'; frame-src http:; "
              "connect-src 'self'; base-uri 'none'; form-action 'none'; "
              "object-src 'none'; frame-ancestors 'none'\r\n"
            : "";
    int length;

    length = snprintf(headers, sizeof(headers),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "Cache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\n"
                      "Referrer-Policy: no-referrer\r\n%s%s%s\r\n",
                      response->status_code,
                      reason_phrase(response->status_code), content_type,
                      response->body_length, content_security_policy,
                      response->allow_get && response->allow_post
                          ? "Allow: GET, POST\r\n"
                          : response->allow_get ? "Allow: GET\r\n" : "",
                      !response->allow_get && response->allow_post
                          ? "Allow: POST\r\n"
                          : "");
    if (length <= 0 || (size_t)length >= sizeof(headers)) {
        return;
    }
    if (send_all(descriptor, headers, (size_t)length)) {
        send_all(descriptor, response->body, response->body_length);
    }
}

static void respond_error(int descriptor, int status_code, const char *code,
                          const char *message)
{
    gw_http_response response = {0};
    gw_error error = {0};

    if (error_response(&response, status_code, code, message, &error) == GW_OK) {
        send_response(descriptor, &response);
    }
}

static void handle_connection(gw_http_server *server, int descriptor)
{
    char request[HTTP_REQUEST_CAP + 1U];
    char method[8];
    char target[HTTP_TARGET_CAP];
    char version[16];
    char extra;
    char *line_end;
    size_t used = 0U;
    gw_http_response response;
    gw_error error = {0};
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};

    setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (used < HTTP_REQUEST_CAP) {
        ssize_t received = recv(descriptor, request + used,
                                HTTP_REQUEST_CAP - used, 0);

        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            respond_error(descriptor, 400, "bad_request",
                          "incomplete HTTP request");
            return;
        }
        used += (size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) {
            break;
        }
    }
    if (used == HTTP_REQUEST_CAP && strstr(request, "\r\n\r\n") == NULL) {
        respond_error(descriptor, 431, "headers_too_large",
                      "HTTP headers exceed the limit");
        return;
    }
    line_end = strstr(request, "\r\n");
    if (line_end == NULL) {
        respond_error(descriptor, 400, "bad_request", "invalid request line");
        return;
    }
    *line_end = '\0';
    if (sscanf(request, "%7s %2047s %15s %c", method, target, version, &extra) !=
            3 ||
        (strcmp(version, "HTTP/1.1") != 0 &&
         strcmp(version, "HTTP/1.0") != 0)) {
        respond_error(descriptor, 400, "bad_request", "invalid request line");
        return;
    }
    if (gw_http_route(server->manager, &server->recording, method, target,
                      &response, &error) != GW_OK) {
        respond_error(descriptor, 500, "internal_error",
                      "cannot build HTTP response");
        return;
    }
    send_response(descriptor, &response);
}

static void *run_server(void *context)
{
    gw_http_server *server = context;

    while (!atomic_load(&server->stop_requested)) {
        struct pollfd descriptor = {
            .fd = server->listen_fd,
            .events = POLLIN,
            .revents = 0
        };
        int result = poll(&descriptor, 1, 100);

        if (result < 0 && errno != EINTR) {
            break;
        }
        if (result > 0 && (descriptor.revents & POLLIN) != 0) {
            int client = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);

            if (client >= 0) {
                handle_connection(server, client);
                close(client);
            }
        }
    }
    return NULL;
}

gw_status gw_http_server_create(gw_http_server **server_output,
                                const gw_server_config *config,
                                const gw_recording_config *recording,
                                gw_channel_manager *manager, gw_error *error)
{
    gw_http_server *server;

    if (server_output == NULL || config == NULL || recording == NULL ||
        manager == NULL) {
        set_error(error, GW_ERR_ARGUMENT,
                  "server output, configuration, recording, and manager are required");
        return GW_ERR_ARGUMENT;
    }
    *server_output = NULL;
    server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        set_error(error, GW_ERR_NO_MEMORY, "cannot allocate HTTP server");
        return GW_ERR_NO_MEMORY;
    }
    server->config = *config;
    server->recording = *recording;
    server->manager = manager;
    server->listen_fd = -1;
    atomic_init(&server->stop_requested, false);
    *server_output = server;
    clear_error(error);
    return GW_OK;
}

static gw_status bind_listener(gw_http_server *server, gw_error *error)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char port[16];
    int result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    snprintf(port, sizeof(port), "%u", (unsigned int)server->config.port);
    result = getaddrinfo(server->config.listen, port, &hints, &addresses);
    if (result != 0) {
        set_error(error, GW_ERR_VALIDATION, "cannot resolve server.listen '%s': %s",
                  server->config.listen, gai_strerror(result));
        return GW_ERR_VALIDATION;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int option = 1;

        server->listen_fd = socket(address->ai_family,
                                   address->ai_socktype | SOCK_CLOEXEC,
                                   address->ai_protocol);
        if (server->listen_fd < 0) {
            continue;
        }
        setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &option,
                   sizeof(option));
        if (bind(server->listen_fd, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(server->listen_fd, 16) == 0) {
            break;
        }
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    freeaddrinfo(addresses);
    if (server->listen_fd < 0) {
        set_error(error, GW_ERR_IO, "cannot bind HTTP server on %s:%u: %s",
                  server->config.listen, (unsigned int)server->config.port,
                  strerror(errno));
        return GW_ERR_IO;
    }
    return GW_OK;
}

static gw_status discover_bound_port(gw_http_server *server, gw_error *error)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);

    if (getsockname(server->listen_fd, (struct sockaddr *)&address, &length) < 0) {
        set_error(error, GW_ERR_IO, "cannot read HTTP listener address: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    if (address.ss_family == AF_INET) {
        server->bound_port = ntohs(((struct sockaddr_in *)&address)->sin_port);
    } else if (address.ss_family == AF_INET6) {
        server->bound_port = ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    } else {
        set_error(error, GW_ERR_IO, "HTTP listener has an unsupported address family");
        return GW_ERR_IO;
    }
    return GW_OK;
}

gw_status gw_http_server_start(gw_http_server *server, gw_error *error)
{
    gw_status status;
    int result;

    if (server == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "HTTP server is required");
        return GW_ERR_ARGUMENT;
    }
    if (server->thread_started || server->listen_fd >= 0) {
        set_error(error, GW_ERR_VALIDATION, "HTTP server is already started");
        return GW_ERR_VALIDATION;
    }
    status = bind_listener(server, error);
    if (status != GW_OK) {
        return status;
    }
    status = discover_bound_port(server, error);
    if (status != GW_OK) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return status;
    }
    atomic_store(&server->stop_requested, false);
    result = pthread_create(&server->thread, NULL, run_server, server);
    if (result != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        set_error(error, GW_ERR_IO, "cannot start HTTP server thread: %s",
                  strerror(result));
        return GW_ERR_IO;
    }
    server->thread_started = true;
    clear_error(error);
    return GW_OK;
}

uint16_t gw_http_server_port(const gw_http_server *server)
{
    return server != NULL ? server->bound_port : 0U;
}

void gw_http_server_stop(gw_http_server *server)
{
    if (server == NULL) {
        return;
    }
    if (server->thread_started) {
        atomic_store(&server->stop_requested, true);
        pthread_join(server->thread, NULL);
        server->thread_started = false;
    }
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
}

void gw_http_server_destroy(gw_http_server *server)
{
    if (server == NULL) {
        return;
    }
    gw_http_server_stop(server);
    free(server);
}
