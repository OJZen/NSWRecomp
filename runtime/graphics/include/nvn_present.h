#pragma once
#include <math.h>
static inline double zelda_present_deadline(double previous, double now, double period) {
    // Absolute deadlines retain the target rate when the display period is not an integer divisor.
    return fmax(now, previous + period);
}
