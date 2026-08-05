#ifndef GATEWAY_PROCESS_MANAGER_H
#define GATEWAY_PROCESS_MANAGER_H

#include "gateway/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    GW_PROCESS_STDOUT = 0,
    GW_PROCESS_STDERR
} gw_process_stream;

typedef struct {
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    bool running;
    bool reaped;
    int wait_status;
} gw_process;

void gw_process_init(gw_process *process);

gw_status gw_process_start(gw_process *process, char *const arguments[],
                           gw_error *error);

gw_status gw_process_read(gw_process *process, gw_process_stream stream,
                          char *buffer, size_t capacity, size_t *bytes_read,
                          bool *end_of_stream, gw_error *error);

gw_status gw_process_poll_exit(gw_process *process, bool *exited, gw_error *error);

gw_status gw_process_stop(gw_process *process, int timeout_ms, gw_error *error);

int gw_process_exit_code(const gw_process *process);

gw_status gw_process_close(gw_process *process, gw_error *error);

#endif
