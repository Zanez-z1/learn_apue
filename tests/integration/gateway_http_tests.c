#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char config_text[] =
    "server: {enabled: true, listen: 127.0.0.1, port: 0}\n"
    "mediamtx:\n"
    "  recording: {enabled: true, directory: /tmp, min_free_mb: 2147483647}\n"
    "defaults: {probe_timeout_sec: 1, startup_timeout_sec: 1, "
    "progress_timeout_sec: 5, stable_run_sec: 5, stop_timeout_sec: 1, "
    "max_retries: 1, max_backoff_sec: 1}\n"
    "channels:\n"
    "  - id: cam01\n"
    "    input: {type: rtsp, url: "
    "'rtsp://user:http-password@camera/hold', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, "
    "encoder: h264_rkmpp, bitrate_kbps: 4000, fps: 25}\n"
    "    output: {path: cam01}\n";

static int write_all(int descriptor, const char *data, size_t length)
{
    size_t written = 0U;

    while (written < length) {
        ssize_t result = write(descriptor, data + written, length - written);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static void pause_milliseconds(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000L,
        .tv_nsec = (milliseconds % 1000L) * 1000000L
    };

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

static ssize_t read_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY);
    ssize_t length;

    if (descriptor < 0) {
        return -1;
    }
    do {
        length = read(descriptor, buffer, capacity - 1U);
    } while (length < 0 && errno == EINTR);
    close(descriptor);
    if (length >= 0) {
        buffer[length] = '\0';
    }
    return length;
}

static int wait_for_http_port(const char *log_path, unsigned int *port)
{
    char log[131072];
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        char *line;

        if (read_file(log_path, log, sizeof(log)) >= 0) {
            line = strstr(log, "HTTP listening on 127.0.0.1:");
            if (line != NULL &&
                sscanf(line, "HTTP listening on 127.0.0.1:%u", port) == 1 &&
                *port > 0U && *port <= 65535U) {
                return 0;
            }
        }
        pause_milliseconds(50L);
    }
    return -1;
}

static int request_http(unsigned int port, const char *request, size_t length,
                        char *response, size_t capacity)
{
    struct sockaddr_in address;
    int descriptor;
    size_t used = 0U;

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(descriptor, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        write_all(descriptor, request, length) < 0) {
        close(descriptor);
        return -1;
    }
    shutdown(descriptor, SHUT_WR);
    while (used + 1U < capacity) {
        ssize_t result = read(descriptor, response + used, capacity - used - 1U);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            close(descriptor);
            return -1;
        }
        if (result == 0) {
            break;
        }
        used += (size_t)result;
    }
    close(descriptor);
    response[used] = '\0';
    return 0;
}

static int expect_response(unsigned int port, const char *request,
                           const char *status, const char *required)
{
    char response[131072];

    if (request_http(port, request, strlen(request), response,
                     sizeof(response)) < 0 ||
        strstr(response, status) == NULL || strstr(response, required) == NULL ||
        strstr(response, "Content-Type: application/json") == NULL ||
        strstr(response, "Content-Length:") == NULL ||
        strstr(response, "Connection: close") == NULL ||
        strstr(response, "X-Content-Type-Options: nosniff") == NULL ||
        strstr(response, "http-password") != NULL ||
        strstr(response, "rtsp://") != NULL) {
        fprintf(stderr, "unexpected HTTP response:\n%s\n", response);
        return -1;
    }
    return 0;
}

static int expect_html_response(unsigned int port, const char *request,
                                const char *required)
{
    char response[131072];

    if (request_http(port, request, strlen(request), response,
                     sizeof(response)) < 0 ||
        strstr(response, "HTTP/1.1 200 OK") == NULL ||
        strstr(response, "Content-Type: text/html; charset=utf-8") == NULL ||
        strstr(response, "Content-Security-Policy:") == NULL ||
        strstr(response, "frame-src http:") == NULL ||
        strstr(response, "connect-src 'self'") == NULL ||
        strstr(response, "object-src 'none'") == NULL ||
        strstr(response, "frame-ancestors 'none'") == NULL ||
        strstr(response, "Cache-Control: no-store") == NULL ||
        strstr(response, "Referrer-Policy: no-referrer") == NULL ||
        strstr(response, "X-Content-Type-Options: nosniff") == NULL ||
        strstr(response, required) == NULL ||
        strstr(response, "http-password") != NULL ||
        strstr(response, "rtsp://") != NULL) {
        fprintf(stderr, "unexpected HTML response:\n%s\n", response);
        return -1;
    }
    return 0;
}

static int wait_for_metrics(unsigned int port, long different_pid,
                            long *observed_pid)
{
    static const char request[] =
        "GET /v1/channels/cam01/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[131072];
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        char *pid_field;
        long pid = -1L;

        if (request_http(port, request, strlen(request), response,
                         sizeof(response)) == 0 &&
            strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, "\"state\":\"RUNNING\"") != NULL &&
            strstr(response, "\"input\":{\"status\":\"available\","
                             "\"codec\":\"h264\",\"width\":1920,"
                             "\"height\":1080}") != NULL &&
            strstr(response, "\"progress\":{\"status\":\"available\"") !=
                NULL &&
            strstr(response, "\"fps\":25.000") != NULL &&
            strstr(response, "\"bitrate\":\"4000kbits/s\"") != NULL &&
            strstr(response, "\"frames\":") != NULL &&
            strstr(response, "\"drop_frames\":0") != NULL &&
            strstr(response, "\"process_metrics\":{\"status\":\"available\"") !=
                NULL &&
            strstr(response, "\"cpu_percent\":null") == NULL &&
            strstr(response, "\"rss_kib\":null") == NULL &&
            strstr(response, "http-password") == NULL &&
            strstr(response, "rtsp://") == NULL) {
            pid_field = strstr(response, "\"ffmpeg_pid\":");
            if (pid_field != NULL &&
                sscanf(pid_field, "\"ffmpeg_pid\":%ld", &pid) == 1 &&
                (different_pid <= 0L || pid != different_pid)) {
                if (observed_pid != NULL) {
                    *observed_pid = pid;
                }
                return 0;
            }
        }
        pause_milliseconds(20L);
    }
    fprintf(stderr, "channel metrics did not become available\n");
    return -1;
}

static int wait_for_channel_state(unsigned int port, const char *state,
                                  long different_pid, long *observed_pid)
{
    static const char request[] =
        "GET /v1/channels/cam01 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[131072];
    char expected[64];
    int attempt;

    snprintf(expected, sizeof(expected), "\"state\":\"%s\"", state);
    for (attempt = 0; attempt < 100; ++attempt) {
        char *pid_field;
        long pid = -1L;

        if (request_http(port, request, strlen(request), response,
                         sizeof(response)) == 0 &&
            strstr(response, "HTTP/1.1 200 OK") != NULL &&
            strstr(response, expected) != NULL &&
            strstr(response, "http-password") == NULL &&
            strstr(response, "rtsp://") == NULL) {
            pid_field = strstr(response, "\"pid\":");
            if (pid_field != NULL && sscanf(pid_field, "\"pid\":%ld", &pid) == 1 &&
                (different_pid <= 0L || pid != different_pid)) {
                if (observed_pid != NULL) {
                    *observed_pid = pid;
                }
                return 0;
            }
        }
        pause_milliseconds(20L);
    }
    fprintf(stderr, "channel did not reach state %s with a new pid\n", state);
    return -1;
}

static bool process_has_socket_fd(long pid)
{
    char directory_path[64];
    DIR *directory;
    struct dirent *entry;
    bool found = false;

    snprintf(directory_path, sizeof(directory_path), "/proc/%ld/fd", pid);
    directory = opendir(directory_path);
    if (directory == NULL) {
        return true;
    }
    while ((entry = readdir(directory)) != NULL) {
        char link_path[128];
        char target[128];
        ssize_t length;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(link_path, sizeof(link_path), "%s/%s", directory_path,
                     entry->d_name) >= (int)sizeof(link_path)) {
            found = true;
            break;
        }
        length = readlink(link_path, target, sizeof(target) - 1U);
        if (length < 0) {
            continue;
        }
        target[length] = '\0';
        if (strncmp(target, "socket:[", strlen("socket:[")) == 0) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

int main(int argc, char **argv)
{
    char config_path[] = "/tmp/gateway-http-config-XXXXXX";
    char log_path[] = "/tmp/gateway-http-log-XXXXXX";
    char log[131072];
    char oversized[8192];
    char response[131072];
    char *arguments[8];
    posix_spawn_file_actions_t actions;
    pid_t child = -1;
    int config_fd = -1;
    int log_fd = -1;
    int status = 0;
    int result;
    unsigned int port = 0U;
    long initial_pid = -1L;
    long started_pid = -1L;
    long final_pid = -1L;
    bool actions_initialized = false;
    int exit_code = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GATEWAY PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    config_fd = mkstemp(config_path);
    log_fd = mkstemp(log_path);
    if (config_fd < 0 || log_fd < 0 ||
        write_all(config_fd, config_text, strlen(config_text)) < 0) {
        fprintf(stderr, "cannot prepare HTTP fixtures: %s\n", strerror(errno));
        goto cleanup;
    }
    arguments[0] = argv[1];
    arguments[1] = "--config";
    arguments[2] = config_path;
    arguments[3] = "--ffprobe-binary";
    arguments[4] = argv[2];
    arguments[5] = "--ffmpeg-binary";
    arguments[6] = argv[2];
    arguments[7] = NULL;
    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        goto cleanup;
    }
    actions_initialized = true;
    if (posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO) != 0) {
        goto cleanup;
    }
    result = posix_spawnp(&child, arguments[0], &actions, NULL, arguments, environ);
    if (result != 0) {
        child = -1;
        goto cleanup;
    }
    if (wait_for_http_port(log_path, &port) < 0) {
        if (read_file(log_path, log, sizeof(log)) >= 0 && log[0] != '\0') {
            fprintf(stderr, "gateway did not publish an HTTP port:\n%s", log);
        } else {
            fprintf(stderr, "gateway did not publish an HTTP port\n");
        }
        goto cleanup;
    }

    if (expect_response(port, "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"channel_count\":1") < 0 ||
        expect_response(port, "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"recording\":\"low_space\"") < 0 ||
        expect_response(port,
                        "GET /v1/recording HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"status\":\"low_space\"") < 0 ||
        expect_response(port,
                        "GET /v1/channels HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"id\":\"cam01\"") < 0 ||
        expect_response(port,
                        "GET /v1/channels/cam01 HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"configuration_generation\":1") <
            0 ||
        expect_html_response(port,
                             "GET /view/cam01?media_host=192.168.1.45 HTTP/1.1\r\n"
                             "Host: localhost\r\n\r\n",
                             "normalizeMediaHost") < 0 ||
        expect_response(port,
                        "GET /view/missing HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 404 Not Found", "\"error\":\"not_found\"") <
            0 ||
        expect_response(port,
                        "GET /v1/channels/missing HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 404 Not Found", "\"error\":\"not_found\"") <
            0 ||
        expect_response(port,
                        "POST /v1/channels/cam01 HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 405 Method Not Allowed", "Allow: GET") < 0) {
        goto cleanup;
    }

    if (wait_for_channel_state(port, "RUNNING", -1L, &initial_pid) < 0 ||
        wait_for_metrics(port, -1L, &initial_pid) < 0 ||
        expect_response(port,
                        "GET /v1/channels/cam01/stop HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 405 Method Not Allowed", "Allow: POST") < 0 ||
        expect_response(port,
                        "POST /v1/channels/missing/start HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 404 Not Found", "\"error\":\"not_found\"") <
            0 ||
        expect_response(port,
                        "POST /v1/channels/cam01/stop HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 202 Accepted", "\"action\":\"stop\"") < 0 ||
        wait_for_channel_state(port, "STOPPED", -1L, NULL) < 0 ||
        expect_response(port,
                        "GET /v1/channels/cam01/metrics HTTP/1.1\r\nHost: localhost\r\n\r\n",
                        "HTTP/1.1 200 OK", "\"rss_kib\":null") < 0 ||
        expect_response(port,
                        "POST /v1/channels/cam01/stop HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 409 Conflict", "\"error\":\"state_conflict\"") <
            0 ||
        expect_response(port,
                        "POST /v1/channels/cam01/start HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 202 Accepted", "\"action\":\"start\"") < 0 ||
        wait_for_channel_state(port, "RUNNING", initial_pid, &started_pid) < 0 ||
        wait_for_metrics(port, initial_pid, &started_pid) < 0 ||
        expect_response(port,
                        "POST /v1/channels/cam01/start HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 409 Conflict", "\"error\":\"state_conflict\"") <
            0 ||
        expect_response(port,
                        "POST /v1/channels/cam01/restart HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 202 Accepted", "\"action\":\"restart\"") < 0 ||
        wait_for_channel_state(port, "RUNNING", started_pid, &final_pid) < 0 ||
        wait_for_metrics(port, started_pid, &final_pid) < 0) {
        goto cleanup;
    }
    if (process_has_socket_fd(final_pid)) {
        fprintf(stderr, "worker inherited an HTTP socket descriptor\n");
        goto cleanup;
    }

    memset(oversized, 'A', sizeof(oversized));
    if (request_http(port, oversized, sizeof(oversized), response,
                     sizeof(response)) < 0 ||
        strstr(response, "HTTP/1.1 431 Request Header Fields Too Large") == NULL) {
        fprintf(stderr, "oversized request was not rejected:\n%s\n", response);
        goto cleanup;
    }

    if (kill(child, SIGTERM) < 0) {
        goto cleanup;
    }
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    child = -1;
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        goto cleanup;
    }
    errno = 0;
    if (final_pid <= 0L || kill((pid_t)final_pid, 0) == 0 || errno != ESRCH) {
        fprintf(stderr, "worker process remained after gateway shutdown\n");
        goto cleanup;
    }
    if (request_http(port, "GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                     strlen("GET /v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n"),
                     response, sizeof(response)) == 0) {
        fprintf(stderr, "HTTP listener remained after gateway shutdown\n");
        goto cleanup;
    }
    if (read_file(log_path, log, sizeof(log)) < 0 ||
        strstr(log, "http-password") != NULL) {
        fprintf(stderr, "plaintext password appeared in gateway log\n");
        goto cleanup;
    }
    exit_code = 0;

cleanup:
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, &status, 0);
    }
    if (actions_initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (config_fd >= 0) {
        close(config_fd);
    }
    if (log_fd >= 0) {
        close(log_fd);
    }
    unlink(config_path);
    unlink(log_path);
    return exit_code;
}
