#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc > 2) {
        const char *mode = getenv("GW_FIXTURE_MODE");
        const struct timespec progress_interval = {
            .tv_sec = 0,
            .tv_nsec = 200000000L
        };
        int index;

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
        if (mode != NULL && strcmp(mode, "stall") == 0) {
            for (;;) {
                pause();
            }
        }
        fprintf(stderr, "fixture FFmpeg diagnostic\n");
        for (index = 1; index + 1 < argc; ++index) {
            if (strcmp(argv[index], "-i") == 0) {
                fprintf(stderr, "fixture input=%s\n", argv[index + 1]);
                break;
            }
        }
        return mode != NULL && strcmp(mode, "fail") == 0 ? 9 : 0;
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
