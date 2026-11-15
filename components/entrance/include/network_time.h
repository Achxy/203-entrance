#pragma once
#include "lighting_local_time.h"
#include <stdbool.h>
#include <stdint.h>

#define NETWORK_TIME_MIN_UNIX INT64_C(1700000000)
#define NETWORK_TIME_MAX_UNIX INT64_C(4102444800)
#define NETWORK_TIME_MAX_AGE_US INT64_C(86400000000)

typedef struct {
    int64_t unix_seconds, monotonic_us;
    int32_t unix_microseconds;
    bool valid;
} network_time_anchor_t;

/* Evidence is a received SNTP sample from this boot, never a plausible RTC.
 * Invalid notifications invalidate the old anchor rather than keep serving it.
 * Network time is unauthenticated and is not a clock attestation. */
static inline bool network_time_capture(network_time_anchor_t *anchor, int64_t unix_seconds,
                                        int64_t unix_microseconds, int64_t monotonic_us)
{
    if (!anchor)
        return false;
    if (unix_seconds < NETWORK_TIME_MIN_UNIX || unix_seconds > NETWORK_TIME_MAX_UNIX ||
        unix_microseconds < 0 || unix_microseconds >= INT64_C(1000000) || monotonic_us < 0 ||
        (anchor->valid && monotonic_us < anchor->monotonic_us)) {
        *anchor = (network_time_anchor_t){0};
        return false;
    }
    *anchor = (network_time_anchor_t){.unix_seconds = unix_seconds,
        .monotonic_us = monotonic_us, .unix_microseconds = (int32_t)unix_microseconds, .valid = true};
    return true;
}

static inline bool network_time_sample(const network_time_anchor_t *anchor, int64_t monotonic_us,
                                       llt_clock_t *sample)
{
    if (!sample)
        return false;
    *sample = (llt_clock_t){0};
    if (!anchor || !anchor->valid || anchor->unix_seconds < NETWORK_TIME_MIN_UNIX ||
        anchor->unix_seconds > NETWORK_TIME_MAX_UNIX || anchor->unix_microseconds < 0 ||
        anchor->unix_microseconds >= 1000000 || anchor->monotonic_us < 0 ||
        monotonic_us < anchor->monotonic_us)
        return false;
    int64_t elapsed = monotonic_us - anchor->monotonic_us;
    if (elapsed > NETWORK_TIME_MAX_AGE_US)
        return false;
    /* The age bound also keeps both sums far below signed 64-bit overflow. */
    int64_t fractional_elapsed = elapsed + anchor->unix_microseconds;
    int64_t derived = anchor->unix_seconds + fractional_elapsed / 1000000;
    if (derived > NETWORK_TIME_MAX_UNIX)
        return false;
    *sample = (llt_clock_t){.seconds = derived,
        .microseconds = (uint32_t)(fractional_elapsed % 1000000), .trusted = true,
        .reference_seconds = anchor->unix_seconds,
        .reference_microseconds = (uint32_t)anchor->unix_microseconds,
        .age_us = (uint64_t)elapsed};
    return true;
}

static inline bool network_time_read(const network_time_anchor_t *anchor, int64_t monotonic_us,
                                     int64_t *unix_seconds)
{
    if (!unix_seconds)
        return false;
    llt_clock_t sample;
    bool valid = network_time_sample(anchor, monotonic_us, &sample);
    *unix_seconds = valid ? sample.seconds : 0;
    return valid;
}
