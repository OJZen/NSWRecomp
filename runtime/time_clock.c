// Shared-memory layout adapted from runtime/services.c (Zelda-Mobile,
// Apache-2.0). See third_party/zelda-mobile/LICENSE and SOURCE.json.
#include "time_clock.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(TimePoint) == 0x18, "steady clock wire size");
_Static_assert(sizeof(TimeSystemContext) == 0x20, "system clock wire size");
_Static_assert(offsetof(TimeSystemContext, timestamp.source) == 0x10, "clock source wire offset");

bool time_clock_initialize(TimeClock *clock, int64_t steady_ns, int64_t posix_seconds,
                           const uint8_t source[16]) {
    if (!clock || !source || clock->initialized || steady_ns < 0) return false;
    int64_t seconds = steady_ns / 1000000000;
    __int128 offset = (__int128)posix_seconds - seconds;
    if (offset < INT64_MIN || offset > INT64_MAX) return false;
    TimeClock candidate = {.anchor_ns = steady_ns, .initialized = true,
        .system = {.offset = (int64_t)offset, .timestamp = {.seconds = seconds}}};
    memcpy(candidate.system.timestamp.source, source, 16);
    // Double-buffer counters are zero; both slots contain the same immutable
    // anchor. The steady base is zero nanoseconds, matching the host tick origin.
    TimePoint steady = {0}; memcpy(steady.source, source, 16);
    memcpy(candidate.shared + 8, &steady, sizeof steady);
    memcpy(candidate.shared + 0x20, &steady, sizeof steady);
    for (size_t base = 0x38; base <= 0x80; base += 0x48) {
        memcpy(candidate.shared + base + 8, &candidate.system, sizeof candidate.system);
        memcpy(candidate.shared + base + 0x28, &candidate.system, sizeof candidate.system);
    }
    // Automatic correction is disabled at 0xc8. Identity continuous adjustment
    // at 0xd0 uses the same source, with all five nanosecond/rate fields zero.
    struct { int64_t point, rate, shift, lower, upper; uint8_t source[16]; } adjustment = {0};
    _Static_assert(sizeof adjustment == 0x38, "continuous adjustment wire size");
    memcpy(adjustment.source, source, 16);
    memcpy(candidate.shared + 0xd8, &adjustment, sizeof adjustment);
    memcpy(candidate.shared + 0x110, &adjustment, sizeof adjustment);
    *clock = candidate;
    return true;
}

bool time_clock_point(const TimeClock *clock, int64_t steady_ns, TimePoint *point) {
    if (!clock || !clock->initialized || !point || steady_ns < clock->anchor_ns) return false;
    *point = clock->system.timestamp;
    point->seconds = steady_ns / 1000000000;
    return true;
}

bool time_clock_current(const TimeClock *clock, int64_t steady_ns, int64_t *seconds) {
    TimePoint point;
    if (!seconds || !time_clock_point(clock, steady_ns, &point)) return false;
    __int128 value = (__int128)clock->system.offset + point.seconds;
    if (value < INT64_MIN || value > INT64_MAX) return false;
    *seconds = (int64_t)value;
    return true;
}
