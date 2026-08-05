#define _POSIX_C_SOURCE 200809L

#include "gateway/process_manager.h"

#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static void pause_briefly(void)
{
    const struct timespec duration = {.tv_sec = 0, .tv_nsec = 10000000L};
    nanosleep(&duration, NULL);
}

static int count_open_descriptors(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

static void append_output(char *destination, size_t capacity, size_t *length,
                          const char *data, size_t data_length)
{
    if (*length + data_length >= capacity) {
        ++failures;
        return;
    }
    memcpy(destination + *length, data, data_length);
    *length += data_length;
    destination[*length] = '\0';
}

static void drain_stream(gw_process *process, gw_process_stream stream,
                         char *output, size_t capacity, size_t *length,
                         bool *finished)
{
    char buffer[128];
    size_t bytes_read;
    bool end_of_stream;
    gw_error error = {0};
    gw_status status;

    status = gw_process_read(process, stream, buffer, sizeof(buffer), &bytes_read,
                             &end_of_stream, &error);
    CHECK(status == GW_OK);
    if (status != GW_OK) {
        return;
    }
    append_output(output, capacity, length, buffer, bytes_read);
    if (end_of_stream) {
        *finished = true;
    }
}

static void test_output_and_exit(const char *fixture)
{
    char *arguments[] = {(char *)fixture, "emit", NULL};
    gw_process process;
    gw_error error = {0};
    char stdout_text[512] = {0};
    char stderr_text[512] = {0};
    size_t stdout_length = 0U;
    size_t stderr_length = 0U;
    bool stdout_finished = false;
    bool stderr_finished = false;
    bool exited = false;
    int iterations;

    gw_process_init(&process);
    CHECK(gw_process_start(&process, arguments, &error) == GW_OK);
    CHECK(process.pid > 0);

    for (iterations = 0; iterations < 200; ++iterations) {
        drain_stream(&process, GW_PROCESS_STDOUT, stdout_text, sizeof(stdout_text),
                     &stdout_length, &stdout_finished);
        drain_stream(&process, GW_PROCESS_STDERR, stderr_text, sizeof(stderr_text),
                     &stderr_length, &stderr_finished);
        CHECK(gw_process_poll_exit(&process, &exited, &error) == GW_OK);
        if (exited && stdout_finished && stderr_finished) {
            break;
        }
        pause_briefly();
    }

    CHECK(exited);
    CHECK(stdout_finished);
    CHECK(stderr_finished);
    CHECK(gw_process_exit_code(&process) == 7);
    CHECK(strstr(stdout_text, "frame=42") != NULL);
    CHECK(strstr(stdout_text, "progress=end") != NULL);
    CHECK(strstr(stderr_text, "fixture diagnostic") != NULL);
    CHECK(gw_process_close(&process, &error) == GW_OK);
}

static void test_term_stop(const char *fixture)
{
    char *arguments[] = {(char *)fixture, "wait", NULL};
    gw_process process;
    gw_error error = {0};

    gw_process_init(&process);
    CHECK(gw_process_start(&process, arguments, &error) == GW_OK);
    CHECK(gw_process_stop(&process, 1000, &error) == GW_OK);
    CHECK(gw_process_exit_code(&process) == 128 + SIGTERM);
    CHECK(gw_process_close(&process, &error) == GW_OK);
}

static void test_kill_after_timeout(const char *fixture)
{
    char *arguments[] = {(char *)fixture, "ignore-term", NULL};
    gw_process process;
    gw_error error = {0};
    char output[64];
    size_t bytes_read = 0U;
    bool end_of_stream;
    int iterations;

    gw_process_init(&process);
    CHECK(gw_process_start(&process, arguments, &error) == GW_OK);
    for (iterations = 0; iterations < 100 && bytes_read == 0U; ++iterations) {
        CHECK(gw_process_read(&process, GW_PROCESS_STDOUT, output, sizeof(output),
                              &bytes_read, &end_of_stream, &error) == GW_OK);
        if (bytes_read == 0U) {
            pause_briefly();
        }
    }
    CHECK(bytes_read >= strlen("ready\n"));
    CHECK(memcmp(output, "ready\n", strlen("ready\n")) == 0);
    CHECK(gw_process_stop(&process, 30, &error) == GW_OK);
    CHECK(gw_process_exit_code(&process) == 128 + SIGKILL);
    CHECK(gw_process_close(&process, &error) == GW_OK);
}

static void test_start_failure(void)
{
    char *arguments[] = {"gateway-command-that-does-not-exist", NULL};
    gw_process process;
    gw_error error = {0};
    int descriptors_before = count_open_descriptors();
    int descriptors_after;
    int iteration;

    for (iteration = 0; iteration < 50; ++iteration) {
        gw_process_init(&process);
        CHECK(gw_process_start(&process, arguments, &error) == GW_ERR_IO);
        CHECK(process.pid == -1);
        CHECK(!process.running);
    }
    descriptors_after = count_open_descriptors();
    CHECK(descriptors_before >= 0);
    CHECK(descriptors_after == descriptors_before);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "fixture path is required\n");
        return 2;
    }
    test_output_and_exit(argv[1]);
    test_term_stop(argv[1]);
    test_kill_after_timeout(argv[1]);
    test_start_failure();

    if (failures == 0) {
        printf("All process manager integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d process manager test(s) failed.\n", failures);
    return 1;
}
