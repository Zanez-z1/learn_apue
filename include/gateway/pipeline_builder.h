#ifndef GATEWAY_PIPELINE_BUILDER_H
#define GATEWAY_PIPELINE_BUILDER_H

#include "gateway/config.h"

#include <stddef.h>

#define GW_PIPELINE_MAX_ARGS 40U

typedef struct {
    char *items[GW_PIPELINE_MAX_ARGS + 1U];
    size_t count;
} gw_pipeline_argv;

void gw_pipeline_argv_init(gw_pipeline_argv *arguments);
void gw_pipeline_argv_free(gw_pipeline_argv *arguments);
gw_status gw_pipeline_build(const gw_channel_config *channel,
                            const gw_mediamtx_config *mediamtx,
                            const char *ffmpeg_binary,
                            gw_pipeline_argv *arguments,
                            gw_error *error);
gw_status gw_pipeline_render_redacted(const gw_pipeline_argv *arguments,
                                      char *output, size_t output_size,
                                      gw_error *error);

#endif
