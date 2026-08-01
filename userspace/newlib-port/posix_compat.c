#include "neutrino_syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

enum {
    kFileDescriptorOffset = 3,
    kWaitRead = 1u << 0,
    kWaitWrite = 1u << 1,
};

void* mmap(void* address,
           size_t length,
           int protection,
           int flags,
           int fd,
           off_t offset) {
    if (length == 0 || offset < 0 || address != NULL ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 ||
        (protection & (PROT_WRITE | PROT_EXEC)) ==
            (PROT_WRITE | PROT_EXEC) ||
        (flags & MAP_PRIVATE) == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    unsigned map_flags = 0;
    if ((protection & PROT_WRITE) != 0) map_flags |= NEUTRINO_MAP_WRITE;
    if ((protection & PROT_EXEC) != 0) map_flags |= NEUTRINO_MAP_EXECUTE;
    long result;
    if ((flags & MAP_ANONYMOUS) != 0) {
        result = neutrino_raw_syscall2(
            NEUTRINO_MAP_ANONYMOUS, (long)length, map_flags);
    } else {
        if (fd < kFileDescriptorOffset) {
            errno = EBADF;
            return MAP_FAILED;
        }
        result = neutrino_raw_syscall4(
            NEUTRINO_MAP_FILE_PRIVATE,
            fd - kFileDescriptorOffset,
            (long)offset,
            (long)length,
            map_flags);
    }
    if (result < 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void*)(uintptr_t)result;
}

int munmap(void* address, size_t length) {
    if (address == NULL || length == 0 ||
        neutrino_raw_syscall2(NEUTRINO_UNMAP,
                              (long)(uintptr_t)address,
                              (long)length) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int mprotect(void* address, size_t length, int protection) {
    if (address == NULL || length == 0 ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 ||
        (protection & (PROT_WRITE | PROT_EXEC)) ==
            (PROT_WRITE | PROT_EXEC)) {
        errno = EINVAL;
        return -1;
    }
    unsigned map_flags = 0;
    if ((protection & PROT_WRITE) != 0) map_flags |= NEUTRINO_MAP_WRITE;
    if ((protection & PROT_EXEC) != 0) map_flags |= NEUTRINO_MAP_EXECUTE;
    if (neutrino_raw_syscall3(NEUTRINO_PROTECT_MEMORY,
                              (long)(uintptr_t)address,
                              (long)length,
                              map_flags) < 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int madvise(void* address, size_t length, int advice) {
    (void)advice;
    if (address == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int access(const char* path, int mode) {
    struct stat status;
    if (path == NULL || (mode & ~(R_OK | W_OK | X_OK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &status) != 0) return -1;
    if ((mode & X_OK) != 0 && (status.st_mode & 0111) == 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

ssize_t getrandom(void* buffer, size_t length, unsigned flags) {
    if (buffer == NULL || flags != 0) {
        errno = EINVAL;
        return -1;
    }
    long result = neutrino_raw_syscall2(
        NEUTRINO_RANDOM_GET, (long)(uintptr_t)buffer, (long)length);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)result;
}

int sched_getaffinity(pid_t pid, size_t set_size, cpu_set_t* set) {
    if ((pid != 0 && pid != getpid()) || set == NULL || set_size == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(set, 0, set_size);
    ((unsigned char*)set)[0] = 1;
    return 0;
}

int getrusage(int who, struct rusage* usage) {
    if ((who != RUSAGE_SELF && who != RUSAGE_THREAD) || usage == NULL) {
        errno = EINVAL;
        return -1;
    }
    *usage = (struct rusage){0};
    return 0;
}

int fcntl(int fd, int command, ...) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    switch (command) {
        case F_GETFD:
            return 0;
        case F_SETFD:
            return 0;
        case F_GETFL:
            return fd < kFileDescriptorOffset ? O_RDWR : O_RDWR;
        case F_SETFL: {
            va_list arguments;
            va_start(arguments, command);
            int value = va_arg(arguments, int);
            va_end(arguments);
            if ((value & ~(O_NONBLOCK)) != 0) {
                errno = ENOTSUP;
                return -1;
            }
            return 0;
        }
        default:
            errno = ENOSYS;
            return -1;
    }
}

int ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    void* output = va_arg(arguments, void*);
    va_end(arguments);
    if (request == TIOCGWINSZ && output != NULL && fd >= 0 && fd <= 2) {
        struct winsize* size = output;
        *size = (struct winsize){0};
        size->ws_row = 25;
        size->ws_col = 80;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

int poll(struct pollfd* descriptors, nfds_t count, int timeout) {
    if ((descriptors == NULL && count != 0) || timeout < -1) {
        errno = EINVAL;
        return -1;
    }
    int ready = 0;
    struct neutrino_descriptor_wait waits[64];
    size_t wait_count = 0;
    for (nfds_t i = 0; i < count; ++i) {
        descriptors[i].revents = 0;
        if (descriptors[i].fd < 0) continue;
        if (descriptors[i].fd >= kFileDescriptorOffset) {
            descriptors[i].revents = descriptors[i].events & (POLLIN | POLLOUT);
            if (descriptors[i].revents != 0) ++ready;
            continue;
        }
        if (wait_count >= 64) {
            errno = EINVAL;
            return -1;
        }
        waits[wait_count].handle = descriptors[i].fd == 0
            ? NEUTRINO_STDIN
            : descriptors[i].fd == 1 ? NEUTRINO_STDOUT : NEUTRINO_STDERR;
        waits[wait_count].events = 0;
        if ((descriptors[i].events & POLLIN) != 0)
            waits[wait_count].events |= kWaitRead;
        if ((descriptors[i].events & POLLOUT) != 0)
            waits[wait_count].events |= kWaitWrite;
        waits[wait_count].reserved = (uint32_t)i;
        ++wait_count;
    }
    if (ready != 0 || wait_count == 0 || timeout == 0) return ready;
    long result = neutrino_raw_syscall2(
        14, (long)(uintptr_t)waits, (long)wait_count);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    for (size_t i = 0; i < wait_count; ++i) {
        nfds_t index = waits[i].reserved;
        if ((waits[i].revents & kWaitRead) != 0)
            descriptors[index].revents |= POLLIN;
        if ((waits[i].revents & kWaitWrite) != 0)
            descriptors[index].revents |= POLLOUT;
        if (descriptors[index].revents != 0) ++ready;
    }
    return ready;
}
