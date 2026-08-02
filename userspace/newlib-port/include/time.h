#pragma once

#include_next <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
// Keep this distinct from Newlib's CLOCK_REALTIME value of 1.  This matches
// the CLOCK_MONOTONIC value Newlib exposes when POSIX monotonic clocks are
// enabled.
#define CLOCK_MONOTONIC 4
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 5
#endif

#if CLOCK_REALTIME == CLOCK_MONOTONIC
#error "CLOCK_REALTIME and CLOCK_MONOTONIC must have distinct IDs"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int nanosleep(const struct timespec* duration, struct timespec* remaining);
int clock_gettime(clockid_t clock_id, struct timespec* value);

#ifdef __cplusplus
}
#endif
