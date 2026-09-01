#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int claim_recovery_failure(const char *environment_name)
{
    const char *marker_path = getenv(environment_name);
    int descriptor;

    if (marker_path == NULL || marker_path[0] == '\0') {
        return -1;
    }
    descriptor = open(marker_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor >= 0) {
        close(descriptor);
        return 1;
    }
    return errno == EEXIST ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        const char *mode = getenv("GW_FIXTURE_MODE");
        const char *input_url = NULL;
        const char *output_url = argv[argc - 1];
        const struct timespec progress_interval = {
            .tv_sec = 0,
            .tv_nsec = 200000000L
        };
        int input_recovery_attempt = -2;
        int publish_recovery_attempt = -2;
        int index;

        for (index = 1; index + 1 < argc; ++index) {
            if (strcmp(argv[index], "-i") == 0) {
                input_url = argv[index + 1];
                break;
            }
        }
        if (input_url != NULL && strstr(input_url, "/recover-once") != NULL) {
            input_recovery_attempt =
                claim_recovery_failure("GW_FIXTURE_RECOVERY_FILE");
            if (input_recovery_attempt < 0) {
                fprintf(stderr, "fixture input recovery marker is unavailable\n");
                return 10;
            }
        }
        if (strstr(output_url, "/publish-recover-once") != NULL) {
            publish_recovery_attempt =
                claim_recovery_failure("GW_FIXTURE_PUBLISH_RECOVERY_FILE");
            if (publish_recovery_attempt < 0) {
                fprintf(stderr,
                        "fixture publish recovery marker is unavailable\n");
                return 10;
            }
        }
        if (mode != NULL && strcmp(mode, "no-progress") == 0) {
            for (;;) {
                pause();
            }
        }
        if (mode != NULL && strcmp(mode, "steady") == 0) {
            int frame;

            for (frame = 1; frame <= 8; ++frame) {
                printf("frame=%d\nfps=25.0\nbitrate=4000kbits/s\n"
                       "out_time_us=%d00000\ndrop_frames=0\nspeed=1.0x\n"
                       "progress=continue\n",
                       frame, frame * 2);
                fflush(stdout);
                nanosleep(&progress_interval, NULL);
            }
        }
        printf("frame=42\nfps=25.0\nbitrate=4000kbits/s\n"
               "out_time_us=1680000\ndrop_frames=0\nspeed=1.0x\nprogress=end\n");
        fflush(stdout);
        if ((mode != NULL && strcmp(mode, "stall") == 0) ||
            (input_url != NULL && strstr(input_url, "/hold") != NULL) ||
            input_recovery_attempt == 0 || publish_recovery_attempt == 0) {
            for (;;) {
                pause();
            }
        }
        fprintf(stderr, "fixture FFmpeg diagnostic\n");
        if (input_url != NULL) {
            fprintf(stderr, "fixture input=%s\n", input_url);
        }
        return (mode != NULL && strcmp(mode, "fail") == 0) ||
                       (input_url != NULL &&
                        strstr(input_url, "/worker-fail") != NULL) ||
                       input_recovery_attempt == 1 ||
                       publish_recovery_attempt == 1
                   ? 9
                   : 0;
    }
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "emit") == 0) {
        printf("frame=42\nfps=25.0\nprogress=end\n");
        fprintf(stderr, "fixture diagnostic\n");
        return 7;
    }
    if (strcmp(argv[1], "wait") == 0) {
        for (;;) {
            pause();
        }
    }
    if (strcmp(argv[1], "ignore-term") == 0) {
        signal(SIGTERM, SIG_IGN);
        printf("ready\n");
        fflush(stdout);
        for (;;) {
            pause();
        }
    }
    return 2;
}
