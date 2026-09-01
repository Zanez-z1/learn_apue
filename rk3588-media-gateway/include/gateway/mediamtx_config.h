#ifndef GATEWAY_MEDIAMTX_CONFIG_H
#define GATEWAY_MEDIAMTX_CONFIG_H

/* Render the MediaMTX recording/playback configuration owned by gateway config. */

#include "gateway/config.h"

#include <stddef.h>

#define GW_MEDIAMTX_CONFIG_CAP 65536U

gw_status gw_mediamtx_render_config(const gw_config *config, char *output,
                                    size_t capacity, gw_error *error);

#endif
