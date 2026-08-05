/* Single-channel CLI, worker supervision loop, progress reporting, and retries. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_state.h"
#include "gateway/config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/process_manager.h"
#include "gateway/progress_parser.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STDERR_LINE_CAP 4096U

/* Signal handlers only publish intent; all cleanup remains in normal control flow. */
static volatile sig_atomic_t stop_requested;

typedef struct {
    char data[STDERR_LINE_CAP];
    size_t length;
    bool dropping;
} stderr_line_buffer;

typedef enum {
    /* One attempt ends with exactly one supervisor-relevant outcome. */
    ATTEMPT_PENDING = 0,
    ATTEMPT_CLEAN_EXIT,
    ATTEMPT_WORKER_FAILURE,
    ATTEMPT_STARTUP_TIMEOUT,
    ATTEMPT_PROGRESS_TIMEOUT,
    ATTEMPT_STOP_REQUESTED,
    ATTEMPT_INTERNAL_ERROR
} worker_attempt_outcome;

typedef struct {
    worker_attempt_outcome outcome;
    int exit_code;
    gw_worker_progress progress;
} worker_attempt_result;

static void handle_stop_signal(int signal_number)
{
    stop_requested = signal_number;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

static void print_usage(const char *program)
{
    printf("Usage: %s --config PATH [--check-config | --dry-run] "
           "[--ffmpeg-binary PATH]\n",
           program);
}

static bool append_text(char *output, size_t capacity, size_t *length,
                        const char *text, size_t text_length)
{
    if (*length + text_length >= capacity) {
        return false;
    }
    memcpy(output + *length, text, text_length);
    *length += text_length;
    output[*length] = '\0';
    return true;
}

static bool redact_source_url(const char *line, const char *source_url,
                              char *output, size_t capacity)
{
    char redacted_url[GW_URL_CAP];
    const char *cursor = line;
    const char *match;
    size_t output_length = 0U;
    size_t source_length = strlen(source_url);
    size_t redacted_length;

    if (gw_redact_url(source_url, redacted_url, sizeof(redacted_url)) != GW_OK) {
        return false;
    }
    redacted_length = strlen(redacted_url);
    output[0] = '\0';
    while ((match = strstr(cursor, source_url)) != NULL) {
        if (!append_text(output, capacity, &output_length, cursor,
                         (size_t)(match - cursor)) ||
            !append_text(output, capacity, &output_length, redacted_url,
                         redacted_length)) {
            return false;
        }
        cursor = match + source_length;
    }
    return append_text(output, capacity, &output_length, cursor, strlen(cursor));
}

static void emit_stderr_line(const char *channel_id, const char *source_url,
                             const char *line)
{
    char redacted[STDERR_LINE_CAP];

    if (redact_source_url(line, source_url, redacted, sizeof(redacted))) {
        fprintf(stderr, "channel=%s worker_stderr=%s\n", channel_id, redacted);
    } else {
        fprintf(stderr, "channel=%s worker_stderr=[line omitted]\n", channel_id);
    }
}

static void consume_stderr(stderr_line_buffer *lines, const char *data,
                           size_t data_length, const gw_channel_config *channel)
{
    size_t index;

    for (index = 0U; index < data_length; ++index) {
        char byte = data[index];
        if (lines->dropping) {
            if (byte == '\n') {
                lines->dropping = false;
                fprintf(stderr, "channel=%s worker_stderr=[overlong line omitted]\n",
                        channel->id);
            }
            continue;
        }
        if (byte == '\n') {
            if (lines->length > 0U && lines->data[lines->length - 1U] == '\r') {
                --lines->length;
            }
            lines->data[lines->length] = '\0';
            emit_stderr_line(channel->id, channel->input.url, lines->data);
            lines->length = 0U;
            continue;
        }
        if (lines->length + 1U >= sizeof(lines->data)) {
            lines->length = 0U;
            lines->dropping = true;
            continue;
        }
        lines->data[lines->length++] = byte;
    }
}

static void flush_stderr(stderr_line_buffer *lines,
                         const gw_channel_config *channel)
{
    if (lines->dropping) {
        fprintf(stderr, "channel=%s worker_stderr=[overlong line omitted]\n",
                channel->id);
    } else if (lines->length > 0U) {
        lines->data[lines->length] = '\0';
        emit_stderr_line(channel->id, channel->input.url, lines->data);
    }
    lines->length = 0U;
    lines->dropping = false;
}

static int stop_timeout_ms(const gw_config *config)
{
    if (config->defaults.stop_timeout_sec > INT_MAX / 1000) {
        return INT_MAX;
    }
    return config->defaults.stop_timeout_sec * 1000;
}

static bool monotonic_now(struct timespec *time_value)
{
    return clock_gettime(CLOCK_MONOTONIC, time_value) == 0;
}

static bool timeout_elapsed(const struct timespec *start, int timeout_sec)
{
    struct timespec now;
    time_t seconds;
    long nanoseconds;

    if (!monotonic_now(&now)) {
        return false;
    }
    seconds = now.tv_sec - start->tv_sec;
    nanoseconds = now.tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    return seconds > timeout_sec ||
           (seconds == timeout_sec && nanoseconds >= 0);
}

static const char *attempt_event_string(worker_attempt_outcome outcome)
{
    switch (outcome) {
    case ATTEMPT_CLEAN_EXIT:
        return "clean_exit";
    case ATTEMPT_WORKER_FAILURE:
        return "worker_failure";
    case ATTEMPT_STARTUP_TIMEOUT:
        return "startup_timeout";
    case ATTEMPT_PROGRESS_TIMEOUT:
        return "progress_timeout";
    case ATTEMPT_STOP_REQUESTED:
        return "stop_requested";
    case ATTEMPT_INTERNAL_ERROR:
        return "internal_error";
    case ATTEMPT_PENDING:
        break;
    }
    return "unknown";
}

static worker_attempt_result run_worker_attempt(
    const gw_config *config, const gw_channel_config *channel,
    const char *ffmpeg_binary, gw_channel_runtime *runtime)
{
    worker_attempt_result attempt = {0};
    gw_pipeline_argv arguments;
    gw_process process;
    gw_progress_parser parser;
    gw_worker_progress latest_progress = {0};
    gw_error error = {0};
    stderr_line_buffer stderr_lines = {0};
    char command[8192];
    bool received_progress = false;
    bool stable_reported = false;
    bool exited = false;
    struct timespec started_at;
    struct timespec last_progress_at;
    struct timespec running_since;
    gw_status status;

    /* This function owns argv, child process, and pipes for exactly one attempt. */
    attempt.outcome = ATTEMPT_INTERNAL_ERROR;
    attempt.exit_code = -1;
    status = gw_pipeline_build(channel, &config->mediamtx, ffmpeg_binary, &arguments,
                               &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s pipeline error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        return attempt;
    }
    status = gw_pipeline_render_redacted(&arguments, command, sizeof(command), &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s command rendering error: %s\n", channel->id,
                error.message);
        gw_pipeline_argv_free(&arguments);
        return attempt;
    }

    gw_process_init(&process);
    gw_progress_parser_init(&parser);
    status = gw_process_start(&process, arguments.items, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s start error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        gw_pipeline_argv_free(&arguments);
        attempt.outcome = ATTEMPT_WORKER_FAILURE;
        return attempt;
    }
    if (!monotonic_now(&started_at)) {
        fprintf(stderr, "channel=%s clock error: %s\n", channel->id,
                strerror(errno));
        gw_process_stop(&process, stop_timeout_ms(config), &error);
        gw_process_close(&process, &error);
        gw_pipeline_argv_free(&arguments);
        return attempt;
    }
    last_progress_at = started_at;
    attempt.outcome = ATTEMPT_PENDING;
    printf("channel=%s pid=%ld state=%s command=%s\n", channel->id,
           (long)process.pid, gw_channel_state_string(runtime->state), command);

    /* Drain both pipes even after waitpid reports exit; buffered output may remain. */
    while (!exited || process.stdout_fd >= 0 || process.stderr_fd >= 0) {
        struct pollfd descriptors[2];
        int poll_result;

        if (stop_requested != 0 && attempt.outcome == ATTEMPT_PENDING && !exited) {
            printf("channel=%s pid=%ld state=STOPPING signal=%d\n", channel->id,
                   (long)process.pid, (int)stop_requested);
            status = gw_process_stop(&process, stop_timeout_ms(config), &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s stop error: %s\n", channel->id,
                        error.message);
                break;
            }
            attempt.outcome = ATTEMPT_STOP_REQUESTED;
            exited = true;
        }

        descriptors[0].fd = process.stdout_fd;
        descriptors[0].events = POLLIN | POLLHUP;
        descriptors[0].revents = 0;
        descriptors[1].fd = process.stderr_fd;
        descriptors[1].events = POLLIN | POLLHUP;
        descriptors[1].revents = 0;
        poll_result = poll(descriptors, 2, 250);
        if (poll_result < 0 && errno != EINTR) {
            fprintf(stderr, "channel=%s poll error: %s\n", channel->id,
                    strerror(errno));
            break;
        }

        if (descriptors[0].revents != 0) {
            char buffer[2048];
            size_t bytes_read;
            bool end_of_stream;
            bool completed;

            status = gw_process_read(&process, GW_PROCESS_STDOUT, buffer,
                                     sizeof(buffer), &bytes_read, &end_of_stream,
                                     &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s stdout error: %s\n", channel->id,
                        error.message);
                break;
            }
            status = gw_progress_parser_consume(&parser, buffer, bytes_read,
                                                &latest_progress, &completed, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s progress error: %s\n", channel->id,
                        error.message);
                break;
            }
            if (completed) {
                if (!monotonic_now(&last_progress_at)) {
                    fprintf(stderr, "channel=%s clock error: %s\n", channel->id,
                            strerror(errno));
                    attempt.outcome = ATTEMPT_INTERNAL_ERROR;
                    break;
                }
                if (!received_progress) {
                    received_progress = true;
                    running_since = last_progress_at;
                    status = gw_channel_transition(runtime, GW_CHANNEL_EVENT_PROGRESS,
                                                   &config->defaults, &error);
                    if (status != GW_OK) {
                        fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                                error.message);
                        attempt.outcome = ATTEMPT_INTERNAL_ERROR;
                        break;
                    }
                    printf("channel=%s pid=%ld state=%s\n", channel->id,
                           (long)process.pid,
                           gw_channel_state_string(runtime->state));
                }
            }
        }
        if (descriptors[1].revents != 0) {
            char buffer[2048];
            size_t bytes_read;
            bool end_of_stream;

            status = gw_process_read(&process, GW_PROCESS_STDERR, buffer,
                                     sizeof(buffer), &bytes_read, &end_of_stream,
                                     &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s stderr error: %s\n", channel->id,
                        error.message);
                break;
            }
            consume_stderr(&stderr_lines, buffer, bytes_read, channel);
            if (end_of_stream) {
                flush_stderr(&stderr_lines, channel);
            }
        }

        if (!exited) {
            status = gw_process_poll_exit(&process, &exited, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s wait error: %s\n", channel->id,
                        error.message);
                break;
            }
        }

        if (!exited && received_progress && !stable_reported &&
            timeout_elapsed(&running_since, config->defaults.stable_run_sec)) {
            status = gw_channel_transition(runtime, GW_CHANNEL_EVENT_STABLE,
                                           &config->defaults, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                        error.message);
                attempt.outcome = ATTEMPT_INTERNAL_ERROR;
                break;
            }
            stable_reported = true;
            printf("channel=%s pid=%ld state=%s event=stable failures=%u\n",
                   channel->id, (long)process.pid,
                   gw_channel_state_string(runtime->state),
                   runtime->consecutive_failures);
        }

        if (!exited && attempt.outcome == ATTEMPT_PENDING) {
            worker_attempt_outcome timeout_outcome = ATTEMPT_PENDING;

            /* Startup waits for the first record; RUNNING requires recurring records. */
            if (!received_progress &&
                timeout_elapsed(&started_at,
                                config->defaults.startup_timeout_sec)) {
                timeout_outcome = ATTEMPT_STARTUP_TIMEOUT;
            } else if (received_progress &&
                       timeout_elapsed(&last_progress_at,
                                       config->defaults.progress_timeout_sec)) {
                timeout_outcome = ATTEMPT_PROGRESS_TIMEOUT;
            }
            if (timeout_outcome != ATTEMPT_PENDING) {
                fprintf(stderr, "channel=%s pid=%ld event=%s\n", channel->id,
                        (long)process.pid, attempt_event_string(timeout_outcome));
                status = gw_process_stop(&process, stop_timeout_ms(config), &error);
                if (status != GW_OK) {
                    fprintf(stderr, "channel=%s timeout cleanup error: %s\n",
                            channel->id, error.message);
                    attempt.outcome = ATTEMPT_INTERNAL_ERROR;
                    break;
                }
                attempt.outcome = timeout_outcome;
                exited = true;
            }
        }
    }

    /* Every exit path below the loop converges on process and argv cleanup. */
    if (process.running) {
        status = gw_process_stop(&process, stop_timeout_ms(config), &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s cleanup error: %s\n", channel->id,
                    error.message);
        }
    }
    if (process.reaped) {
        attempt.exit_code = gw_process_exit_code(&process);
        if (attempt.outcome == ATTEMPT_PENDING) {
            attempt.outcome = attempt.exit_code == 0 ? ATTEMPT_CLEAN_EXIT
                                                     : ATTEMPT_WORKER_FAILURE;
        }
    }
    status = gw_process_close(&process, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s close error: %s\n", channel->id, error.message);
        attempt.outcome = ATTEMPT_INTERNAL_ERROR;
    }
    gw_pipeline_argv_free(&arguments);
    attempt.progress = latest_progress;
    return attempt;
}

static bool wait_for_backoff(int backoff_sec)
{
    struct timespec started_at;
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 100000000L};

    if (!monotonic_now(&started_at)) {
        return false;
    }
    /* Sleep in short intervals so service termination interrupts backoff promptly. */
    while (!timeout_elapsed(&started_at, backoff_sec)) {
        if (stop_requested != 0) {
            return false;
        }
        nanosleep(&pause_time, NULL);
    }
    return true;
}

static int supervise_channel(const gw_config *config,
                             const gw_channel_config *channel,
                             const char *ffmpeg_binary)
{
    gw_channel_runtime runtime;
    gw_error error = {0};
    gw_status status;

    gw_channel_runtime_init(&runtime, channel->enabled);
    status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START,
                                   &config->defaults, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s state error: %s\n", channel->id, error.message);
        return 1;
    }

    /* Each loop iteration is one worker attempt followed by stop, fail, or retry. */
    for (;;) {
        worker_attempt_result attempt;

        printf("channel=%s state=%s restart_count=%llu\n", channel->id,
               gw_channel_state_string(runtime.state),
               (unsigned long long)runtime.total_restarts);
        status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_PROBE_SUCCEEDED,
                                       &config->defaults, &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                    error.message);
            return 1;
        }

        attempt = run_worker_attempt(config, channel, ffmpeg_binary, &runtime);
        if (attempt.outcome == ATTEMPT_CLEAN_EXIT ||
            attempt.outcome == ATTEMPT_STOP_REQUESTED) {
            status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP,
                                           &config->defaults, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                        error.message);
                return 1;
            }
            printf("channel=%s state=%s event=%s exit_code=%d frame=%llu fps=%.2f "
                   "bitrate=%s drop_frames=%llu speed=%.3f restart_count=%llu\n",
                   channel->id, gw_channel_state_string(runtime.state),
                   attempt_event_string(attempt.outcome), attempt.exit_code,
                   (unsigned long long)attempt.progress.frame, attempt.progress.fps,
                   attempt.progress.bitrate,
                   (unsigned long long)attempt.progress.drop_frames,
                   attempt.progress.speed,
                   (unsigned long long)runtime.total_restarts);
            return 0;
        }

        /* All non-clean outcomes consume retry budget through the state machine. */
        status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE,
                                       &config->defaults, &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                    error.message);
            return 1;
        }
        if (runtime.state == GW_CHANNEL_FAILED) {
            fprintf(stderr,
                    "channel=%s state=%s event=%s exit_code=%d failures=%u "
                    "restart_count=%llu\n",
                    channel->id, gw_channel_state_string(runtime.state),
                    attempt_event_string(attempt.outcome), attempt.exit_code,
                    runtime.consecutive_failures,
                    (unsigned long long)runtime.total_restarts);
            return 1;
        }

        fprintf(stderr,
                "channel=%s state=%s event=%s exit_code=%d retry_in=%ds "
                "failures=%u\n",
                channel->id, gw_channel_state_string(runtime.state),
                attempt_event_string(attempt.outcome), attempt.exit_code,
                runtime.backoff_sec, runtime.consecutive_failures);
        if (!wait_for_backoff(runtime.backoff_sec)) {
            status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP,
                                           &config->defaults, &error);
            if (status == GW_OK) {
                printf("channel=%s state=%s event=stop_requested\n", channel->id,
                       gw_channel_state_string(runtime.state));
                return 0;
            }
            fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                    error.message);
            return 1;
        }
        status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_BACKOFF_ELAPSED,
                                       &config->defaults, &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                    error.message);
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    bool dry_run = false;
    bool check_only = false;
    const char *ffmpeg_binary = "ffmpeg";
    gw_config config;
    gw_error error = {0};
    gw_status status;
    size_t index;
    int argument;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--config") == 0 && argument + 1 < argc) {
            config_path = argv[++argument];
        } else if (strcmp(argv[argument], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[argument], "--check-config") == 0) {
            check_only = true;
        } else if (strcmp(argv[argument], "--ffmpeg-binary") == 0 &&
                   argument + 1 < argc) {
            ffmpeg_binary = argv[++argument];
        } else if (strcmp(argv[argument], "--help") == 0 ||
                   strcmp(argv[argument], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[argument]);
            print_usage(argv[0]);
            return 2;
        }
    }
    if (config_path == NULL || (dry_run && check_only)) {
        print_usage(argv[0]);
        return 2;
    }

    /* Every mode loads and validates configuration before choosing its behavior. */
    status = gw_config_load_file(config_path, &config, &error);
    if (status != GW_OK) {
        fprintf(stderr, "Configuration error (%s): %s\n",
                gw_status_string(status), error.message);
        return 1;
    }
    printf("Configuration valid: %zu channel(s)\n", config.channel_count);
    if (check_only) {
        return 0;
    }
    if (dry_run) {
        /* Dry-run builds the exact argv but never creates a worker process. */
        for (index = 0U; index < config.channel_count; ++index) {
        const gw_channel_config *channel = &config.channels[index];
        gw_pipeline_argv arguments;
        char command[8192];

        if (!channel->enabled) {
            printf("channel=%s disabled\n", channel->id);
            continue;
        }
            status = gw_pipeline_build(channel, &config.mediamtx, ffmpeg_binary,
                                       &arguments, &error);
        if (status == GW_OK) {
            status = gw_pipeline_render_redacted(&arguments, command, sizeof(command),
                                                 &error);
        }
            if (status != GW_OK) {
            fprintf(stderr, "channel=%s pipeline error (%s): %s\n", channel->id,
                    gw_status_string(status), error.message);
            gw_pipeline_argv_free(&arguments);
            return 1;
        }
        printf("channel=%s command=%s\n", channel->id, command);
        gw_pipeline_argv_free(&arguments);
        }
        return 0;
    }

    {
        const gw_channel_config *enabled_channel = NULL;
        size_t enabled_count = 0U;

        for (index = 0U; index < config.channel_count; ++index) {
            if (config.channels[index].enabled) {
                enabled_channel = &config.channels[index];
                ++enabled_count;
            }
        }
        if (enabled_count == 0U) {
            fprintf(stderr, "No enabled channel is configured.\n");
            return 1;
        }
        /* The current supervisor intentionally owns one enabled channel only. */
        if (enabled_count > 1U) {
            fprintf(stderr,
                    "This milestone supports one enabled channel; configured=%zu.\n",
                    enabled_count);
            return 1;
        }
        if (!install_signal_handlers()) {
            fprintf(stderr, "Cannot install signal handlers: %s\n", strerror(errno));
            return 1;
        }
        return supervise_channel(&config, enabled_channel, ffmpeg_binary);
    }
}
