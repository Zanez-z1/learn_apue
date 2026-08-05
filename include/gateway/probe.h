#ifndef GATEWAY_PROBE_H
#define GATEWAY_PROBE_H

/* Input stream discovery through a short-lived ffprobe child process. */

#include "gateway/config.h"

#include <stdbool.h>
#include <stddef.h>

#define GW_PROBE_MAX_ARGS 16U

typedef struct {
    char codec_name[GW_NAME_CAP];
    int width;
    int height;
} gw_probe_info;

typedef struct {
    char *items[GW_PROBE_MAX_ARGS + 1U];
    size_t count;
} gw_probe_argv;

void gw_probe_argv_init(gw_probe_argv *arguments);
void gw_probe_argv_free(gw_probe_argv *arguments);

gw_status gw_probe_build(const gw_channel_config *channel,
                         const char *ffprobe_binary,
                         gw_probe_argv *arguments,
                         gw_error *error);

gw_status gw_probe_parse(const char *output, gw_probe_info *info, gw_error *error);

bool gw_probe_matches_decoder(const gw_probe_info *info, const char *decoder);

#endif
