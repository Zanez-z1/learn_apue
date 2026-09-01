#ifndef GATEWAY_PROCESS_MANAGER_H
#define GATEWAY_PROCESS_MANAGER_H

/* Lifecycle and pipe ownership for one external FFmpeg worker process. */

#include "gateway/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    GW_PROCESS_STDOUT = 1,
    GW_PROCESS_STDERR
} gw_process_stream;

typedef struct {
    /* Child PID also identifies the process group created at spawn time. */
    pid_t pid;
    /* Parent-owned, nonblocking read ends of the child output pipes. */
    int stdout_fd;
    int stderr_fd;
    bool running;
    bool reaped;
    int wait_status;
} gw_process;

/* Initialize an inactive process handle; required before gw_process_start(). */
void gw_process_init(gw_process *process);

/* Spawn arguments[0] without a shell and capture stdout/stderr in separate pipes. */
gw_status gw_process_start(gw_process *process, char *const arguments[],
                           gw_error *error);

/* Perform one nonblocking read from the selected stream. */
gw_status gw_process_read(gw_process *process, gw_process_stream stream,
                          char *buffer, size_t capacity, size_t *bytes_read,
                          bool *end_of_stream, gw_error *error);

/* Poll and reap the direct child without blocking. */
gw_status gw_process_poll_exit(gw_process *process, bool *exited, gw_error *error);

/* Stop the worker process group with SIGTERM, escalating to SIGKILL on timeout. */
gw_status gw_process_stop(gw_process *process, int timeout_ms, gw_error *error);

/* Return a shell-style exit code, or -1 until the child has been reaped. */
int gw_process_exit_code(const gw_process *process);

/* Close remaining pipe descriptors after the worker is no longer running. */
gw_status gw_process_close(gw_process *process, gw_error *error);

#endif
