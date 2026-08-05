#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int is_probe_command(int argc, char **argv)
{
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-show_entries") == 0) {
            return 1;
        }
    }
    return 0;
}

static int run_probe_fixture(int argc, char **argv)
{
    const char *mode = getenv("GW_PROBE_FIXTURE_MODE");

    if (mode != NULL && strcmp(mode, "timeout") == 0) {
        for (;;) {
            pause();
        }
    }
    if (mode != NULL && strcmp(mode, "fail") == 0) {
        fprintf(stderr, "fixture ffprobe failure input=%s\n", argv[argc - 1]);
        return 6;
    }
    if (mode != NULL && strcmp(mode, "invalid") == 0) {
        printf("codec_name=h264\nwidth=invalid\nheight=1080\n");
        return 0;
    }
    printf("codec_name=%s\nwidth=1920\nheight=1080\n",
           mode != NULL && strcmp(mode, "mismatch") == 0 ? "hevc" : "h264");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        const char *mode = getenv("GW_FIXTURE_MODE");
        const struct timespec progress_interval = {
            .tv_sec = 0,
            .tv_nsec = 200000000L
        };
        int index;

        if (is_probe_command(argc, argv)) {
            return run_probe_fixture(argc, argv);
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
