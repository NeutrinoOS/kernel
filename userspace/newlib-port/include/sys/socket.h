#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    unsigned char __data[126];
};

#define AF_UNSPEC 0
#define AF_INET 2
#define PF_UNSPEC AF_UNSPEC
#define PF_INET AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_TYPE 3
#define SO_NOSIGPIPE 0x1022

#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#ifdef __cplusplus
extern "C" {
#endif
int socket(int domain, int type, int protocol);
int connect(int socket, const struct sockaddr* address, socklen_t address_len);
int bind(int socket, const struct sockaddr* address, socklen_t address_len);
int listen(int socket, int backlog);
int accept(int socket, struct sockaddr* address, socklen_t* address_len);
ssize_t send(int socket, const void* buffer, size_t length, int flags);
ssize_t recv(int socket, void* buffer, size_t length, int flags);
int shutdown(int socket, int how);
int getsockopt(int socket, int level, int option, void* value, socklen_t* value_len);
int setsockopt(int socket, int level, int option, const void* value, socklen_t value_len);
int getpeername(int socket, struct sockaddr* address, socklen_t* address_len);
int getsockname(int socket, struct sockaddr* address, socklen_t* address_len);
#ifdef __cplusplus
}
#endif
