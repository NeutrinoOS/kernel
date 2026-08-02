#pragma once

#include <sys/socket.h>

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

#define AI_PASSIVE 0x01
#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x0400

#define EAI_BADFLAGS -1
#define EAI_NONAME -2
#define EAI_AGAIN -3
#define EAI_FAIL -4
#define EAI_FAMILY -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE -8
#define EAI_MEMORY -10
#define EAI_SYSTEM -11
#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_MAXHOST 1025
#define NI_MAXSERV 32

#ifdef __cplusplus
extern "C" {
#endif
int getaddrinfo(const char* node, const char* service,
                const struct addrinfo* hints, struct addrinfo** result);
void freeaddrinfo(struct addrinfo* result);
const char* gai_strerror(int error);
int getnameinfo(const struct sockaddr* address, socklen_t address_len,
                char* host, socklen_t host_len, char* service,
                socklen_t service_len, int flags);
#ifdef __cplusplus
}
#endif
