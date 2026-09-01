#ifndef GATEWAY_PIPELINE_BUILDER_H
#define GATEWAY_PIPELINE_BUILDER_H

/* FFmpeg argument construction without shell command evaluation. */

#include "gateway/config.h"

#include <stddef.h>

#define GW_PIPELINE_MAX_ARGS 40U

typedef struct {
    /* Each populated item is heap-owned and released by gw_pipeline_argv_free(). */
    char *items[GW_PIPELINE_MAX_ARGS + 1U];
    size_t count;
} gw_pipeline_argv;

/* Initialize/free the owned, null-terminated argument vector. */
void gw_pipeline_argv_init(gw_pipeline_argv *arguments);
void gw_pipeline_argv_free(gw_pipeline_argv *arguments);

/* Build argv for one validated channel; this function does not start FFmpeg. */
gw_status gw_pipeline_build(const gw_channel_config *channel,
                            const gw_mediamtx_config *mediamtx,
                            const char *ffmpeg_binary,
                            gw_pipeline_argv *arguments,
                            gw_error *error);

/* Render a shell-quoted diagnostic command with URL credentials redacted. */
gw_status gw_pipeline_render_redacted(const gw_pipeline_argv *arguments,
                                      char *output, size_t output_size,
                                      gw_error *error);

#endif
