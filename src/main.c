/* Command-line entry point, configuration selection, and signal publication. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/pipeline_builder.h"
#include "gateway/supervisor.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The handler only publishes intent; supervisor cleanup runs in normal flow. */
static volatile sig_atomic_t stop_signal;

static void handle_stop_signal(int signal_number)
{
    stop_signal = signal_number;
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
           "[--ffprobe-binary PATH] [--ffmpeg-binary PATH]\n",
           program);
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

static int run_channels(const gw_config *config, gw_supervisor_options *options)
{
    gw_channel_manager *manager = NULL;
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
    result = gw_channel_manager_wait(manager);
    gw_channel_manager_destroy(manager);
    return result;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    bool dry_run = false;
    bool check_only = false;
    gw_supervisor_options options;
    gw_config config;
    gw_error error = {0};
    gw_status status;
    int argument;

    gw_supervisor_options_init(&options);
    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--config") == 0 && argument + 1 < argc) {
            config_path = argv[++argument];
        } else if (strcmp(argv[argument], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[argument], "--check-config") == 0) {
            check_only = true;
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
        return run_dry_run(&config, options.ffmpeg_binary);
    }

    if (!install_signal_handlers()) {
        fprintf(stderr, "Cannot install signal handlers: %s\n", strerror(errno));
        return 1;
    }
    options.stop_signal = &stop_signal;
    return run_channels(&config, &options);
}
