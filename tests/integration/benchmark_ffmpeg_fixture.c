#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        stop_requested = 1;
    }
}

static int record_arguments(int argc, char **argv)
{
    const char *log_path = getenv("GW_BENCH_FIXTURE_LOG");
    FILE *stream;
    int index;

    if (log_path == NULL) {
        return 0;
    }
    stream = fopen(log_path, "a");
    if (stream == NULL) {
        return -1;
    }
    for (index = 1; index < argc; index++) {
        if (index > 1 && fputc(' ', stream) == EOF) {
            fclose(stream);
            return -1;
        }
        if (fputs(argv[index], stream) == EOF) {
            fclose(stream);
            return -1;
        }
    }
    if (fputc('\n', stream) == EOF || fclose(stream) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction action;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
    bool validation = false;
    bool decode = false;
    int index;

    if (record_arguments(argc, argv) != 0) {
        return 2;
    }
    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-t") == 0) {
            validation = true;
        }
        if (index > 1 && strcmp(argv[index - 1], "-v") == 0 &&
            strcmp(argv[index], "error") == 0) {
            decode = true;
        }
    }
    if (decode) {
        return 0;
    }
    if (validation) {
        FILE *output;

        if (argc < 2) {
            return 3;
        }
        output = fopen(argv[argc - 1], "wb");
        if (output == NULL) {
            return 3;
        }
        if (fputs("fixture\n", output) == EOF || fclose(output) != 0) {
            return 3;
        }
        return 0;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return 4;
    }
    while (!stop_requested) {
        nanosleep(&pause, NULL);
    }
    return 255;
}
