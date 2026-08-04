#pragma once

#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

void sync(void);
int brk(void* address);

#ifdef __cplusplus
}
#endif
