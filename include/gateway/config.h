#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_MAX_CHANNELS 64U
#define GW_ID_CAP 65U
#define GW_URL_CAP 2048U
#define GW_PATH_CAP 129U
#define GW_NAME_CAP 33U
#define GW_ERROR_CAP 256U

typedef enum {
    GW_OK = 0,
    GW_ERR_ARGUMENT,
    GW_ERR_IO,
    GW_ERR_PARSE,
    GW_ERR_VALIDATION,
    GW_ERR_ENV,
    GW_ERR_NO_MEMORY,
    GW_ERR_OVERFLOW
} gw_status;

typedef struct {
    gw_status code;
    char message[GW_ERROR_CAP];
} gw_error;

typedef struct {
    char listen[64];
    uint16_t port;
} gw_server_config;

typedef struct {
    char publish_base_url[GW_URL_CAP];
} gw_mediamtx_config;

typedef struct {
    int startup_timeout_sec;
    int progress_timeout_sec;
    int stop_timeout_sec;
    int max_retries;
    int max_backoff_sec;
} gw_retry_policy;

typedef struct {
    char type[GW_NAME_CAP];
    char url[GW_URL_CAP];
    char transport[GW_NAME_CAP];
} gw_input_config;

typedef struct {
    char decoder[GW_NAME_CAP];
    int width;
    int height;
    char encoder[GW_NAME_CAP];
    int bitrate_kbps;
    int fps;
} gw_video_config;

typedef struct {
    char path[GW_PATH_CAP];
} gw_output_config;

typedef struct {
    char id[GW_ID_CAP];
    bool enabled;
    gw_input_config input;
    gw_video_config video;
    gw_output_config output;
} gw_channel_config;

typedef struct {
    gw_server_config server;
    gw_mediamtx_config mediamtx;
    gw_retry_policy defaults;
    gw_channel_config channels[GW_MAX_CHANNELS];
    size_t channel_count;
} gw_config;

void gw_config_init(gw_config *config);
gw_status gw_config_load_file(const char *path, gw_config *config, gw_error *error);
gw_status gw_config_validate(const gw_config *config, gw_error *error);
gw_status gw_expand_environment(const char *input, char *output, size_t output_size,
                                gw_error *error);
gw_status gw_redact_url(const char *url, char *output, size_t output_size);
const char *gw_status_string(gw_status status);

#endif
