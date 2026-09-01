/* Build ffprobe argv and parse its stable key=value stream description. */
#include "gateway/probe.h"

#include <errno.h>
#include <limits.h>
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

static void clear_error(gw_error *error)
{
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
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

void gw_probe_argv_init(gw_probe_argv *arguments)
{
    if (arguments != NULL) {
        memset(arguments, 0, sizeof(*arguments));
    }
}

void gw_probe_argv_free(gw_probe_argv *arguments)
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

static gw_status append_argument(gw_probe_argv *arguments, const char *value,
                                 gw_error *error)
{
    char *copy;

    if (arguments->count >= GW_PROBE_MAX_ARGS) {
        set_error(error, GW_ERR_OVERFLOW, "ffprobe argument limit exceeded");
        return GW_ERR_OVERFLOW;
    }
    copy = duplicate_text(value);
    if (copy == NULL) {
        set_error(error, GW_ERR_NO_MEMORY, "cannot allocate ffprobe argument");
        return GW_ERR_NO_MEMORY;
    }
    arguments->items[arguments->count++] = copy;
    arguments->items[arguments->count] = NULL;
    return GW_OK;
}

gw_status gw_probe_build(const gw_channel_config *channel,
                         const char *ffprobe_binary,
                         gw_probe_argv *arguments,
                         gw_error *error)
{
    static const char *options[] = {
        "-v", "error",
        "-rtsp_transport", NULL,
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,width,height",
        "-of", "default=noprint_wrappers=1:nokey=0"
    };
    const char *binary = ffprobe_binary != NULL ? ffprobe_binary : "ffprobe";
    size_t index;
    gw_status status;

    if (channel == NULL || arguments == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "probe channel and arguments are required");
        return GW_ERR_ARGUMENT;
    }
    gw_probe_argv_init(arguments);
    status = append_argument(arguments, binary, error);
    if (status != GW_OK) {
        return status;
    }
    for (index = 0U; index < sizeof(options) / sizeof(options[0]); ++index) {
        const char *value = options[index];

        if (value == NULL) {
            value = channel->input.transport;
        }
        status = append_argument(arguments, value, error);
        if (status != GW_OK) {
            gw_probe_argv_free(arguments);
            return status;
        }
    }
    status = append_argument(arguments, channel->input.url, error);
    if (status != GW_OK) {
        gw_probe_argv_free(arguments);
        return status;
    }
    clear_error(error);
    return GW_OK;
}

static gw_status parse_positive_int(const char *value, int *result,
                                    const char *field, gw_error *error)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX) {
        set_error(error, GW_ERR_PARSE, "ffprobe %s is invalid", field);
        return GW_ERR_PARSE;
    }
    *result = (int)parsed;
    return GW_OK;
}

static gw_status parse_line(char *line, gw_probe_info *info, gw_error *error)
{
    char *separator = strchr(line, '=');
    const char *value;
    size_t length;

    if (separator == NULL) {
        return GW_OK;
    }
    *separator = '\0';
    value = separator + 1;
    if (strcmp(line, "codec_name") == 0) {
        length = strlen(value);
        if (length == 0U || length >= sizeof(info->codec_name)) {
            set_error(error, GW_ERR_PARSE, "ffprobe codec_name is invalid");
            return GW_ERR_PARSE;
        }
        memcpy(info->codec_name, value, length + 1U);
    } else if (strcmp(line, "width") == 0) {
        return parse_positive_int(value, &info->width, "width", error);
    } else if (strcmp(line, "height") == 0) {
        return parse_positive_int(value, &info->height, "height", error);
    }
    return GW_OK;
}

gw_status gw_probe_parse(const char *output, gw_probe_info *info, gw_error *error)
{
    char buffer[1024];
    char *line_start;
    char *cursor;
    size_t length;
    gw_status status;

    if (output == NULL || info == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "ffprobe output and result are required");
        return GW_ERR_ARGUMENT;
    }
    length = strlen(output);
    if (length >= sizeof(buffer)) {
        set_error(error, GW_ERR_OVERFLOW, "ffprobe output exceeds %zu bytes",
                  sizeof(buffer) - 1U);
        return GW_ERR_OVERFLOW;
    }
    memset(info, 0, sizeof(*info));
    memcpy(buffer, output, length + 1U);
    line_start = buffer;
    for (cursor = buffer; ; ++cursor) {
        if (*cursor == '\r' || *cursor == '\n' || *cursor == '\0') {
            char delimiter = *cursor;
            bool finished = delimiter == '\0';

            *cursor = '\0';
            status = parse_line(line_start, info, error);
            if (status != GW_OK) {
                return status;
            }
            if (finished) {
                break;
            }
            if (delimiter == '\r' && cursor[1] == '\n') {
                ++cursor;
            }
            line_start = cursor + 1;
        }
    }
    if (info->codec_name[0] == '\0' || info->width <= 0 || info->height <= 0) {
        set_error(error, GW_ERR_PARSE,
                  "ffprobe output is missing codec_name, width, or height");
        return GW_ERR_PARSE;
    }
    clear_error(error);
    return GW_OK;
}

bool gw_probe_matches_decoder(const gw_probe_info *info, const char *decoder)
{
    if (info == NULL || decoder == NULL) {
        return false;
    }
    return (strcmp(info->codec_name, "h264") == 0 &&
            strcmp(decoder, "h264_rkmpp") == 0) ||
           (strcmp(info->codec_name, "hevc") == 0 &&
            strcmp(decoder, "hevc_rkmpp") == 0);
}
