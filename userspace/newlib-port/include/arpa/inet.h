#pragma once

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif
int inet_aton(const char* text, struct in_addr* address);
in_addr_t inet_addr(const char* text);
const char* inet_ntop(int family, const void* address, char* output, socklen_t size);
int inet_pton(int family, const char* text, void* address);
#ifdef __cplusplus
}
#endif
