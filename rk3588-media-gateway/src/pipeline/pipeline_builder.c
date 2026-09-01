/* Translate validated channel settings into an owned, shell-free FFmpeg argv. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/pipeline_builder.h"
#include "gateway/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gw_pipeline_argv_init(gw_pipeline_argv *arguments)
{
    if (arguments != NULL) {
        memset(arguments, 0, sizeof(*arguments));
    }
}

/*  Free the strings allocated by strdup() one by one,
then reset the argument array to empty.*/
void gw_pipeline_argv_free(gw_pipeline_argv *arguments)
{
    size_t index;

    if (arguments == NULL) {
        return;
    }
    for (index = 0; index < arguments->count; index++) {
        free(arguments->items[index]);
        arguments->items[index] = NULL;
    }
    arguments->count = 0;
}

static gw_status append_argument(gw_pipeline_argv *arguments, const char *value,
                                 gw_error *error)
{
    char *copy;
    /* Validate inputs and check for overflow before
     allocating a copy of the argument string.*/
    if (arguments->count >= GW_PIPELINE_MAX_ARGS) {
        gw_error_set(error, GW_ERR_OVERFLOW, "FFmpeg argument limit exceeded");
        return GW_ERR_OVERFLOW;
    }
    copy = strdup(value);
    if (copy == NULL) {
        gw_error_set(error, GW_ERR_NO_MEMORY, "cannot allocate FFmpeg argument");
        return GW_ERR_NO_MEMORY;
    }
    /* Always NULL‑terminate argv after each append for safe early access/cleanup.*/
    arguments->items[arguments->count++] = copy;
    arguments->items[arguments->count] = NULL;
    return GW_OK;
}

static gw_status append_pair(gw_pipeline_argv *arguments, const char *option,
                             const char *value, gw_error *error)
{
    gw_status status = append_argument(arguments, option, error);
    if (status == GW_OK) {
        status = append_argument(arguments, value, error);
    }
    return status;
}

static gw_status append_integer_pair(gw_pipeline_argv *arguments,
                                     const char *option, int value,
                                     const char *suffix, gw_error *error)
{
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%d%s", value, suffix);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        gw_error_set(error, GW_ERR_OVERFLOW, "cannot format FFmpeg numeric argument");
        return GW_ERR_OVERFLOW;
    }
    return append_pair(arguments, option, buffer, error);
}

gw_status gw_pipeline_build(const gw_channel_config *channel,
                            const gw_mediamtx_config *mediamtx,
                            const char *ffmpeg_binary,
                            gw_pipeline_argv *arguments,
                            gw_error *error)
{
    char rga_filter[160];
    char output_url[GW_URL_CAP];
    int written;
    gw_status status;
    const char *binary = ffmpeg_binary != NULL ? ffmpeg_binary : "ffmpeg";

    if (channel == NULL || mediamtx == NULL || arguments == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT, "pipeline inputs are required");
        return GW_ERR_ARGUMENT;
    }
    if (mediamtx->publish_base_url[0] == '\0') {
        gw_error_set(error, GW_ERR_VALIDATION, "MediaMTX publish base URL is empty");
        return GW_ERR_VALIDATION;
    }
    gw_pipeline_argv_init(arguments);

#define APPEND(value)                                                               \
    do {                                                                            \
        status = append_argument(arguments, value, error);                          \
        if (status != GW_OK) {                                                      \
            goto fail;                                                              \
        }                                                                           \
    } while (0)
#define APPEND_PAIR(option, value)                                                   \
    do {                                                                            \
        status = append_pair(arguments, option, value, error);                      \
        if (status != GW_OK) {                                                      \
            goto fail;                                                              \
        }                                                                           \
    } while (0)

    /* Run unattended and keep worker logs limited to actionable messages. */
    APPEND(binary);
    APPEND("-nostdin");
    APPEND("-hide_banner");
    APPEND_PAIR("-loglevel", "warning");

    /* Emit machine-readable progress once per second on stdout. */
    APPEND_PAIR("-progress", "pipe:1");
    APPEND_PAIR("-stats_period", "1");

    /* Decode the RTSP input into DRM PRIME frames with Rockchip MPP. */
    APPEND_PAIR("-rtsp_transport", channel->input.transport);
    APPEND_PAIR("-hwaccel", "rkmpp");
    APPEND_PAIR("-hwaccel_output_format", "drm_prime");
    APPEND_PAIR("-c:v", channel->video.decoder);
    APPEND_PAIR("-i", channel->input.url);

    written = snprintf(rga_filter, sizeof(rga_filter),
                       "scale_rkrga=w=%d:h=%d:format=nv12", channel->video.width,
                       channel->video.height);
    if (written < 0 || (size_t)written >= sizeof(rga_filter)) {
        gw_error_set(error, GW_ERR_OVERFLOW, "RGA filter expression is too long");
        status = GW_ERR_OVERFLOW;
        goto fail;
    }
    APPEND_PAIR("-vf", rga_filter);
    APPEND_PAIR("-c:v", channel->video.encoder);
    status = append_integer_pair(arguments, "-b:v", channel->video.bitrate_kbps,
                                 "k", error);
    if (status != GW_OK) {
        goto fail;
    }
    status = append_integer_pair(arguments, "-r", channel->video.fps, "", error);
    if (status != GW_OK) {
        goto fail;
    }
    status = append_integer_pair(arguments, "-g", channel->video.fps * 2, "", error);
    if (status != GW_OK) {
        goto fail;
    }
    APPEND("-an");
    APPEND_PAIR("-f", "rtsp");
    APPEND_PAIR("-rtsp_transport", "tcp");

    written = snprintf(output_url, sizeof(output_url), "%s%s%s",
                       mediamtx->publish_base_url,
                       mediamtx->publish_base_url[strlen(mediamtx->publish_base_url) - 1U]
                                   == '/'
                           ? ""
                           : "/",
                       channel->output.path);
    if (written < 0 || (size_t)written >= sizeof(output_url)) {
        gw_error_set(error, GW_ERR_OVERFLOW, "publish URL is too long");
        status = GW_ERR_OVERFLOW;
        goto fail;
    }
    APPEND(output_url);

#undef APPEND_PAIR
#undef APPEND
    gw_error_clear(error);
    return GW_OK;

fail:
#undef APPEND_PAIR
#undef APPEND
    gw_pipeline_argv_free(arguments);
    return status;
}
