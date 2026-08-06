#include "gateway/process_metrics.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static void test_stat_parser(void)
{
    const char *stat_line =
        "123 (gateway worker) S 1 2 3 4 5 6 7 8 9 10 100 50 0\n";
    const char *paren_line =
        "123 (gateway)worker) S 1 2 3 4 5 6 7 8 9 10 7 11 0\n";
    const char *negative_terminal_line =
        "123 (gateway) S 1 2 3 0 -1 6 7 8 9 10 20 30 0\n";
    const char *negative_cpu_line =
        "123 (gateway) S 1 2 3 4 5 6 7 8 9 10 -1 30 0\n";
    uint64_t ticks = 0U;

    CHECK(gw_process_metrics_parse_stat(stat_line, &ticks) == 0);
    CHECK(ticks == 150U);
    CHECK(gw_process_metrics_parse_stat(paren_line, &ticks) == 0);
    CHECK(ticks == 18U);
    CHECK(gw_process_metrics_parse_stat(negative_terminal_line, &ticks) == 0);
    CHECK(ticks == 50U);
    CHECK(gw_process_metrics_parse_stat("invalid", &ticks) != 0);
    CHECK(gw_process_metrics_parse_stat(negative_cpu_line, &ticks) != 0);
    CHECK(gw_process_metrics_parse_stat(stat_line, NULL) != 0);
}

static void test_status_parser(void)
{
    const char *status = "Name:\tgatewayd\nState:\tS\nVmRSS:\t  2048 kB\n";
    long rss_kib = -1L;

    CHECK(gw_process_metrics_parse_status(status, &rss_kib) == 0);
    CHECK(rss_kib == 2048L);
    CHECK(gw_process_metrics_parse_status("VmRSS: 2 MB\n", &rss_kib) != 0);
    CHECK(gw_process_metrics_parse_status("Name: gatewayd\n", &rss_kib) != 0);
    CHECK(gw_process_metrics_parse_status(status, NULL) != 0);
}

static void test_live_process(void)
{
    gw_process_metrics metrics;

    CHECK(gw_process_metrics_read(getpid(), &metrics) == 0);
    CHECK(metrics.rss_kib >= 0L);
    CHECK(metrics.fd_count >= 3U);
    CHECK(gw_process_metrics_read((pid_t)-1, &metrics) != 0);
    CHECK(gw_process_metrics_read(getpid(), NULL) != 0);
}

int main(void)
{
    test_stat_parser();
    test_status_parser();
    test_live_process();

    if (failures != 0) {
        fprintf(stderr, "%d process metrics test(s) failed\n", failures);
        return 1;
    }
    printf("process metrics tests passed\n");
    return 0;
}
