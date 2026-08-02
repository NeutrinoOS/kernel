#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
ssize_t getrandom(void* buffer, size_t length, unsigned flags);
#ifdef __cplusplus
}
#endif

