#ifndef GATEWAY_PROGRESS_PARSER_H
#define GATEWAY_PROGRESS_PARSER_H

/* Incremental parser for records emitted by FFmpeg -progress pipe:1. */

#include "gateway/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PROGRESS_LINE_CAP 1024U

typedef struct {
    /* Most recent complete progress record. */
    uint64_t frame;
    double fps;
    char bitrate[32];
    int64_t out_time_us;
    uint64_t drop_frames;
    double speed;
    char status[16];
} gw_worker_progress;

typedef struct {
    /* Partial input line retained across pipe reads. */
    char line[GW_PROGRESS_LINE_CAP];
    size_t line_length;
    gw_worker_progress current;
} gw_progress_parser;

/* Reset both the partial line and in-progress record. */
void gw_progress_parser_init(gw_progress_parser *parser);

/*
 * Consume an arbitrary pipe fragment. completed is set when a progress= line
 * closes a record; progress then receives the latest completed snapshot.
 */
gw_status gw_progress_parser_consume(gw_progress_parser *parser,
                                     const char *data, size_t data_length,
                                     gw_worker_progress *progress,
                                     bool *completed, gw_error *error);

#endif
