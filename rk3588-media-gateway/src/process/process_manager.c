/* POSIX worker creation, nonblocking output capture, termination, and reaping. */
#define _POSIX_C_SOURCE 200809L

#include "gateway/error.h"
#include "gateway/process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Close an owned descriptor and mark it invalid. */
static void close_fd(int *descriptor)
{
    if (*descriptor >= 0) {
        close(*descriptor);
        *descriptor = -1;
    }
}

/* Protect pipe endpoints from exec leaks and make the read end nonblocking. */
static gw_status configure_pipe(int descriptors[2], gw_error *error)
{
    int flags;
    int index;

    /* Prevent unrelated descriptors from leaking through exec in the child. */
    for (index = 0; index < 2; ++index) {
        flags = fcntl(descriptors[index], F_GETFD);
        if (flags < 0 || fcntl(descriptors[index], F_SETFD, flags | FD_CLOEXEC) < 0) {
            gw_error_set(error, GW_ERR_IO, "cannot configure close-on-exec: %s",
                      strerror(errno));
            return GW_ERR_IO;
        }
    }
    /* Only the parent read end is nonblocking; the worker keeps normal writes. */
    flags = fcntl(descriptors[0], F_GETFL);
    if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        gw_error_set(error, GW_ERR_IO, "cannot make worker pipe nonblocking: %s",
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

/* Queue the child-side stdout/stderr redirection and close operations. */
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
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                  POSIX_SPAWN_SETSIGMASK;
    sigset_t default_signals;
    sigset_t empty_mask;
    pid_t child_pid = -1;
    int result;
    gw_status status;

    /* Check that a command was provided and this process is not already running. */
    if (process == NULL || arguments == NULL || arguments[0] == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT, "worker arguments are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->running || process->pid > 0) {
        gw_error_set(error, GW_ERR_VALIDATION, "worker process is already active");
        return GW_ERR_VALIDATION;
    }

    /* Create two pipes so gatewayd can read the worker's output and error logs. */
    if (pipe(stdout_pipe) < 0) {
        gw_error_set(error, GW_ERR_IO, "cannot create stdout pipe: %s", strerror(errno));
        return GW_ERR_IO;
    }
    status = configure_pipe(stdout_pipe, error);
    if (status != GW_OK) {
        goto fail;
    }
    if (pipe(stderr_pipe) < 0) {
        gw_error_set(error, GW_ERR_IO, "cannot create stderr pipe: %s", strerror(errno));
        goto fail;
    }
    status = configure_pipe(stderr_pipe, error);
    if (status != GW_OK) {
        goto fail;
    }

    /* Make the worker write stdout and stderr into the two pipes. */
    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        gw_error_set(error, GW_ERR_IO, "cannot initialize spawn file actions: %s",
                  strerror(result));
        goto fail;
    }
    actions_initialized = true;
    result = add_file_actions(&actions, stdout_pipe, stderr_pipe);
    if (result != 0) {
        gw_error_set(error, GW_ERR_IO, "cannot configure worker file actions: %s",
                  strerror(result));
        goto fail;
    }

    /* Put the worker in its own group and let it receive signals normally. */
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        gw_error_set(error, GW_ERR_IO, "cannot initialize spawn attributes: %s",
                  strerror(result));
        goto fail;
    }
    attributes_initialized = true;

    /* Restore normal handling of the signals blocked by gatewayd. */
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGHUP);
    sigemptyset(&empty_mask);
    /* Apply the process-group and signal settings, stopping on the first error. */
    result = posix_spawnattr_setflags(&attributes, flags);
    if (result == 0) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result == 0) {
        result = posix_spawnattr_setsigdefault(&attributes, &default_signals);
    }
    if (result == 0) {
        result = posix_spawnattr_setsigmask(&attributes, &empty_mask);
    }
    if (result != 0) {
        gw_error_set(error, GW_ERR_IO, "cannot configure worker spawn attributes: %s",
                  strerror(result));
        goto fail;
    }

    /* Start the worker directly without passing its arguments through a shell. */
    result = posix_spawnp(&child_pid, arguments[0], &actions, &attributes, arguments,
                          environ);
    if (result != 0) {
        gw_error_set(error, GW_ERR_IO, "cannot start worker '%s': %s", arguments[0],
                  strerror(result));
        goto fail;
    }

    /* After startup, gatewayd keeps only the two pipe read ends. */
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
    gw_error_clear(error);
    return GW_OK;

fail:
    /* If startup fails, release everything created before the failure. */
    if (attributes_initialized) {
        posix_spawnattr_destroy(&attributes);
    }
    if (actions_initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    close_fd(&stdout_pipe[0]);
    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[0]);
    close_fd(&stderr_pipe[1]);
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
        gw_error_set(error, GW_ERR_ARGUMENT, "invalid worker read arguments");
        return GW_ERR_ARGUMENT;
    }
    if (stream == GW_PROCESS_STDOUT) {
        descriptor = &process->stdout_fd;
    } else if (stream == GW_PROCESS_STDERR) {
        descriptor = &process->stderr_fd;
    } else {
        gw_error_set(error, GW_ERR_ARGUMENT, "invalid worker stream");
        return GW_ERR_ARGUMENT;
    }

    *bytes_read = 0;
    *end_of_stream = false;
    //if descriptor == -1
    if (*descriptor < 0) {
        *end_of_stream = true;
        gw_error_clear(error);
        return GW_OK;
    }

    result = read(*descriptor, buffer, capacity);
    if (result > 0) {
        *bytes_read = result;
        gw_error_clear(error);
        return GW_OK;
    }
    if (result == 0) {
        close_fd(descriptor);
        *end_of_stream = true;
        gw_error_clear(error);
        return GW_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        gw_error_clear(error);
        return GW_OK;
    }
    gw_error_set(error, GW_ERR_IO, "cannot read worker stream: %s", strerror(errno));
    return GW_ERR_IO;
}

gw_status gw_process_poll_exit(gw_process *process, bool *exited, gw_error *error)
{
    pid_t result;

    if (process == NULL || exited == NULL) {
        gw_error_set(error, GW_ERR_ARGUMENT, "worker and exit result are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->reaped) {
        *exited = true;
        gw_error_clear(error);
        return GW_OK;
    }
    if (process->pid <= 0) {
        gw_error_set(error, GW_ERR_VALIDATION, "worker process has not been started");
        return GW_ERR_VALIDATION;
    }

    /* Reaping is part of polling so a reported exit cannot leave a zombie. */
    result = waitpid(process->pid, &process->wait_status, WNOHANG);
    if (result == 0) {
        *exited = false;
        gw_error_clear(error);
        return GW_OK;
    }
    if (result == process->pid) {
        process->running = false;
        process->reaped = true;
        *exited = true;
        gw_error_clear(error);
        return GW_OK;
    }
    if (result < 0 && errno == EINTR) {
        *exited = false;
        gw_error_clear(error);
        return GW_OK;
    }
    gw_error_set(error, GW_ERR_IO, "cannot inspect worker process: %s", strerror(errno));
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

/* After SIGKILL is sent, wait for the worker and remove its zombie entry. */
static gw_status wait_after_kill(gw_process *process, gw_error *error)
{
    pid_t result;

    do {
        result = waitpid(process->pid, &process->wait_status, 0);
    } while (result < 0 && errno == EINTR);

    if (result != process->pid) {
        gw_error_set(error, GW_ERR_IO, "cannot reap worker process: %s", strerror(errno));
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
        gw_error_set(error, GW_ERR_ARGUMENT, "worker and non-negative timeout are required");
        return GW_ERR_ARGUMENT;
    }
    if (process->reaped) {
        gw_error_clear(error);
        return GW_OK;
    }
    if (process->pid <= 0) {
        gw_error_set(error, GW_ERR_VALIDATION, "worker process has not been started");
        return GW_ERR_VALIDATION;
    }

    /* Negative PID targets the worker's entire process group. */
    if (kill(-process->pid, SIGTERM) < 0 && errno != ESRCH) {
        gw_error_set(error, GW_ERR_IO, "cannot terminate worker process group: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    deadline = make_deadline(timeout_ms);
    while (!deadline_reached(&deadline)) {
        status = gw_process_poll_exit(process, &exited, error);
        if (status != GW_OK) {
            return status;
        }
        if (exited) {
            return GW_OK;
        }
        nanosleep(&pause_time, NULL);
    }

    status = gw_process_poll_exit(process, &exited, error);
    if (status != GW_OK) {
        return status;
    }
    if (exited) {
        return GW_OK;
    }
    /* Escalate only after the graceful deadline expires. */
    if (kill(-process->pid, SIGKILL) < 0 && errno != ESRCH) {
        gw_error_set(error, GW_ERR_IO, "cannot kill worker process group: %s",
                  strerror(errno));
        return GW_ERR_IO;
    }
    status = wait_after_kill(process, error);
    if (status == GW_OK) {
        gw_error_clear(error);
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
        gw_error_set(error, GW_ERR_ARGUMENT, "worker process is required");
        return GW_ERR_ARGUMENT;
    }
    if (process->running) {
        gw_error_set(error, GW_ERR_VALIDATION, "cannot close a running worker process");
        return GW_ERR_VALIDATION;
    }
    close_fd(&process->stdout_fd);
    close_fd(&process->stderr_fd);
    process->pid = -1;
    gw_error_clear(error);
    return GW_OK;
}
