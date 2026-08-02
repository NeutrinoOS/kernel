#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
int neutrino_socket_is_fd(int fd);
ssize_t neutrino_socket_read(int fd, void* buffer, size_t length);
ssize_t neutrino_socket_write(int fd, const void* buffer, size_t length);
int neutrino_socket_close(int fd);
int neutrino_socket_fcntl(int fd, int command, int value);
int neutrino_socket_poll_handle(int fd, uint32_t* handle, int* connected);
#ifdef __cplusplus
}
#endif

