#pragma once

#include_next <sys/resource.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

#define RLIM_INFINITY ((rlim_t)UINT64_MAX)

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9

int getrlimit(int resource, struct rlimit* limits);
int setrlimit(int resource, const struct rlimit* limits);

#ifdef __cplusplus
}
#endif
