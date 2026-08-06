/* Single-channel probing, worker supervision, progress reporting, and retries. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_state.h"
#include "gateway/config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/probe.h"
#include "gateway/process_manager.h"
#include "gateway/progress_parser.h"
#include "gateway/supervisor.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define STDERR_LINE_CAP 4096U
#define PROBE_OUTPUT_CAP 1024U

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

typedef enum {
    PROBE_ATTEMPT_PENDING = 0,
    PROBE_ATTEMPT_SUCCEEDED,
    PROBE_ATTEMPT_FAILURE,
    PROBE_ATTEMPT_TIMEOUT,
    PROBE_ATTEMPT_MISMATCH,
    PROBE_ATTEMPT_STOP_REQUESTED,
    PROBE_ATTEMPT_INTERNAL_ERROR
} probe_attempt_outcome;

typedef struct {
    probe_attempt_outcome outcome;
    int exit_code;
    gw_probe_info info;
} probe_attempt_result;

static int stop_signal_value(const gw_supervisor_options *options)
{
    int requested;

    if (options == NULL) {
        return 0;
    }
    if (options->stop_check != NULL) {
        requested = options->stop_check(options->stop_context);
        if (requested != 0) {
            return requested;
        }
    }
    return options->stop_signal != NULL ? (int)*options->stop_signal : 0;
}

static void publish_snapshot(const gw_supervisor_options *options,
                             const gw_channel_snapshot *snapshot)
{
    if (options->observer != NULL) {
        options->observer(snapshot, options->observer_context);
    }
}

void gw_supervisor_options_init(gw_supervisor_options *options)
{
    if (options == NULL) {
        return;
    }
    options->ffprobe_binary = "ffprobe";
    options->ffmpeg_binary = "ffmpeg";
    options->stop_signal = NULL;
    options->stop_check = NULL;
    options->stop_context = NULL;
    options->stop_on_clean_exit = false;
    options->observer = NULL;
    options->observer_context = NULL;
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

static void emit_stderr_line(const char *channel_id, const char *component,
                             const char *source_url, const char *line)
{
    char redacted[STDERR_LINE_CAP];

    if (redact_source_url(line, source_url, redacted, sizeof(redacted))) {
        fprintf(stderr, "channel=%s %s_stderr=%s\n", channel_id, component,
                redacted);
    } else {
        fprintf(stderr, "channel=%s %s_stderr=[line omitted]\n", channel_id,
                component);
    }
}

static void consume_stderr(stderr_line_buffer *lines, const char *data,
                           size_t data_length, const gw_channel_config *channel,
                           const char *component)
{
    size_t index;

    for (index = 0U; index < data_length; ++index) {
        char byte = data[index];
        if (lines->dropping) {
            if (byte == '\n') {
                lines->dropping = false;
                fprintf(stderr, "channel=%s %s_stderr=[overlong line omitted]\n",
                        channel->id, component);
            }
            continue;
        }
        if (byte == '\n') {
            if (lines->length > 0U && lines->data[lines->length - 1U] == '\r') {
                --lines->length;
            }
            lines->data[lines->length] = '\0';
            emit_stderr_line(channel->id, component, channel->input.url,
                             lines->data);
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
                         const gw_channel_config *channel, const char *component)
{
    if (lines->dropping) {
        fprintf(stderr, "channel=%s %s_stderr=[overlong line omitted]\n",
                channel->id, component);
    } else if (lines->length > 0U) {
        lines->data[lines->length] = '\0';
        emit_stderr_line(channel->id, component, channel->input.url, lines->data);
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

static const char *probe_attempt_event_string(probe_attempt_outcome outcome)
{
    switch (outcome) {
    case PROBE_ATTEMPT_SUCCEEDED:
        return "probe_succeeded";
    case PROBE_ATTEMPT_FAILURE:
        return "probe_failure";
    case PROBE_ATTEMPT_TIMEOUT:
        return "probe_timeout";
    case PROBE_ATTEMPT_MISMATCH:
        return "probe_mismatch";
    case PROBE_ATTEMPT_STOP_REQUESTED:
        return "stop_requested";
    case PROBE_ATTEMPT_INTERNAL_ERROR:
        return "internal_error";
    case PROBE_ATTEMPT_PENDING:
        break;
    }
    return "unknown";
}

static probe_attempt_result run_probe_attempt(
    const gw_config *config, const gw_channel_config *channel,
    const gw_supervisor_options *options, gw_channel_snapshot *snapshot)
{
    probe_attempt_result attempt = {0};
    gw_probe_argv arguments;
    gw_process process;
    gw_error error = {0};
    stderr_line_buffer stderr_lines = {0};
    char output[PROBE_OUTPUT_CAP] = {0};
    size_t output_length = 0U;
    bool exited = false;
    struct timespec started_at;
    gw_status status;

    attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
    attempt.exit_code = -1;
    status = gw_probe_build(channel, options->ffprobe_binary, &arguments, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s probe command error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        return attempt;
    }

    gw_process_init(&process);
    status = gw_process_start(&process, arguments.items, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s probe start error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        gw_probe_argv_free(&arguments);
        attempt.outcome = PROBE_ATTEMPT_FAILURE;
        return attempt;
    }
    if (!monotonic_now(&started_at)) {
        fprintf(stderr, "channel=%s probe clock error: %s\n", channel->id,
                strerror(errno));
        gw_process_stop(&process, stop_timeout_ms(config), &error);
        gw_process_close(&process, &error);
        gw_probe_argv_free(&arguments);
        return attempt;
    }

    gw_channel_snapshot_set_process(snapshot, GW_CHANNEL_PROCESS_PROBE,
                                    process.pid, "probe_started");
    publish_snapshot(options, snapshot);
    attempt.outcome = PROBE_ATTEMPT_PENDING;
    printf("channel=%s pid=%ld state=PROBING event=probe_started\n", channel->id,
           (long)process.pid);
    while (!exited || process.stdout_fd >= 0 || process.stderr_fd >= 0) {
        struct pollfd descriptors[2];
        int poll_result;

        if (stop_signal_value(options) != 0 &&
            attempt.outcome == PROBE_ATTEMPT_PENDING && !exited) {
            status = gw_process_stop(&process, stop_timeout_ms(config), &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s probe stop error: %s\n", channel->id,
                        error.message);
                attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
                break;
            }
            attempt.outcome = PROBE_ATTEMPT_STOP_REQUESTED;
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
            fprintf(stderr, "channel=%s probe poll error: %s\n", channel->id,
                    strerror(errno));
            attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
            break;
        }

        if (descriptors[0].revents != 0) {
            char buffer[256];
            size_t bytes_read;
            bool end_of_stream;

            status = gw_process_read(&process, GW_PROCESS_STDOUT, buffer,
                                     sizeof(buffer), &bytes_read, &end_of_stream,
                                     &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s probe stdout error: %s\n", channel->id,
                        error.message);
                attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
                break;
            }
            if (memchr(buffer, '\0', bytes_read) != NULL ||
                !append_text(output, sizeof(output), &output_length, buffer,
                             bytes_read)) {
                fprintf(stderr, "channel=%s probe output is invalid or too large\n",
                        channel->id);
                attempt.outcome = PROBE_ATTEMPT_FAILURE;
                break;
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
                fprintf(stderr, "channel=%s probe stderr error: %s\n", channel->id,
                        error.message);
                attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
                break;
            }
            consume_stderr(&stderr_lines, buffer, bytes_read, channel, "probe");
            if (end_of_stream) {
                flush_stderr(&stderr_lines, channel, "probe");
            }
        }

        if (!exited) {
            status = gw_process_poll_exit(&process, &exited, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s probe wait error: %s\n", channel->id,
                        error.message);
                attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
                break;
            }
        }
        if (!exited && attempt.outcome == PROBE_ATTEMPT_PENDING &&
            timeout_elapsed(&started_at, config->defaults.probe_timeout_sec)) {
            status = gw_process_stop(&process, stop_timeout_ms(config), &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s probe timeout cleanup error: %s\n",
                        channel->id, error.message);
                attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
                break;
            }
            attempt.outcome = PROBE_ATTEMPT_TIMEOUT;
            exited = true;
        }
    }

    if (process.running) {
        status = gw_process_stop(&process, stop_timeout_ms(config), &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s probe cleanup error: %s\n", channel->id,
                    error.message);
            attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
        }
    }
    if (process.reaped) {
        attempt.exit_code = gw_process_exit_code(&process);
        if (attempt.outcome == PROBE_ATTEMPT_PENDING) {
            if (attempt.exit_code != 0) {
                attempt.outcome = PROBE_ATTEMPT_FAILURE;
            } else {
                status = gw_probe_parse(output, &attempt.info, &error);
                if (status != GW_OK) {
                    fprintf(stderr, "channel=%s probe parse error (%s): %s\n",
                            channel->id, gw_status_string(status), error.message);
                    attempt.outcome = PROBE_ATTEMPT_FAILURE;
                } else if (!gw_probe_matches_decoder(&attempt.info,
                                                     channel->video.decoder)) {
                    attempt.outcome = PROBE_ATTEMPT_MISMATCH;
                } else {
                    attempt.outcome = PROBE_ATTEMPT_SUCCEEDED;
                }
            }
        }
    }
    status = gw_process_close(&process, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s probe close error: %s\n", channel->id,
                error.message);
        attempt.outcome = PROBE_ATTEMPT_INTERNAL_ERROR;
    }
    gw_probe_argv_free(&arguments);
    return attempt;
}

static worker_attempt_result run_worker_attempt(
    const gw_config *config, const gw_channel_config *channel,
    const gw_supervisor_options *options, gw_channel_runtime *runtime,
    gw_channel_snapshot *snapshot)
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
    struct timespec running_since = {0};
    gw_status status;

    /* This function owns argv, child process, and pipes for exactly one attempt. */
    attempt.outcome = ATTEMPT_INTERNAL_ERROR;
    attempt.exit_code = -1;
    status = gw_pipeline_build(channel, &config->mediamtx, options->ffmpeg_binary,
                               &arguments, &error);
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
    gw_channel_snapshot_set_process(snapshot, GW_CHANNEL_PROCESS_WORKER,
                                    process.pid, "worker_started");
    publish_snapshot(options, snapshot);
    attempt.outcome = ATTEMPT_PENDING;
    printf("channel=%s pid=%ld state=%s command=%s\n", channel->id,
           (long)process.pid, gw_channel_state_string(runtime->state), command);

    /* Drain both pipes even after waitpid reports exit; buffered output may remain. */
    while (!exited || process.stdout_fd >= 0 || process.stderr_fd >= 0) {
        struct pollfd descriptors[2];
        int poll_result;

        if (stop_signal_value(options) != 0 &&
            attempt.outcome == ATTEMPT_PENDING && !exited) {
            printf("channel=%s pid=%ld state=STOPPING signal=%d\n", channel->id,
                   (long)process.pid, stop_signal_value(options));
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
                gw_channel_snapshot_update_runtime(snapshot, runtime, "progress");
                gw_channel_snapshot_set_progress(snapshot, &latest_progress,
                                                 "progress");
                publish_snapshot(options, snapshot);
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
            consume_stderr(&stderr_lines, buffer, bytes_read, channel, "worker");
            if (end_of_stream) {
                flush_stderr(&stderr_lines, channel, "worker");
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
            gw_channel_snapshot_update_runtime(snapshot, runtime, "stable");
            publish_snapshot(options, snapshot);
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
            attempt.outcome = attempt.exit_code == 0 && options->stop_on_clean_exit
                                  ? ATTEMPT_CLEAN_EXIT
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

static bool wait_for_backoff(int backoff_sec,
                             const gw_supervisor_options *options)
{
    struct timespec started_at;
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 100000000L};

    if (!monotonic_now(&started_at)) {
        return false;
    }
    /* Sleep in short intervals so service termination interrupts backoff promptly. */
    while (!timeout_elapsed(&started_at, backoff_sec)) {
        if (stop_signal_value(options) != 0) {
            return false;
        }
        nanosleep(&pause_time, NULL);
    }
    return true;
}

int gw_supervisor_run(const gw_config *config,
                      const gw_channel_config *channel,
                      const gw_supervisor_options *options)
{
    gw_channel_runtime runtime;
    gw_channel_snapshot snapshot;
    gw_error error = {0};
    gw_status status;

    if (config == NULL || channel == NULL || options == NULL ||
        options->ffprobe_binary == NULL || options->ffprobe_binary[0] == '\0' ||
        options->ffmpeg_binary == NULL || options->ffmpeg_binary[0] == '\0') {
        fprintf(stderr, "supervisor configuration and executable names are required\n");
        return 2;
    }

    gw_channel_runtime_init(&runtime, channel->enabled);
    gw_channel_snapshot_init(&snapshot, channel);
    status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START,
                                   &config->defaults, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s state error: %s\n", channel->id, error.message);
        return 1;
    }
    gw_channel_snapshot_update_runtime(&snapshot, &runtime, "start");
    publish_snapshot(options, &snapshot);

    /* Each loop iteration probes, starts one worker attempt, then stops or retries. */
    for (;;) {
        probe_attempt_result probe;
        worker_attempt_result attempt = {0};
        const char *failure_event;
        int failure_exit_code;

        printf("channel=%s state=%s restart_count=%llu\n", channel->id,
               gw_channel_state_string(runtime.state),
               (unsigned long long)runtime.total_restarts);
        probe = run_probe_attempt(config, channel, options, &snapshot);
        if (probe.info.codec_name[0] != '\0') {
            gw_channel_snapshot_set_probe(&snapshot, &probe.info);
        }
        gw_channel_snapshot_clear_process(
            &snapshot, probe.exit_code, probe_attempt_event_string(probe.outcome));
        publish_snapshot(options, &snapshot);
        if (probe.outcome == PROBE_ATTEMPT_STOP_REQUESTED) {
            status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP,
                                           &config->defaults, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                        error.message);
                return 1;
            }
            gw_channel_snapshot_update_runtime(&snapshot, &runtime,
                                               "stop_requested");
            publish_snapshot(options, &snapshot);
            printf("channel=%s state=%s event=stop_requested restart_count=%llu\n",
                   channel->id, gw_channel_state_string(runtime.state),
                   (unsigned long long)runtime.total_restarts);
            return 0;
        }
        if (probe.outcome == PROBE_ATTEMPT_SUCCEEDED) {
            printf("channel=%s state=PROBING event=probe_succeeded codec=%s "
                   "width=%d height=%d\n",
                   channel->id, probe.info.codec_name, probe.info.width,
                   probe.info.height);
            status = gw_channel_transition(&runtime,
                                           GW_CHANNEL_EVENT_PROBE_SUCCEEDED,
                                           &config->defaults, &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                        error.message);
                return 1;
            }
            gw_channel_snapshot_update_runtime(&snapshot, &runtime,
                                               "probe_succeeded");
            publish_snapshot(options, &snapshot);

            attempt = run_worker_attempt(config, channel, options, &runtime,
                                         &snapshot);
            gw_channel_snapshot_clear_process(
                &snapshot, attempt.exit_code,
                attempt_event_string(attempt.outcome));
            if (attempt.progress.status[0] != '\0') {
                gw_channel_snapshot_set_progress(
                    &snapshot, &attempt.progress,
                    attempt_event_string(attempt.outcome));
            }
            publish_snapshot(options, &snapshot);
            if (attempt.outcome == ATTEMPT_CLEAN_EXIT ||
                attempt.outcome == ATTEMPT_STOP_REQUESTED) {
                status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP,
                                               &config->defaults, &error);
                if (status != GW_OK) {
                    fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                            error.message);
                    return 1;
                }
                gw_channel_snapshot_update_runtime(
                    &snapshot, &runtime, attempt_event_string(attempt.outcome));
                publish_snapshot(options, &snapshot);
                printf("channel=%s state=%s event=%s exit_code=%d frame=%llu "
                       "fps=%.2f bitrate=%s drop_frames=%llu speed=%.3f "
                       "restart_count=%llu\n",
                       channel->id, gw_channel_state_string(runtime.state),
                       attempt_event_string(attempt.outcome), attempt.exit_code,
                       (unsigned long long)attempt.progress.frame,
                       attempt.progress.fps, attempt.progress.bitrate,
                       (unsigned long long)attempt.progress.drop_frames,
                       attempt.progress.speed,
                       (unsigned long long)runtime.total_restarts);
                return 0;
            }
            failure_event = attempt_event_string(attempt.outcome);
            failure_exit_code = attempt.exit_code;
        } else {
            failure_event = probe_attempt_event_string(probe.outcome);
            failure_exit_code = probe.exit_code;
            if (probe.outcome == PROBE_ATTEMPT_MISMATCH) {
                fprintf(stderr,
                        "channel=%s state=PROBING event=probe_mismatch codec=%s "
                        "decoder=%s\n",
                        channel->id, probe.info.codec_name,
                        channel->video.decoder);
            }
        }

        /* All non-clean outcomes consume retry budget through the state machine. */
        status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE,
                                       &config->defaults, &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s state error: %s\n", channel->id,
                    error.message);
            return 1;
        }
        gw_channel_snapshot_update_runtime(&snapshot, &runtime, failure_event);
        publish_snapshot(options, &snapshot);
        if (runtime.state == GW_CHANNEL_FAILED) {
            fprintf(stderr,
                    "channel=%s state=%s event=%s exit_code=%d failures=%u "
                    "restart_count=%llu\n",
                    channel->id, gw_channel_state_string(runtime.state),
                    failure_event, failure_exit_code, runtime.consecutive_failures,
                    (unsigned long long)runtime.total_restarts);
            return 1;
        }

        fprintf(stderr,
                "channel=%s state=%s event=%s exit_code=%d retry_in=%ds "
                "failures=%u\n",
                channel->id, gw_channel_state_string(runtime.state),
                failure_event, failure_exit_code, runtime.backoff_sec,
                runtime.consecutive_failures);
        if (!wait_for_backoff(runtime.backoff_sec, options)) {
            status = gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP,
                                           &config->defaults, &error);
            if (status == GW_OK) {
                gw_channel_snapshot_update_runtime(&snapshot, &runtime,
                                                   "stop_requested");
                publish_snapshot(options, &snapshot);
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
        gw_channel_snapshot_update_runtime(&snapshot, &runtime,
                                           "backoff_elapsed");
        publish_snapshot(options, &snapshot);
    }
}
