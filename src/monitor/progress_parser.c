#include "gateway/progress_parser.h"

#include <errno.h>
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

void gw_progress_parser_init(gw_progress_parser *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

static bool parse_u64(const char *value, uint64_t *output)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *output = (uint64_t)parsed;
    return true;
}

static bool parse_i64(const char *value, int64_t *output)
{
    char *end = NULL;
    long long parsed;

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *output = (int64_t)parsed;
    return true;
}

static bool parse_double_value(const char *value, double *output)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || (*end != '\0' && strcmp(end, "x") != 0)) {
        return false;
    }
    *output = parsed;
    return true;
}

static void copy_truncated(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool parse_line(gw_progress_parser *parser, char *line,
                       gw_worker_progress *snapshot)
{
    char *separator;
    const char *key;
    const char *value;

    separator = strchr(line, '=');
    if (separator == NULL) {
        return false;
    }
    *separator = '\0';
    key = line;
    value = separator + 1;

    if (strcmp(key, "frame") == 0) {
        parse_u64(value, &parser->current.frame);
    } else if (strcmp(key, "fps") == 0) {
        parse_double_value(value, &parser->current.fps);
    } else if (strcmp(key, "bitrate") == 0) {
        copy_truncated(parser->current.bitrate, sizeof(parser->current.bitrate), value);
    } else if (strcmp(key, "out_time_us") == 0) {
        parse_i64(value, &parser->current.out_time_us);
    } else if (strcmp(key, "drop_frames") == 0) {
        parse_u64(value, &parser->current.drop_frames);
    } else if (strcmp(key, "speed") == 0) {
        parse_double_value(value, &parser->current.speed);
    } else if (strcmp(key, "progress") == 0) {
        copy_truncated(parser->current.status, sizeof(parser->current.status), value);
        *snapshot = parser->current;
        memset(&parser->current, 0, sizeof(parser->current));
        return true;
    }
    return false;
}

gw_status gw_progress_parser_consume(gw_progress_parser *parser,
                                     const char *data, size_t data_length,
                                     gw_worker_progress *progress,
                                     bool *completed, gw_error *error)
{
    size_t index;

    if (parser == NULL || (data == NULL && data_length != 0U) || progress == NULL ||
        completed == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "invalid progress parser arguments");
        return GW_ERR_ARGUMENT;
    }
    *completed = false;
    for (index = 0U; index < data_length; ++index) {
        char byte = data[index];
        if (byte == '\n') {
            bool line_completed;
            if (parser->line_length > 0U &&
                parser->line[parser->line_length - 1U] == '\r') {
                --parser->line_length;
            }
            parser->line[parser->line_length] = '\0';
            line_completed = parse_line(parser, parser->line, progress);
            parser->line_length = 0U;
            if (line_completed) {
                *completed = true;
            }
        } else {
            if (parser->line_length + 1U >= sizeof(parser->line)) {
                parser->line_length = 0U;
                set_error(error, GW_ERR_OVERFLOW,
                          "FFmpeg progress line exceeds %u bytes",
                          (unsigned int)GW_PROGRESS_LINE_CAP - 1U);
                return GW_ERR_OVERFLOW;
            }
            parser->line[parser->line_length++] = byte;
        }
    }
    if (error != NULL) {
        error->code = GW_OK;
        error->message[0] = '\0';
    }
    return GW_OK;
}
