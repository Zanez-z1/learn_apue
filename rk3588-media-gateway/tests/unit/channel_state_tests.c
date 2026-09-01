#include "gateway/channel_state.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static gw_retry_policy retry_policy(int max_retries, int max_backoff_sec)
{
    gw_retry_policy policy = {0};
    policy.max_retries = max_retries;
    policy.max_backoff_sec = max_backoff_sec;
    return policy;
}

static void test_happy_path(void)
{
    gw_retry_policy policy = retry_policy(3, 30);
    gw_channel_runtime runtime;
    gw_error error = {0};

    gw_channel_runtime_init(&runtime, true);
    CHECK(runtime.state == GW_CHANNEL_STOPPED);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_STARTING);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_PROGRESS, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_RUNNING);
    runtime.consecutive_failures = 2U;
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STABLE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.consecutive_failures == 0U);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_STOP, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_STOPPED);
}

static void test_backoff_and_exhaustion(void)
{
    gw_retry_policy policy = retry_policy(3, 3);
    gw_channel_runtime runtime;
    gw_error error = {0};
    int expected_backoff[] = {1, 2, 3};
    size_t attempt;

    gw_channel_runtime_init(&runtime, true);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START, &policy, &error) ==
          GW_OK);
    for (attempt = 0U; attempt < 3U; ++attempt) {
        CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE, &policy,
                                    &error) == GW_OK);
        CHECK(runtime.state == GW_CHANNEL_BACKOFF);
        CHECK(runtime.backoff_sec == expected_backoff[attempt]);
        CHECK(runtime.consecutive_failures == attempt + 1U);
        CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_BACKOFF_ELAPSED,
                                    &policy, &error) == GW_OK);
    }
    CHECK(runtime.total_restarts == 3U);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_FAILED);
    CHECK(runtime.consecutive_failures == 4U);
    CHECK(runtime.backoff_sec == 3);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_BACKOFF_ELAPSED,
                                &policy, &error) == GW_OK);
    CHECK(runtime.state == GW_CHANNEL_STARTING);
    CHECK(runtime.total_restarts == 4U);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE, &policy,
                                &error) == GW_OK);
    CHECK(runtime.state == GW_CHANNEL_FAILED);
    CHECK(runtime.consecutive_failures == 5U);
    CHECK(runtime.backoff_sec == 3);
}

static void test_zero_retries(void)
{
    gw_retry_policy policy = retry_policy(0, 30);
    gw_channel_runtime runtime;
    gw_error error = {0};

    gw_channel_runtime_init(&runtime, true);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START, &policy, &error) ==
          GW_OK);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_FAILED);
    CHECK(runtime.backoff_sec == 30);
    CHECK(runtime.total_restarts == 0U);
}

static void test_disable_and_enable(void)
{
    gw_retry_policy policy = retry_policy(3, 30);
    gw_channel_runtime runtime;
    gw_error error = {0};

    gw_channel_runtime_init(&runtime, false);
    CHECK(runtime.state == GW_CHANNEL_DISABLED);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_ENABLE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_STOPPED);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_DISABLE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_DISABLED);
}

static void test_invalid_transition_and_manual_reset(void)
{
    gw_retry_policy policy = retry_policy(0, 30);
    gw_channel_runtime runtime;
    gw_error error = {0};

    gw_channel_runtime_init(&runtime, true);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_PROGRESS, &policy, &error) ==
          GW_ERR_VALIDATION);
    CHECK(strstr(error.message, "STOPPED") != NULL);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START, &policy, &error) ==
          GW_OK);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_FAILURE, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_FAILED);
    CHECK(gw_channel_transition(&runtime, GW_CHANNEL_EVENT_START, &policy, &error) ==
          GW_OK);
    CHECK(runtime.state == GW_CHANNEL_STARTING);
    CHECK(runtime.consecutive_failures == 0U);
}

int main(void)
{
    test_happy_path();
    test_backoff_and_exhaustion();
    test_zero_retries();
    test_disable_and_enable();
    test_invalid_transition_and_manual_reset();

    if (failures == 0) {
        printf("All channel state tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d channel state test(s) failed.\n", failures);
    return 1;
}
