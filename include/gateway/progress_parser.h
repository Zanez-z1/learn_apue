#ifndef GATEWAY_PROGRESS_PARSER_H
#define GATEWAY_PROGRESS_PARSER_H

#include "gateway/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PROGRESS_LINE_CAP 1024U

typedef struct {
    uint64_t frame;
    double fps;
    char bitrate[32];
    int64_t out_time_us;
    uint64_t drop_frames;
    double speed;
    char status[16];
} gw_worker_progress;

typedef struct {
    char line[GW_PROGRESS_LINE_CAP];
    size_t line_length;
    gw_worker_progress current;
} gw_progress_parser;

void gw_progress_parser_init(gw_progress_parser *parser);
gw_status gw_progress_parser_consume(gw_progress_parser *parser,
                                     const char *data, size_t data_length,
                                     gw_worker_progress *progress,
                                     bool *completed, gw_error *error);

#endif
