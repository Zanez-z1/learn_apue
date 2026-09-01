#ifndef GATEWAY_PIPELINE_BUILDER_H
#define GATEWAY_PIPELINE_BUILDER_H

/* FFmpeg argument construction without shell command evaluation. */

#include "gateway/config.h"

#include <stddef.h>

#define GW_PIPELINE_MAX_ARGS 40U

/**
 * gw_pipeline_argv - owned FFmpeg argument vector
 * @items: null-terminated array of heap-allocated strings
 * @count: number of arguments, excluding the terminating NULL
 */
typedef struct {
    char *items[GW_PIPELINE_MAX_ARGS + 1U];
    size_t count;
} gw_pipeline_argv;

/**
 * gw_pipeline_argv_init - initialize an empty argument vector
 * @arguments: argument vector to initialize
 */
void gw_pipeline_argv_init(gw_pipeline_argv *arguments);

/**
 * gw_pipeline_argv_free - release all strings in an argument vector
 * @arguments: argument vector to release, or NULL
 */
void gw_pipeline_argv_free(gw_pipeline_argv *arguments);

/**
 * gw_pipeline_build - build an FFmpeg argument vector
 * @channel: channel configuration
 * @mediamtx: MediaMTX publishing configuration
 * @ffmpeg_binary: executable name or path, or NULL to use "ffmpeg" from PATH
 * @arguments: resulting argument vector, released by gw_pipeline_argv_free()
 * @error: optional error details
 *
 * Generated FFmpeg arguments:
 * - "-nostdin -hide_banner -loglevel warning": unattended, concise logging
 * - "-progress pipe:1 -stats_period 1": progress on stdout once per second
 * - "-rtsp_transport <transport>": configured RTSP input transport
 * - "-hwaccel rkmpp": Rockchip MPP hardware acceleration
 * - "-hwaccel_output_format drm_prime": hardware-backed decoded frames
 * - "-c:v <decoder> -i <url>": video decoder and RTSP input
 * - "-vf scale_rkrga=w=...:h=...:format=nv12": RGA scaling and conversion
 * - "-c:v <encoder> -b:v <kbps>k": video encoder and target bitrate
 * - "-r <fps> -g <fps*2>": frame rate and two-second GOP
 * - "-an": disable audio
 * - "-f rtsp -rtsp_transport tcp <publish_url>": publish RTSP over TCP
 *
 * Return: GW_OK on success, otherwise an error status.
 */
gw_status gw_pipeline_build(const gw_channel_config *channel,
                            const gw_mediamtx_config *mediamtx,
                            const char *ffmpeg_binary,
                            gw_pipeline_argv *arguments,
                            gw_error *error);

#endif
