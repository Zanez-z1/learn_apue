#ifndef GATEWAY_HTTP_SERVER_H
#define GATEWAY_HTTP_SERVER_H

/* Bounded local HTTP/1.x server and JSON control-plane responses. */

#include "gateway/channel_manager.h"
#include "gateway/config.h"
#include "gateway/recording_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_HTTP_BODY_CAP 131072U

typedef struct gw_http_server gw_http_server;

typedef struct {
    int status_code;
    bool allow_get;
    bool allow_post;
    char body[GW_HTTP_BODY_CAP];
    size_t body_length;
} gw_http_response;

/* Router used by the socket server and unit tests. Control routes mutate manager state. */
gw_status gw_http_route(gw_channel_manager *manager,
                        const gw_recording_config *recording,
                        const char *method, const char *target,
                        gw_http_response *response, gw_error *error);

/* The manager is borrowed and must outlive the server. */
gw_status gw_http_server_create(gw_http_server **server,
                                const gw_server_config *config,
                                const gw_recording_config *recording,
                                gw_channel_manager *manager, gw_error *error);

gw_status gw_http_server_start(gw_http_server *server, gw_error *error);

/* Returns the configured port, or the kernel-selected port when configured as 0. */
uint16_t gw_http_server_port(const gw_http_server *server);

/* Idempotently stop the accept loop and release the listening socket. */
void gw_http_server_stop(gw_http_server *server);

void gw_http_server_destroy(gw_http_server *server);

#endif
