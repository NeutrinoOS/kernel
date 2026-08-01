#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

int poll(struct pollfd* descriptors, nfds_t count, int timeout);

#ifdef __cplusplus
}
#endif
