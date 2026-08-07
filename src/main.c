/* Command-line entry point, configuration selection, and signalfd event loop. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/http_server.h"
#include "gateway/mediamtx_config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/supervisor.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

static int open_control_signal_fd(void)
{
    sigset_t signals;
    int result;

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    result = pthread_sigmask(SIG_BLOCK, &signals, NULL);
    if (result != 0) {
        errno = result;
        return -1;
    }
    return signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
}

static bool read_control_signals(int descriptor, int *requested_stop,
                                 bool *requested_reload)
{
    for (;;) {
        struct signalfd_siginfo information;
        ssize_t length = read(descriptor, &information, sizeof(information));

        if (length == (ssize_t)sizeof(information)) {
            if (information.ssi_signo == (uint32_t)SIGHUP) {
                *requested_reload = true;
            } else if (information.ssi_signo == (uint32_t)SIGINT ||
                       information.ssi_signo == (uint32_t)SIGTERM) {
                *requested_stop = (int)information.ssi_signo;
            }
            continue;
        }
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
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
                        gw_supervisor_options *options, bool exit_when_idle,
                        int signal_descriptor)
{
    const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 100000000L};
    gw_channel_manager *manager = NULL;
    gw_http_server *http_server = NULL;
    gw_error error = {0};
    gw_status status;
    int requested_stop = 0;
    bool requested_reload = false;
    bool signal_read_failed = false;
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
        status = gw_http_server_create(&http_server, &config->server,
                                       &config->mediamtx.recording, manager,
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

        if (!read_control_signals(signal_descriptor, &requested_stop,
                                  &requested_reload)) {
            fprintf(stderr, "Cannot read control signals: %s\n", strerror(errno));
            requested_stop = SIGTERM;
            signal_read_failed = true;
        }
        if (requested_stop != 0) {
            gw_channel_manager_request_stop(manager, requested_stop);
            if (finished) {
                break;
            }
        } else if (requested_reload) {
            gw_channel_reload_summary summary;
            gw_config candidate;

            requested_reload = false;
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
    if (signal_read_failed) {
        result = 1;
    }
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
    int signal_descriptor;
    int result;

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
            options.stop_on_clean_exit = true;
            options.exit_on_retry_exhaustion = true;
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

    signal_descriptor = open_control_signal_fd();
    if (signal_descriptor < 0) {
        fprintf(stderr, "Cannot open signal descriptor: %s\n", strerror(errno));
        return 1;
    }
    result = run_channels(config_path, &config, &options, exit_when_idle,
                          signal_descriptor);
    close(signal_descriptor);
    return result;
}
