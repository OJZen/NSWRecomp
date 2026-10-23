#include "time_clock.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static int64_t signed64(const uint8_t *p) { int64_t result; memcpy(&result, p, 8); return result; }

int main(void) {
    TimeClock clock = {0}; TimePoint point; int64_t result = 123;
    uint8_t source[16]; for (unsigned i = 0; i < 16; i++) source[i] = i + 1;
    assert(!time_clock_current(&clock, 0, &result) && result == 123);
    assert(!time_clock_initialize(&clock, -1, 0, source));
    assert(!time_clock_initialize(&clock, 1000000000, INT64_MIN, source));
    assert(!clock.initialized);
    const int64_t anchor = 1234999999999ll, epoch = 1700000000;
    assert(time_clock_initialize(&clock, anchor, epoch, source));
    assert(!time_clock_initialize(&clock, anchor, epoch + 86400, source));
    uint8_t original[0x1000]; memcpy(original, clock.shared, sizeof original);
    assert(!signed64(clock.shared) && !signed64(clock.shared + 0x38) && !signed64(clock.shared + 0x80));
    assert(!signed64(clock.shared + 8));
    assert(!memcmp(clock.shared + 8, clock.shared + 0x20, 0x18));
    assert(!memcmp(clock.shared + 0x40, clock.shared + 0x60, 0x20));
    assert(!memcmp(clock.shared + 0x40, clock.shared + 0x88, 0x20));
    assert(!memcmp(clock.shared + 0x88, clock.shared + 0xa8, 0x20));
    for (unsigned i = 0xc8; i < 0x100; i++) assert(!clock.shared[i]);
    assert(!memcmp(clock.shared + 0xd8, clock.shared + 0x110, 0x38));
    for (unsigned i = 0; i < 3; i++) {
        size_t offset = (size_t[]){0x10, 0x50, 0x100}[i];
        assert(!memcmp(clock.shared + offset, source, 16));
    }
    for (unsigned i = 0; i < 4; i++) {
        int64_t elapsed = (int64_t[]){0, 1, 5000000000ll, 86400000000000ll}[i];
        int64_t now = anchor + elapsed;
        assert(time_clock_point(&clock, now, &point));
        assert(point.seconds == now / 1000000000 && !memcmp(point.source, source, 16));
        // Independent SDK/libnx shared-memory reader: base is in nanoseconds,
        // system offset is in seconds, and both refer to the same source.
        int64_t shared_time = (signed64(clock.shared + 8) + now) / 1000000000 + signed64(clock.shared + 0x40);
        assert(time_clock_current(&clock, now, &result) && result == shared_time);
        assert(result == epoch + now / 1000000000 - anchor / 1000000000);
    }
    assert(!memcmp(original, clock.shared, sizeof original));
    result = 123;
    assert(!time_clock_current(&clock, anchor - 1, &result) && result == 123);
    TimeClock overflow = {0};
    assert(time_clock_initialize(&overflow, 0, INT64_MAX, source));
    assert(!time_clock_current(&overflow, 1000000000, &result) && result == 123);
    TimeClock negative = {0};
    assert(time_clock_initialize(&negative, 1000000000, -3600, source));
    assert(time_clock_current(&negative, 2000000000, &result) && result == -3599);
    assert(!time_clock_point(NULL, 0, &point));
    assert(!time_clock_current(&clock, anchor, NULL));
    return 0;
}
