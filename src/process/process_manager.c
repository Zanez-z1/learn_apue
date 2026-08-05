/* POSIX worker creation, nonblocking output capture, termination, and reaping. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

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

static void close_fd(int *descriptor)
{
    if (*descriptor >= 0) {
        close(*descriptor);
        *descriptor = -1;
    }
}

static gw_status configure_pipe(int descriptors[2], gw_error *error)
{
    int flags;
    int index;

    /* Prevent unrelated descriptors from leaking through exec in the child. */
    for (index = 0; index < 2; ++index) {
        flags = fcntl(descriptors[index], F_GETFD);
        if (flags < 0 || fcntl(descriptors[index], F_SETFD, flags | FD_CLOEXEC) < 0) {
            set_error(error, GW_ERR_IO, "cannot configure close-on-exec: %s",
                      strerror(errno));
            return GW_ERR_IO;
        }
    }
    /* Only the parent read end is nonblocking; the worker keeps normal writes. */
    flags = fcntl(descriptors[0], F_GETFL);
    if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        set_error(error, GW_ERR_IO, "cannot make worker pipe nonblocking: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    return GW_OK;
}

void gw_process_init(gw_process *process)
{
    if (process == NULL) {
        return;
    }
    memset(process, 0, sizeof(*process));
    process->pid = -1;
    process->stdout_fd = -1;
    process->stderr_fd = -1;
}

static int add_file_actions(posix_spawn_file_actions_t *actions,
                            int stdout_pipe[2], int stderr_pipe[2])
{
    int result;

    result = posix_spawn_file_actions_adddup2(actions, stdout_pipe[1], STDOUT_FILENO);
    if (result != 0) {
        return result;
    }
    result = posix_spawn_file_actions_adddup2(actions, stderr_pipe[1], STDERR_FILENO);
    if (result != 0) {
        return result;
    }
    result = posix_spawn_file_actions_addclose(actions, stdout_pipe[0]);
    if (result != 0) {
        return result;
    }
    result = posix_spawn_file_actions_addclose(actions, stdout_pipe[1]);
    if (result != 0) {
        return result;
    }
    result = posix_spawn_file_actions_addclose(actions, stderr_pipe[0]);
    if (result != 0) {
        return result;
    }
    return posix_spawn_file_actions_addclose(actions, stderr_pipe[1]);
}

gw_status gw_process_start(gw_process *process, char *const arguments[],
                           gw_error *error)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actions_initialized = false;
    bool attributes_initialized = false;
    short flags = POSIX_SPAWN_SETPGROUP;
    pid_t child_pid = -1;
    int result;
    gw_status status;

    if (process == NULL || arguments == NULL || arguments[0] == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "worker arguments are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->running || process->pid > 0) {
        set_error(error, GW_ERR_VALIDATION, "worker process is already active");
        return GW_ERR_VALIDATION;
    }

    if (pipe(stdout_pipe) < 0) {
        set_error(error, GW_ERR_IO, "cannot create stdout pipe: %s", strerror(errno));
        return GW_ERR_IO;
    }
    status = configure_pipe(stdout_pipe, error);
    if (status != GW_OK) {
        goto fail;
    }
    if (pipe(stderr_pipe) < 0) {
        set_error(error, GW_ERR_IO, "cannot create stderr pipe: %s", strerror(errno));
        goto fail;
    }
    status = configure_pipe(stderr_pipe, error);
    if (status != GW_OK) {
        goto fail;
    }

    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        set_error(error, GW_ERR_IO, "cannot initialize spawn file actions: %s",
                  strerror(result));
        goto fail;
    }
    actions_initialized = true;
    result = add_file_actions(&actions, stdout_pipe, stderr_pipe);
    if (result != 0) {
        set_error(error, GW_ERR_IO, "cannot configure worker file actions: %s",
                  strerror(result));
        goto fail;
    }

    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        set_error(error, GW_ERR_IO, "cannot initialize spawn attributes: %s",
                  strerror(result));
        goto fail;
    }
    attributes_initialized = true;
    /* A dedicated process group lets stop/kill include any FFmpeg descendants. */
    result = posix_spawnattr_setflags(&attributes, flags);
    if (result == 0) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result != 0) {
        set_error(error, GW_ERR_IO, "cannot configure worker process group: %s",
                  strerror(result));
        goto fail;
    }

    /* Execute argv directly; no shell parses configuration-derived arguments. */
    result = posix_spawnp(&child_pid, arguments[0], &actions, &attributes, arguments,
                          environ);
    if (result != 0) {
        set_error(error, GW_ERR_IO, "cannot start worker '%s': %s", arguments[0],
                  strerror(result));
        goto fail;
    }

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[1]);

    process->pid = child_pid;
    process->stdout_fd = stdout_pipe[0];
    process->stderr_fd = stderr_pipe[0];
    process->running = true;
    process->reaped = false;
    process->wait_status = 0;
    clear_error(error);
    return GW_OK;

fail:
    if (attributes_initialized) {
        posix_spawnattr_destroy(&attributes);
    }
    if (actions_initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (stdout_pipe[0] >= 0) {
        close(stdout_pipe[0]);
    }
    if (stdout_pipe[1] >= 0) {
        close(stdout_pipe[1]);
    }
    if (stderr_pipe[0] >= 0) {
        close(stderr_pipe[0]);
    }
    if (stderr_pipe[1] >= 0) {
        close(stderr_pipe[1]);
    }
    return GW_ERR_IO;
}

gw_status gw_process_read(gw_process *process, gw_process_stream stream,
                          char *buffer, size_t capacity, size_t *bytes_read,
                          bool *end_of_stream, gw_error *error)
{
    int *descriptor;
    ssize_t result;

    if (process == NULL || buffer == NULL || capacity == 0U || bytes_read == NULL ||
        end_of_stream == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "invalid worker read arguments");
        return GW_ERR_ARGUMENT;
    }
    if (stream == GW_PROCESS_STDOUT) {
        descriptor = &process->stdout_fd;
    } else if (stream == GW_PROCESS_STDERR) {
        descriptor = &process->stderr_fd;
    } else {
        set_error(error, GW_ERR_ARGUMENT, "invalid worker stream");
        return GW_ERR_ARGUMENT;
    }

    *bytes_read = 0U;
    *end_of_stream = false;
    if (*descriptor < 0) {
        *end_of_stream = true;
        clear_error(error);
        return GW_OK;
    }

    result = read(*descriptor, buffer, capacity);
    if (result > 0) {
        *bytes_read = (size_t)result;
        clear_error(error);
        return GW_OK;
    }
    if (result == 0) {
        close_fd(descriptor);
        *end_of_stream = true;
        clear_error(error);
        return GW_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        clear_error(error);
        return GW_OK;
    }
    set_error(error, GW_ERR_IO, "cannot read worker stream: %s", strerror(errno));
    return GW_ERR_IO;
}

gw_status gw_process_poll_exit(gw_process *process, bool *exited, gw_error *error)
{
    pid_t result;

    if (process == NULL || exited == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "worker and exit result are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->reaped) {
        *exited = true;
        clear_error(error);
        return GW_OK;
    }
    if (process->pid <= 0) {
        set_error(error, GW_ERR_VALIDATION, "worker process has not been started");
        return GW_ERR_VALIDATION;
    }

    /* Reaping is part of polling so a reported exit cannot leave a zombie. */
    result = waitpid(process->pid, &process->wait_status, WNOHANG);
    if (result == 0) {
        *exited = false;
        clear_error(error);
        return GW_OK;
    }
    if (result == process->pid) {
        process->running = false;
        process->reaped = true;
        *exited = true;
        clear_error(error);
        return GW_OK;
    }
    if (result < 0 && errno == EINTR) {
        *exited = false;
        clear_error(error);
        return GW_OK;
    }
    set_error(error, GW_ERR_IO, "cannot inspect worker process: %s", strerror(errno));
    return GW_ERR_IO;
}

static bool deadline_reached(const struct timespec *deadline)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return true;
    }
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static struct timespec make_deadline(int timeout_ms)
{
    struct timespec deadline = {0};
    long additional_nanoseconds;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    additional_nanoseconds = (long)(timeout_ms % 1000) * 1000000L;
    deadline.tv_nsec += additional_nanoseconds;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static gw_status wait_after_kill(gw_process *process, gw_error *error)
{
    pid_t result;

    do {
        result = waitpid(process->pid, &process->wait_status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != process->pid) {
        set_error(error, GW_ERR_IO, "cannot reap worker process: %s", strerror(errno));
        return GW_ERR_IO;
    }
    process->running = false;
    process->reaped = true;
    return GW_OK;
}

gw_status gw_process_stop(gw_process *process, int timeout_ms, gw_error *error)
{
    struct timespec deadline;
    const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 10000000L};
    bool exited;
    gw_status status;

    if (process == NULL || timeout_ms < 0) {
        set_error(error, GW_ERR_ARGUMENT, "worker and non-negative timeout are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->reaped) {
        clear_error(error);
        return GW_OK;
    }
    if (process->pid <= 0) {
        set_error(error, GW_ERR_VALIDATION, "worker process has not been started");
        return GW_ERR_VALIDATION;
    }

    /* Negative PID targets the worker's entire process group. */
    if (kill(-process->pid, SIGTERM) < 0 && errno != ESRCH) {
        set_error(error, GW_ERR_IO, "cannot terminate worker process group: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    deadline = make_deadline(timeout_ms);
    while (!deadline_reached(&deadline)) {
        status = gw_process_poll_exit(process, &exited, error);
        if (status != GW_OK || exited) {
            return status;
        }
        nanosleep(&pause_time, NULL);
    }

    status = gw_process_poll_exit(process, &exited, error);
    if (status != GW_OK || exited) {
        return status;
    }
    /* Escalate only after the graceful deadline expires. */
    if (kill(-process->pid, SIGKILL) < 0 && errno != ESRCH) {
        set_error(error, GW_ERR_IO, "cannot kill worker process group: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    status = wait_after_kill(process, error);
    if (status == GW_OK) {
        clear_error(error);
    }
    return status;
}

int gw_process_exit_code(const gw_process *process)
{
    if (process == NULL || !process->reaped) {
        return -1;
    }
    if (WIFEXITED(process->wait_status)) {
        return WEXITSTATUS(process->wait_status);
    }
    if (WIFSIGNALED(process->wait_status)) {
        return 128 + WTERMSIG(process->wait_status);
    }
    return -1;
}

gw_status gw_process_close(gw_process *process, gw_error *error)
{
    if (process == NULL) {
        set_error(error, GW_ERR_ARGUMENT, "worker process is required");
        return GW_ERR_ARGUMENT;
    }
    if (process->running) {
        set_error(error, GW_ERR_VALIDATION, "cannot close a running worker process");
        return GW_ERR_VALIDATION;
    }
    close_fd(&process->stdout_fd);
    close_fd(&process->stderr_fd);
    process->pid = -1;
    clear_error(error);
    return GW_OK;
}
