#include "network_time.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

static void unavailable(const network_time_anchor_t *anchor, int64_t monotonic_us)
{
    int64_t seconds = INT64_C(1800000000);
    assert(!network_time_read(anchor, monotonic_us, &seconds));
    assert(seconds == 0);
    llt_clock_t sample = {.seconds = INT64_C(1800000000), .trusted = true,
                          .reference_seconds = INT64_C(1800000000), .age_us = 7};
    assert(!network_time_sample(anchor, monotonic_us, &sample));
    assert(!sample.trusted && sample.seconds == 0 && sample.microseconds == 0 &&
           sample.reference_seconds == 0 && sample.reference_microseconds == 0 && sample.age_us == 0);
}

static void equals(const network_time_anchor_t *anchor, int64_t monotonic_us, int64_t expected)
{
    int64_t seconds = 0;
    assert(network_time_read(anchor, monotonic_us, &seconds));
    assert(seconds == expected);
}

int main(void)
{
    network_time_anchor_t anchor = {0};
    unavailable(&anchor, 0);
    unavailable(&anchor, INT64_C(1800000000)); /* A plausible value is not sync evidence. */
    unavailable(NULL, 0);
    assert(!network_time_read(&anchor, 0, NULL));
    assert(!network_time_sample(&anchor, 0, NULL));
    assert(!network_time_capture(NULL, INT64_C(1800000000), 0, 0));

    /* Fractional UTC must carry across a second without rounding early. */
    assert(network_time_capture(&anchor, INT64_C(1800000000), 999999, 0));
    equals(&anchor, 0, INT64_C(1800000000));
    equals(&anchor, 1, INT64_C(1800000001));
    equals(&anchor, INT64_C(1000000), INT64_C(1800000001));
    equals(&anchor, INT64_C(1000001), INT64_C(1800000002));
    llt_clock_t sample;
    assert(network_time_sample(&anchor, 1, &sample));
    assert(sample.trusted && sample.seconds == INT64_C(1800000001) && sample.microseconds == 0);
    assert(sample.reference_seconds == INT64_C(1800000000) &&
           sample.reference_microseconds == 999999 && sample.age_us == 1);
    assert(network_time_sample(&anchor, INT64_C(1000124), &sample));
    assert(sample.seconds == INT64_C(1800000002) && sample.microseconds == 123);
    assert(sample.reference_seconds == INT64_C(1800000000) &&
           sample.reference_microseconds == 999999 && sample.age_us == INT64_C(1000124));
    equals(&anchor, NETWORK_TIME_MAX_AGE_US, INT64_C(1800086400));
    assert(network_time_sample(&anchor, NETWORK_TIME_MAX_AGE_US, &sample));
    assert(sample.reference_seconds == INT64_C(1800000000) &&
           sample.reference_microseconds == 999999 && sample.age_us == (uint64_t)NETWORK_TIME_MAX_AGE_US);
    unavailable(&anchor, NETWORK_TIME_MAX_AGE_US + 1);

    /* A received resync replaces a stale anchor; mere reads cannot renew it. */
    assert(network_time_capture(&anchor, INT64_C(1800086401), 0,
                                NETWORK_TIME_MAX_AGE_US + 1));
    equals(&anchor, NETWORK_TIME_MAX_AGE_US + 1, INT64_C(1800086401));
    assert(network_time_sample(&anchor, NETWORK_TIME_MAX_AGE_US + 1, &sample));
    assert(sample.reference_seconds == INT64_C(1800086401) && sample.reference_microseconds == 0 &&
           sample.age_us == 0);
    unavailable(&anchor, NETWORK_TIME_MAX_AGE_US);
    assert(!network_time_capture(&anchor, INT64_C(1800086401), 0,
                                 NETWORK_TIME_MAX_AGE_US));
    unavailable(&anchor, NETWORK_TIME_MAX_AGE_US + 2);

    /* NTP's 2036 era rollover and Unix's 2038 boundary stay 64-bit. */
    assert(network_time_capture(&anchor, INT64_C(2085978495), 500000, INT64_C(4294967295)));
    equals(&anchor, INT64_C(4295467295), INT64_C(2085978496));
    assert(network_time_capture(&anchor, INT64_C(2147483647), 0, INT64_C(5000000000)));
    equals(&anchor, INT64_C(5001000000), INT64_C(2147483648));

    /* Huge monotonic clocks do not wrap subtraction or freshness into success. */
    assert(network_time_capture(&anchor, INT64_C(1800000000), 0, INT64_MAX - 1000000));
    equals(&anchor, INT64_MAX, INT64_C(1800000001));
    unavailable(&anchor, INT64_MIN);
    anchor = (network_time_anchor_t){0};
    assert(network_time_capture(&anchor, INT64_C(1800000000), 0, 0));
    unavailable(&anchor, INT64_MAX);

    const int64_t bad_seconds[] = {INT64_MIN, -1, 0, NETWORK_TIME_MIN_UNIX - 1,
                                   NETWORK_TIME_MAX_UNIX + 1, INT64_MAX};
    for (unsigned i = 0; i < sizeof(bad_seconds) / sizeof(bad_seconds[0]); ++i) {
        assert(network_time_capture(&anchor, INT64_C(1800000000), 0, 1));
        assert(!network_time_capture(&anchor, bad_seconds[i], 0, 2));
        unavailable(&anchor, 3); /* An invalid notification invalidates prior evidence. */
    }
    assert(!network_time_capture(&anchor, INT64_C(1800000000), -1, 0));
    assert(!network_time_capture(&anchor, INT64_C(1800000000), 1000000, 0));
    assert(!network_time_capture(&anchor, INT64_C(1800000000), INT64_MAX, 0));
    assert(!network_time_capture(&anchor, INT64_C(1800000000), 0, -1));
    assert(network_time_capture(&anchor, NETWORK_TIME_MIN_UNIX, 0, 0));
    equals(&anchor, 0, NETWORK_TIME_MIN_UNIX);
    assert(network_time_capture(&anchor, NETWORK_TIME_MAX_UNIX, 999999, 0));
    equals(&anchor, 0, NETWORK_TIME_MAX_UNIX);
    unavailable(&anchor, 1); /* Extrapolation cannot cross the accepted UTC bound. */

    /* A reboot loses synchronization, even if an RTC retains a plausible date. */
    anchor = (network_time_anchor_t){0};
    unavailable(&anchor, 0);
    unavailable(&anchor, INT64_C(100000000000));
    puts("PASS received-time evidence, expiry, fractional carry, era boundaries and fail-closed reset");
}
