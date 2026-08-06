#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char initial_config[] =
    "server: {enabled: false}\n"
    "channels:\n"
    "  - id: cam01\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 4000, fps: 25}\n"
    "    output: {path: cam01}\n"
    "  - id: cam02\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold-two', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 4000, fps: 25}\n"
    "    output: {path: cam02}\n";

static const char changed_config[] =
    "server: {enabled: false}\n"
    "channels:\n"
    "  - id: cam01\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 4000, fps: 25}\n"
    "    output: {path: cam01}\n"
    "  - id: cam02\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold-two', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 5000, fps: 25}\n"
    "    output: {path: cam02}\n"
    "  - id: cam03\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold-three', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 4000, fps: 25}\n"
    "    output: {path: cam03}\n";

static const char invalid_config[] =
    "server: {enabled: false}\n"
    "channels:\n"
    "  - id: cam01\n"
    "    input: {type: rtsp, url: 'rtsp://user:reload-password@camera/hold', transport: tcp}\n"
    "    video: {decoder: h264_rkmpp, width: 1280, height: 720, encoder: h264_rkmpp, bitrate_kbps: 0, fps: 25}\n"
    "    output: {path: cam01}\n";

static int write_config(int descriptor, const char *content)
{
    size_t length = strlen(content);
    size_t written = 0U;

    if (ftruncate(descriptor, 0) < 0 || lseek(descriptor, 0, SEEK_SET) < 0) {
        return -1;
    }
    while (written < length) {
        ssize_t result = write(descriptor, content + written, length - written);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return fsync(descriptor);
}

static void pause_milliseconds(long milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

static size_t occurrence_count(const char *text, const char *needle)
{
    size_t count = 0U;
    size_t length = strlen(needle);
    const char *cursor = text;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        cursor += length;
    }
    return count;
}

static ssize_t read_log_file(const char *path, char *buffer, size_t capacity)
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

static int wait_for_log(const char *path, const char *needle)
{
    char buffer[131072];
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        if (read_log_file(path, buffer, sizeof(buffer)) >= 0 &&
            strstr(buffer, needle) != NULL) {
            return 0;
        }
        pause_milliseconds(50L);
    }
    return -1;
}

int main(int argc, char **argv)
{
    char config_path[] = "/tmp/gateway-reload-config-XXXXXX";
    char log_path[] = "/tmp/gateway-reload-log-XXXXXX";
    char log_buffer[131072];
    char *child_arguments[8];
    posix_spawn_file_actions_t actions;
    pid_t child = -1;
    int config_fd = -1;
    int log_fd = -1;
    int status = 0;
    int result;
    ssize_t log_length;
    bool actions_initialized = false;
    int exit_code = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GATEWAY PROCESS_FIXTURE\n", argv[0]);
        return 2;
    }
    config_fd = mkstemp(config_path);
    log_fd = mkstemp(log_path);
    if (config_fd < 0 || log_fd < 0 || write_config(config_fd, initial_config) < 0) {
        fprintf(stderr, "cannot prepare reload fixtures: %s\n", strerror(errno));
        goto cleanup;
    }

    child_arguments[0] = argv[1];
    child_arguments[1] = "--config";
    child_arguments[2] = config_path;
    child_arguments[3] = "--ffprobe-binary";
    child_arguments[4] = argv[2];
    child_arguments[5] = "--ffmpeg-binary";
    child_arguments[6] = argv[2];
    child_arguments[7] = NULL;
    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        fprintf(stderr, "cannot initialize spawn actions: %s\n", strerror(result));
        goto cleanup;
    }
    actions_initialized = true;
    result = posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO);
    if (result == 0) {
        result = posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
    }
    if (result != 0) {
        fprintf(stderr, "cannot redirect gateway output: %s\n", strerror(result));
        goto cleanup;
    }
    result = posix_spawnp(&child, child_arguments[0], &actions, NULL,
                          child_arguments, environ);
    if (result != 0) {
        fprintf(stderr, "cannot start gateway: %s\n", strerror(result));
        child = -1;
        goto cleanup;
    }

    if (wait_for_log(log_path, "Configuration valid: 2 channel(s)") < 0) {
        fprintf(stderr, "gateway did not report initial configuration\n");
        goto cleanup;
    }
    if (wait_for_log(log_path, "channel=cam01 pid=") < 0 ||
        wait_for_log(log_path, "channel=cam02 pid=") < 0) {
        fprintf(stderr, "initial channels did not start\n");
        goto cleanup;
    }
    if (write_config(config_fd, changed_config) < 0 || kill(child, SIGHUP) < 0) {
        fprintf(stderr, "cannot request valid reload: %s\n", strerror(errno));
        goto cleanup;
    }
    if (wait_for_log(
            log_path,
            "Configuration reloaded: unchanged=1 added=1 removed=0 restarted=1") <
        0) {
        fprintf(stderr, "gateway did not complete valid reload\n");
        goto cleanup;
    }
    if (wait_for_log(log_path, "channel=cam03 pid=") < 0) {
        fprintf(stderr, "added channel did not start\n");
        goto cleanup;
    }
    if (write_config(config_fd, invalid_config) < 0 || kill(child, SIGHUP) < 0) {
        fprintf(stderr, "cannot request invalid reload: %s\n", strerror(errno));
        goto cleanup;
    }
    if (wait_for_log(log_path, "Configuration reload rejected") < 0) {
        fprintf(stderr, "gateway did not reject invalid reload\n");
        goto cleanup;
    }
    if (kill(child, SIGTERM) < 0) {
        fprintf(stderr, "cannot stop gateway: %s\n", strerror(errno));
        goto cleanup;
    }
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    child = -1;
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "gateway did not stop cleanly\n");
        if (read_log_file(log_path, log_buffer, sizeof(log_buffer)) >= 0) {
            fprintf(stderr, "%s\n", log_buffer);
        }
        goto cleanup;
    }

    if (lseek(log_fd, 0, SEEK_SET) < 0) {
        goto cleanup;
    }
    log_length = read(log_fd, log_buffer, sizeof(log_buffer) - 1U);
    if (log_length < 0) {
        goto cleanup;
    }
    log_buffer[log_length] = '\0';
    if (strstr(log_buffer,
               "Configuration reloaded: unchanged=1 added=1 removed=0 restarted=1") ==
            NULL ||
        strstr(log_buffer, "Configuration reload rejected") == NULL ||
        occurrence_count(log_buffer,
                         "channel=cam01 state=PROBING restart_count=0") != 1U ||
        occurrence_count(log_buffer,
                         "channel=cam02 state=PROBING restart_count=0") != 2U ||
        occurrence_count(log_buffer,
                         "channel=cam03 state=PROBING restart_count=0") != 1U ||
        strstr(log_buffer, "reload-password") != NULL) {
        fprintf(stderr, "unexpected reload log:\n%s\n", log_buffer);
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
