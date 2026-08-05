#include "gateway/pipeline_builder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(gw_error *error, gw_status code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static char *duplicate_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

void gw_pipeline_argv_init(gw_pipeline_argv *arguments)
{
    if (arguments != NULL) {
        memset(arguments, 0, sizeof(*arguments));
    }
}

void gw_pipeline_argv_free(gw_pipeline_argv *arguments)
{
    size_t index;

    if (arguments == NULL) {
        return;
    }
    for (index = 0U; index < arguments->count; ++index) {
        free(arguments->items[index]);
        arguments->items[index] = NULL;
    }
    arguments->count = 0U;
    arguments->items[0] = NULL;
}

static gw_status append_argument(gw_pipeline_argv *arguments, const char *value,
                                 gw_error *error)
{
    char *copy;

    if (arguments->count >= GW_PIPELINE_MAX_ARGS) {
        set_error(error, GW_ERR_OVERFLOW, "FFmpeg argument limit exceeded");
        return GW_ERR_OVERFLOW;
    }
    copy = duplicate_text(value);
    if (copy == NULL) {
        set_error(error, GW_ERR_NO_MEMORY, "cannot allocate FFmpeg argument");
        return GW_ERR_NO_MEMORY;
    }
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
        set_error(error, GW_ERR_OVERFLOW, "cannot format FFmpeg numeric argument");
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
    char filter[160];
    char output_url[GW_URL_CAP];
    int written;
    gw_status status;
    const char *binary = ffmpeg_binary != NULL ? ffmpeg_binary : "ffmpeg";

    if (channel == NULL || mediamtx == NULL || arguments == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "pipeline inputs are required");
        return GW_ERR_ARGUMENT;
    }
    if (mediamtx->publish_base_url[0] == '\0') {
        set_error(error, GW_ERR_VALIDATION, "MediaMTX publish base URL is empty");
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

    APPEND(binary);
    APPEND("-nostdin");
    APPEND("-hide_banner");
    APPEND_PAIR("-loglevel", "warning");
    APPEND_PAIR("-progress", "pipe:1");
    APPEND_PAIR("-stats_period", "1");
    APPEND_PAIR("-rtsp_transport", channel->input.transport);
    APPEND_PAIR("-hwaccel", "rkmpp");
    APPEND_PAIR("-hwaccel_output_format", "drm_prime");
    APPEND_PAIR("-c:v", channel->video.decoder);
    APPEND_PAIR("-i", channel->input.url);

    written = snprintf(filter, sizeof(filter),
                       "scale_rkrga=w=%d:h=%d:format=nv12", channel->video.width,
                       channel->video.height);
    if (written < 0 || (size_t)written >= sizeof(filter)) {
        set_error(error, GW_ERR_OVERFLOW, "RGA filter expression is too long");
        status = GW_ERR_OVERFLOW;
        goto fail;
    }
    APPEND_PAIR("-vf", filter);
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
    APPEND("-an");
    APPEND_PAIR("-f", "rtsp");

    written = snprintf(output_url, sizeof(output_url), "%s%s%s",
                       mediamtx->publish_base_url,
                       mediamtx->publish_base_url[strlen(mediamtx->publish_base_url) - 1U]
                                   == '/'
                           ? ""
                           : "/",
                       channel->output.path);
    if (written < 0 || (size_t)written >= sizeof(output_url)) {
        set_error(error, GW_ERR_OVERFLOW, "publish URL is too long");
        status = GW_ERR_OVERFLOW;
        goto fail;
    }
    APPEND(output_url);

#undef APPEND_PAIR
#undef APPEND
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
    return GW_OK;

fail:
#undef APPEND_PAIR
#undef APPEND
    gw_pipeline_argv_free(arguments);
    return status;
}

static gw_status append_rendered(char *output, size_t output_size, size_t *used,
                                 const char *text, gw_error *error)
{
    size_t length;

    if (output == NULL || used == NULL || text == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "invalid rendered command fragment");
        return GW_ERR_ARGUMENT;
    }
    length = strlen(text);
    if (*used + length >= output_size) {
        set_error(error, GW_ERR_OVERFLOW, "rendered command exceeds output buffer");
        return GW_ERR_OVERFLOW;
    }
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return GW_OK;
}

static gw_status append_shell_quoted(char *output, size_t output_size, size_t *used,
                                     const char *argument, gw_error *error)
{
    const char *cursor;
    char character[2] = {'\0', '\0'};
    gw_status status = append_rendered(output, output_size, used, "'", error);
    if (status != GW_OK) {
        return status;
    }
    for (cursor = argument; *cursor != '\0'; ++cursor) {
        if (*cursor == '\'') {
            status = append_rendered(output, output_size, used, "'\\''", error);
        } else {
            character[0] = *cursor;
            character[1] = '\0';
            status = append_rendered(output, output_size, used, character, error);
        }
        if (status != GW_OK) {
            return status;
        }
    }
    return append_rendered(output, output_size, used, "'", error);
}

gw_status gw_pipeline_render_redacted(const gw_pipeline_argv *arguments,
                                      char *output, size_t output_size,
                                      gw_error *error)
{
    size_t index;
    size_t used = 0U;

    if (arguments == NULL || output == NULL || output_size == 0U) {
        set_error(error, GW_ERR_ARGUMENT, "render output is required");
        return GW_ERR_ARGUMENT;
    }
    output[0] = '\0';
    for (index = 0U; index < arguments->count; ++index) {
        char redacted[GW_URL_CAP];
        const char *value = arguments->items[index];
        gw_status status;

        if (value == NULL) {
            set_error(error, GW_ERR_ARGUMENT, "FFmpeg argument %zu is null", index);
            return GW_ERR_ARGUMENT;
        }
        if (strstr(value, "://") != NULL) {
            status = gw_redact_url(value, redacted, sizeof(redacted));
            if (status != GW_OK) {
                set_error(error, status, "cannot redact URL argument");
                return status;
            }
            value = redacted;
        }
        if (index != 0U) {
            status = append_rendered(output, output_size, &used, " ", error);
            if (status != GW_OK) {
                return status;
            }
        }
        status = append_shell_quoted(output, output_size, &used, value, error);
        if (status != GW_OK) {
            return status;
        }
    }
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
    return GW_OK;
}
