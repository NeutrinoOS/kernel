#pragma once

#include_next <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

int nanosleep(const struct timespec* duration, struct timespec* remaining);
int clock_gettime(clockid_t clock_id, struct timespec* value);

#ifdef __cplusplus
}
#endif
