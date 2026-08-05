#define _POSIX_C_SOURCE 200809L

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

static volatile sig_atomic_t stop_requested;

typedef struct {
    char data[STDERR_LINE_CAP];
    size_t length;
    bool dropping;
} stderr_line_buffer;

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

static int run_channel(const gw_config *config, const gw_channel_config *channel,
                       const char *ffmpeg_binary)
{
    gw_pipeline_argv arguments;
    gw_process process;
    gw_progress_parser parser;
    gw_worker_progress latest_progress = {0};
    gw_error error = {0};
    stderr_line_buffer stderr_lines = {0};
    char command[8192];
    bool received_progress = false;
    bool exited = false;
    bool stop_sent = false;
    int result = 1;
    gw_status status;

    status = gw_pipeline_build(channel, &config->mediamtx, ffmpeg_binary, &arguments,
                               &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s pipeline error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        return 1;
    }
    status = gw_pipeline_render_redacted(&arguments, command, sizeof(command), &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s command rendering error: %s\n", channel->id,
                error.message);
        gw_pipeline_argv_free(&arguments);
        return 1;
    }

    gw_process_init(&process);
    gw_progress_parser_init(&parser);
    status = gw_process_start(&process, arguments.items, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s start error (%s): %s\n", channel->id,
                gw_status_string(status), error.message);
        gw_pipeline_argv_free(&arguments);
        return 1;
    }
    printf("channel=%s pid=%ld state=STARTING command=%s\n", channel->id,
           (long)process.pid, command);

    while (!exited || process.stdout_fd >= 0 || process.stderr_fd >= 0) {
        struct pollfd descriptors[2];
        int poll_result;

        if (stop_requested != 0 && !stop_sent && !exited) {
            printf("channel=%s pid=%ld state=STOPPING signal=%d\n", channel->id,
                   (long)process.pid, (int)stop_requested);
            status = gw_process_stop(&process, stop_timeout_ms(config), &error);
            if (status != GW_OK) {
                fprintf(stderr, "channel=%s stop error: %s\n", channel->id,
                        error.message);
                break;
            }
            stop_sent = true;
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
            if (completed && !received_progress) {
                received_progress = true;
                printf("channel=%s pid=%ld state=RUNNING\n", channel->id,
                       (long)process.pid);
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
    }

    if (process.running) {
        status = gw_process_stop(&process, stop_timeout_ms(config), &error);
        if (status != GW_OK) {
            fprintf(stderr, "channel=%s cleanup error: %s\n", channel->id,
                    error.message);
        }
    }
    if (process.reaped) {
        result = gw_process_exit_code(&process);
        printf("channel=%s pid=%ld state=STOPPED exit_code=%d frame=%llu fps=%.2f "
               "bitrate=%s drop_frames=%llu speed=%.3f\n",
               channel->id, (long)process.pid, result,
               (unsigned long long)latest_progress.frame, latest_progress.fps,
               latest_progress.bitrate,
               (unsigned long long)latest_progress.drop_frames,
               latest_progress.speed);
    }
    status = gw_process_close(&process, &error);
    if (status != GW_OK) {
        fprintf(stderr, "channel=%s close error: %s\n", channel->id, error.message);
        result = 1;
    }
    gw_pipeline_argv_free(&arguments);
    return result;
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
        return run_channel(&config, enabled_channel, ffmpeg_binary);
    }
}
