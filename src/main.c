/* Command-line entry point, configuration selection, and signal publication. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/http_server.h"
#include "gateway/mediamtx_config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/supervisor.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The handler only publishes intent; supervisor cleanup runs in normal flow. */
static volatile sig_atomic_t stop_signal;
static volatile sig_atomic_t reload_requested;

static void handle_stop_signal(int signal_number)
{
    if (signal_number == SIGHUP) {
        reload_requested = 1;
    } else {
        stop_signal = signal_number;
    }
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 &&
           sigaction(SIGHUP, &action, NULL) == 0;
}

static bool recording_config_equal(const gw_recording_config *first,
                                   const gw_recording_config *second)
{
    return first->enabled == second->enabled &&
           strcmp(first->directory, second->directory) == 0 &&
           strcmp(first->format, second->format) == 0 &&
           first->part_duration_sec == second->part_duration_sec &&
           first->max_part_size_mb == second->max_part_size_mb &&
           first->segment_duration_sec == second->segment_duration_sec &&
           first->delete_after_sec == second->delete_after_sec &&
           first->min_free_mb == second->min_free_mb &&
           strcmp(first->playback_listen, second->playback_listen) == 0 &&
           first->playback_port == second->playback_port;
}

static void print_usage(const char *program)
{
    printf("Usage: %s --config PATH [--check-config | --dry-run] "
           "[--print-mediamtx-config] [--exit-when-idle] [--ffprobe-binary PATH] "
           "[--ffmpeg-binary PATH]\n",
           program);
}

static int print_mediamtx_config(const gw_config *config)
{
    char output[GW_MEDIAMTX_CONFIG_CAP];
    gw_error error = {0};
    gw_status status;

    status = gw_mediamtx_render_config(config, output, sizeof(output), &error);
    if (status != GW_OK) {
        fprintf(stderr, "MediaMTX configuration error (%s): %s\n",
                gw_status_string(status), error.message);
        return 1;
    }
    if (fputs(output, stdout) == EOF) {
        fprintf(stderr, "Cannot write MediaMTX configuration: %s\n",
                strerror(errno));
        return 1;
    }
    return 0;
}

static int run_dry_run(const gw_config *config, const char *ffmpeg_binary)
{
    gw_error error = {0};
    size_t index;

    /* Build the exact argv for each channel without creating child processes. */
    for (index = 0U; index < config->channel_count; ++index) {
        const gw_channel_config *channel = &config->channels[index];
        gw_pipeline_argv arguments;
        char command[8192];
        gw_status status;

        if (!channel->enabled) {
            printf("channel=%s disabled\n", channel->id);
            continue;
        }
        status = gw_pipeline_build(channel, &config->mediamtx, ffmpeg_binary,
                                   &arguments, &error);
        if (status == GW_OK) {
            status = gw_pipeline_render_redacted(&arguments, command,
                                                 sizeof(command), &error);
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

static int run_channels(const char *config_path, const gw_config *config,
                        gw_supervisor_options *options, bool exit_when_idle)
{
    const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 100000000L};
    gw_channel_manager *manager = NULL;
    gw_http_server *http_server = NULL;
    gw_error error = {0};
    gw_status status;
    int result;

    status = gw_channel_manager_create(&manager, config, options, &error);
    if (status != GW_OK) {
        fprintf(stderr, "Channel manager error (%s): %s\n",
                gw_status_string(status), error.message);
        return 1;
    }
    status = gw_channel_manager_start(manager, &error);
    if (status != GW_OK) {
        fprintf(stderr, "Channel manager start error (%s): %s\n",
                gw_status_string(status), error.message);
        gw_channel_manager_destroy(manager);
        return 1;
    }
    if (config->server.enabled) {
        status = gw_http_server_create(&http_server, &config->server, manager,
                                       &error);
        if (status == GW_OK) {
            status = gw_http_server_start(http_server, &error);
        }
        if (status != GW_OK) {
            fprintf(stderr, "HTTP server error (%s): %s\n",
                    gw_status_string(status), error.message);
            gw_http_server_destroy(http_server);
            gw_channel_manager_request_stop(manager, SIGTERM);
            gw_channel_manager_wait(manager);
            gw_channel_manager_destroy(manager);
            return 1;
        }
        printf("HTTP listening on %s:%u\n", config->server.listen,
               (unsigned int)gw_http_server_port(http_server));
    }
    for (;;) {
        bool finished = gw_channel_manager_is_finished(manager);

        if (stop_signal != 0) {
            gw_channel_manager_request_stop(manager, (int)stop_signal);
            if (finished) {
                break;
            }
        } else if (reload_requested != 0) {
            gw_channel_reload_summary summary;
            gw_config candidate;

            reload_requested = 0;
            status = gw_config_load_file(config_path, &candidate, &error);
            if (status != GW_OK) {
                fprintf(stderr, "Configuration reload rejected (%s): %s\n",
                        gw_status_string(status), error.message);
            } else if (candidate.server.enabled != config->server.enabled ||
                       strcmp(candidate.server.listen, config->server.listen) !=
                           0 ||
                       candidate.server.port != config->server.port) {
                fprintf(stderr,
                        "Configuration reload rejected: server.listen/port "
                        "changes require a process restart\n");
            } else if (!recording_config_equal(
                           &candidate.mediamtx.recording,
                           &config->mediamtx.recording)) {
                fprintf(stderr,
                        "Configuration reload rejected: MediaMTX recording "
                        "changes require regeneration and service reload\n");
            } else {
                status = gw_channel_manager_reload(manager, &candidate, &summary,
                                                   &error);
                if (status != GW_OK) {
                    fprintf(stderr, "Configuration reload failed (%s): %s\n",
                            gw_status_string(status), error.message);
                } else {
                    printf("Configuration reloaded: unchanged=%zu added=%zu "
                           "removed=%zu restarted=%zu\n",
                           summary.unchanged, summary.added, summary.removed,
                           summary.restarted);
                    fflush(stdout);
                }
            }
        } else if (exit_when_idle && finished) {
            break;
        }
        nanosleep(&poll_interval, NULL);
    }
    gw_http_server_stop(http_server);
    result = gw_channel_manager_wait(manager);
    gw_http_server_destroy(http_server);
    gw_channel_manager_destroy(manager);
    return result;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    bool dry_run = false;
    bool check_only = false;
    bool print_mediamtx = false;
    bool exit_when_idle = false;
    gw_supervisor_options options;
    gw_config config;
    gw_error error = {0};
    gw_status status;
    int argument;

    /* Preserve one-record-per-line diagnostics when stdout is redirected. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    gw_supervisor_options_init(&options);
    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--config") == 0 && argument + 1 < argc) {
            config_path = argv[++argument];
        } else if (strcmp(argv[argument], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[argument], "--check-config") == 0) {
            check_only = true;
        } else if (strcmp(argv[argument], "--print-mediamtx-config") == 0) {
            print_mediamtx = true;
        } else if (strcmp(argv[argument], "--exit-when-idle") == 0) {
            exit_when_idle = true;
        } else if (strcmp(argv[argument], "--ffprobe-binary") == 0 &&
                   argument + 1 < argc) {
            options.ffprobe_binary = argv[++argument];
        } else if (strcmp(argv[argument], "--ffmpeg-binary") == 0 &&
                   argument + 1 < argc) {
            options.ffmpeg_binary = argv[++argument];
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
    if (config_path == NULL || (dry_run ? 1 : 0) + (check_only ? 1 : 0) +
                                   (print_mediamtx ? 1 : 0) >
                                   1) {
        print_usage(argv[0]);
        return 2;
    }

    status = gw_config_load_file(config_path, &config, &error);
    if (status != GW_OK) {
        fprintf(stderr, "Configuration error (%s): %s\n",
                gw_status_string(status), error.message);
        return 1;
    }
    if (!print_mediamtx) {
        printf("Configuration valid: %zu channel(s)\n", config.channel_count);
        fflush(stdout);
    }
    if (check_only) {
        return 0;
    }
    if (dry_run) {
        return run_dry_run(&config, options.ffmpeg_binary);
    }
    if (print_mediamtx) {
        return print_mediamtx_config(&config);
    }

    if (!install_signal_handlers()) {
        fprintf(stderr, "Cannot install signal handlers: %s\n", strerror(errno));
        return 1;
    }
    return run_channels(config_path, &config, &options, exit_when_idle);
}
