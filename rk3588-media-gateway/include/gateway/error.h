#ifndef GATEWAY_ERROR_H
#define GATEWAY_ERROR_H

/* Shared status codes and optional caller-owned diagnostic storage. */

#define GW_ERROR_CAP 256U

typedef enum {
    GW_OK = 0,
    GW_ERR_ARGUMENT,
    GW_ERR_IO,
    GW_ERR_PARSE,
    GW_ERR_VALIDATION,
    GW_ERR_ENV,
    GW_ERR_NO_MEMORY,
    GW_ERR_OVERFLOW,
    GW_ERR_NOT_FOUND,
    GW_ERR_CONFLICT
} gw_status;

typedef struct {
    gw_status code;
    char message[GW_ERROR_CAP];
} gw_error;

/* Store a formatted diagnostic when error is non-null. */
void gw_error_set(gw_error *error, gw_status code, const char *format, ...);

/* Mark an optional diagnostic as successful and empty. */
void gw_error_clear(gw_error *error);

#endif
