#include "neutrino_syscall.h"
#include "socket_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum {
    kFileDescriptorOffset = 3,
    kHeapCapacity = 64 * 1024 * 1024,
};

static unsigned char* g_heap_base;
static size_t g_heap_size;
static long g_fallback_console = -2;

static uint32_t standard_descriptor(int fd) {
    if (fd == STDIN_FILENO) {
        return NEUTRINO_STDIN;
    }
    if (fd == STDOUT_FILENO) {
        return NEUTRINO_STDOUT;
    }
    return NEUTRINO_STDERR;
}

static int file_handle_from_fd(int fd, uint32_t* handle) {
    if (fd < kFileDescriptorOffset || handle == NULL) {
        errno = EBADF;
        return -1;
    }
    *handle = (uint32_t)(fd - kFileDescriptorOffset);
    return 0;
}

__attribute__((noreturn))
void _exit(int status) {
    (void)neutrino_raw_syscall1(NEUTRINO_EXIT, (uint16_t)status);
    for (;;) {
        __asm__ volatile("ud2");
    }
}

ssize_t _read(int fd, void* buffer, size_t length) {
    long result;
    if (neutrino_socket_is_fd(fd))
        return neutrino_socket_read(fd, buffer, length);
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        result = neutrino_raw_syscall4(
            NEUTRINO_DESCRIPTOR_READ,
            standard_descriptor(fd),
            (long)(uintptr_t)buffer,
            (long)length,
            0);
    } else {
        uint32_t handle;
        if (file_handle_from_fd(fd, &handle) < 0) {
            return -1;
        }
        result = neutrino_raw_syscall3(
            NEUTRINO_FILE_READ,
            handle,
            (long)(uintptr_t)buffer,
            (long)length);
    }

    if (result < 0) {
        errno = result == -2 ? EAGAIN : EIO;
        return -1;
    }
    return (ssize_t)result;
}

ssize_t _write(int fd, const void* buffer, size_t length) {
    long result;
    if (neutrino_socket_is_fd(fd))
        return neutrino_socket_write(fd, buffer, length);
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        result = neutrino_raw_syscall4(
            NEUTRINO_DESCRIPTOR_WRITE,
            standard_descriptor(fd),
            (long)(uintptr_t)buffer,
            (long)length,
            0);
        if (result < 0 &&
            (fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
            if (g_fallback_console == -2) {
                g_fallback_console = neutrino_raw_syscall4(
                    NEUTRINO_DESCRIPTOR_OPEN,
                    NEUTRINO_DESCRIPTOR_CONSOLE,
                    0,
                    0,
                    0);
            }
            if (g_fallback_console >= 0) {
                result = neutrino_raw_syscall4(
                    NEUTRINO_DESCRIPTOR_WRITE,
                    g_fallback_console,
                    (long)(uintptr_t)buffer,
                    (long)length,
                    0);
            }
        }
    } else {
        uint32_t handle;
        if (file_handle_from_fd(fd, &handle) < 0) {
            return -1;
        }
        result = neutrino_raw_syscall3(
            NEUTRINO_FILE_WRITE,
            handle,
            (long)(uintptr_t)buffer,
            (long)length);
    }

    if (result < 0) {
        errno = result == -2 ? EAGAIN : EIO;
        return -1;
    }
    return (ssize_t)result;
}

int _open(const char* path, int flags, ...) {
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if ((flags & O_TRUNC) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    unsigned open_flags = 0;
    switch (flags & O_ACCMODE) {
        case O_RDONLY:
            open_flags = NEUTRINO_FILE_OPEN_READ;
            break;
        case O_WRONLY:
            open_flags = NEUTRINO_FILE_OPEN_WRITE;
            break;
        case O_RDWR:
            open_flags = NEUTRINO_FILE_OPEN_READ |
                         NEUTRINO_FILE_OPEN_WRITE;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    if ((flags & O_CREAT) != 0) open_flags |= NEUTRINO_FILE_OPEN_CREATE;
    if ((flags & O_EXCL) != 0) open_flags |= NEUTRINO_FILE_OPEN_EXCLUSIVE;
    if ((flags & O_APPEND) != 0) open_flags |= NEUTRINO_FILE_OPEN_APPEND;

    long handle = neutrino_raw_syscall2(
        NEUTRINO_FILE_OPEN_FLAGS,
        (long)(uintptr_t)path,
        open_flags);

    if (handle < 0 || handle > INT32_MAX - kFileDescriptorOffset) {
        errno = ENOENT;
        return -1;
    }
    return (int)handle + kFileDescriptorOffset;
}

int _close(int fd) {
    if (neutrino_socket_is_fd(fd))
        return neutrino_socket_close(fd);
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        return 0;
    }

    uint32_t handle;
    if (file_handle_from_fd(fd, &handle) < 0) {
        return -1;
    }
    if (neutrino_raw_syscall1(NEUTRINO_FILE_CLOSE, handle) < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

int _fsync(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        return 0;
    }

    uint32_t handle;
    if (file_handle_from_fd(fd, &handle) < 0) {
        return -1;
    }
    if (neutrino_raw_syscall1(NEUTRINO_FILE_SYNC, handle) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int _fstat(int fd, struct stat* status) {
    if (status == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (fd < STDIN_FILENO) {
        errno = EBADF;
        return -1;
    }

    *status = (struct stat){0};
    if (neutrino_socket_is_fd(fd)) {
#ifdef S_IFSOCK
        status->st_mode = S_IFSOCK | 0600;
#else
        status->st_mode = 0140000 | 0600;
#endif
        status->st_blksize = 1460;
        return 0;
    }
    if (fd <= STDERR_FILENO) {
        status->st_mode = S_IFCHR | 0600;
        status->st_blksize = 512;
        return 0;
    }
    uint32_t handle;
    struct neutrino_file_metadata metadata;
    if (file_handle_from_fd(fd, &handle) < 0 ||
        neutrino_raw_syscall2(NEUTRINO_FILE_STAT,
                              handle,
                              (long)(uintptr_t)&metadata) < 0) {
        errno = EBADF;
        return -1;
    }
    status->st_mode = S_IFREG | 0600;
    status->st_size = (off_t)metadata.size;
    status->st_blksize = 512;
    status->st_blocks = (blkcnt_t)((metadata.size + 511u) / 512u);
    return 0;
}

int _isatty(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

off_t _lseek(int fd, off_t offset, int whence) {
    uint32_t handle;
    if (file_handle_from_fd(fd, &handle) < 0) {
        return (off_t)-1;
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return (off_t)-1;
    }
    long result = neutrino_raw_syscall3(
        NEUTRINO_FILE_SEEK, handle, (long)offset, whence);
    if (result < 0) {
        errno = EINVAL;
        return (off_t)-1;
    }
    return (off_t)result;
}

int _stat(const char* path, struct stat* status) {
    if (path == NULL || status == NULL) {
        errno = EFAULT;
        return -1;
    }
    struct neutrino_file_metadata metadata;
    if (neutrino_raw_syscall2(NEUTRINO_PATH_STAT,
                              (long)(uintptr_t)path,
                              (long)(uintptr_t)&metadata) < 0) {
        errno = ENOENT;
        return -1;
    }
    *status = (struct stat){0};
    status->st_mode =
        (metadata.flags & NEUTRINO_FILE_METADATA_DIRECTORY) != 0
            ? (S_IFDIR | 0700)
            : (S_IFREG | 0600);
    status->st_size = (off_t)metadata.size;
    status->st_blksize = 512;
    status->st_blocks = (blkcnt_t)((metadata.size + 511u) / 512u);
    return 0;
}

ssize_t pread(int fd, void* buffer, size_t length, off_t offset) {
    uint32_t handle;
    if (offset < 0 || file_handle_from_fd(fd, &handle) < 0) {
        if (offset < 0) errno = EINVAL;
        return -1;
    }
    long result = neutrino_raw_syscall4(
        NEUTRINO_FILE_READ_AT,
        handle,
        (long)(uintptr_t)buffer,
        (long)length,
        (long)offset);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)result;
}

int clock_gettime(clockid_t clock_id, struct timespec* value) {
    if (value == NULL) {
        errno = EFAULT;
        return -1;
    }
    long clock_kind;
    if (clock_id == CLOCK_MONOTONIC
#ifdef CLOCK_MONOTONIC_RAW
        || clock_id == CLOCK_MONOTONIC_RAW
#endif
    ) {
        clock_kind = 0;
    } else if (clock_id == CLOCK_REALTIME) {
        clock_kind = 1;
    } else {
        errno = EINVAL;
        return -1;
    }
    long result = neutrino_raw_syscall1(NEUTRINO_CLOCK_GET, clock_kind);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    uint64_t nanoseconds = (uint64_t)result;
    value->tv_sec = (time_t)(nanoseconds / 1000000000ull);
    value->tv_nsec = (long)(nanoseconds % 1000000000ull);
    return 0;
}

void* _sbrk(ptrdiff_t increment) {
    if (g_heap_base == NULL) {
        long address = neutrino_raw_syscall2(
            NEUTRINO_MAP_ANONYMOUS,
            kHeapCapacity,
            NEUTRINO_MAP_WRITE);
        if (address < 0) {
            errno = ENOMEM;
            return (void*)-1;
        }
        g_heap_base = (unsigned char*)(uintptr_t)address;
    }

    if (increment < 0) {
        if (increment == PTRDIFF_MIN) {
            errno = EINVAL;
            return (void*)-1;
        }
        size_t decrease = (size_t)(-increment);
        if (decrease > g_heap_size) {
            errno = EINVAL;
            return (void*)-1;
        }
        void* previous = g_heap_base + g_heap_size;
        g_heap_size -= decrease;
        return previous;
    }

    size_t increase = (size_t)increment;
    if (increase > kHeapCapacity - g_heap_size) {
        errno = ENOMEM;
        return (void*)-1;
    }
    void* previous = g_heap_base + g_heap_size;
    g_heap_size += increase;
    return previous;
}

int _gettimeofday(struct timeval* time_value, void* timezone_value) {
    (void)timezone_value;
    if (time_value == NULL) {
        errno = EFAULT;
        return -1;
    }

    struct neutrino_wall_time snapshot;
    long result = neutrino_raw_syscall2(
        NEUTRINO_TIME_GET,
        (long)(uintptr_t)&snapshot,
        sizeof(snapshot));
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    time_value->tv_sec = (time_t)snapshot.unix_seconds;
    time_value->tv_usec = (suseconds_t)(snapshot.nanoseconds / 1000u);
    return 0;
}

int _getpid(void) {
    long result = neutrino_raw_syscall0(NEUTRINO_PROCESS_ID);
    if (result < 0 || result > INT32_MAX) {
        errno = ESRCH;
        return -1;
    }
    return (int)result;
}

int _kill(int pid, int signal_number) {
    (void)pid;
    (void)signal_number;
    errno = ENOSYS;
    return -1;
}

int _unlink(const char* path) {
    if (neutrino_raw_syscall1(
            NEUTRINO_FILE_REMOVE, (long)(uintptr_t)path) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _mkdir(const char* path, mode_t mode) {
    (void)mode;
    if (neutrino_raw_syscall1(
            NEUTRINO_DIRECTORY_CREATE, (long)(uintptr_t)path) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int _rmdir(const char* path) {
    if (neutrino_raw_syscall1(
            NEUTRINO_DIRECTORY_REMOVE, (long)(uintptr_t)path) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _chdir(const char* path) {
    if (neutrino_raw_syscall1(
            NEUTRINO_PROCESS_SET_CWD, (long)(uintptr_t)path) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

char* _getcwd(char* buffer, size_t size) {
    long result = neutrino_raw_syscall2(
        NEUTRINO_PROCESS_GET_CWD,
        (long)(uintptr_t)buffer,
        (long)size);
    if (result < 0) {
        errno = ERANGE;
        return NULL;
    }
    return buffer;
}

int getentropy(void* buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    long result = neutrino_raw_syscall2(
        NEUTRINO_RANDOM_GET,
        (long)(uintptr_t)buffer,
        (long)length);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int nanosleep(const struct timespec* duration, struct timespec* remaining) {
    if (duration == NULL || duration->tv_sec < 0 ||
        duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)duration->tv_sec >
        (UINT64_MAX - (uint64_t)duration->tv_nsec) / 1000000000ull) {
        errno = EINVAL;
        return -1;
    }

    uint64_t nanoseconds =
        (uint64_t)duration->tv_sec * 1000000000ull +
        (uint64_t)duration->tv_nsec;
    if (neutrino_raw_syscall1(NEUTRINO_SLEEP, (long)nanoseconds) < 0) {
        errno = EINTR;
        if (remaining != NULL) {
            *remaining = *duration;
        }
        return -1;
    }
    if (remaining != NULL) {
        *remaining = (struct timespec){0};
    }
    return 0;
}

unsigned sleep(unsigned seconds) {
    struct timespec duration = {
        .tv_sec = seconds,
        .tv_nsec = 0,
    };
    return nanosleep(&duration, NULL) == 0 ? 0 : seconds;
}

int usleep(useconds_t microseconds) {
    struct timespec duration = {
        .tv_sec = microseconds / 1000000u,
        .tv_nsec = (long)(microseconds % 1000000u) * 1000L,
    };
    return nanosleep(&duration, NULL);
}

/*
 * Newlib's reentrant wrappers call the unsuffixed POSIX names for generic
 * targets. Keep the underscore entry points above as the platform boundary,
 * and expose these thin forwards for libc itself and user code.
 */
int read(int fd, void* buffer, size_t length) {
    if (length > INT_MAX) {
        length = INT_MAX;
    }
    return (int)_read(fd, buffer, length);
}

int write(int fd, const void* buffer, size_t length) {
    if (length > INT_MAX) {
        length = INT_MAX;
    }
    return (int)_write(fd, buffer, length);
}

int open(const char* path, int flags, ...) {
    return _open(path, flags);
}

int close(int fd) {
    return _close(fd);
}

int fsync(int fd) {
    return _fsync(fd);
}

void sync(void) {
    (void)neutrino_raw_syscall0(NEUTRINO_SYNC);
}

off_t lseek(int fd, off_t offset, int whence) {
    return _lseek(fd, offset, whence);
}

int fstat(int fd, struct stat* status) {
    return _fstat(fd, status);
}

int stat(const char* path, struct stat* status) {
    return _stat(path, status);
}

clock_t times(struct tms* usage) {
    struct timespec now;
    if (usage != NULL) {
        *usage = (struct tms){0};
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return (clock_t)-1;
    }
    return (clock_t)(now.tv_sec * CLOCKS_PER_SEC +
                     now.tv_nsec / (1000000000L / CLOCKS_PER_SEC));
}

int link(const char* existing_path, const char* new_path) {
    (void)existing_path;
    (void)new_path;
    errno = ENOSYS;
    return -1;
}

int isatty(int fd) {
    return _isatty(fd);
}

void* sbrk(ptrdiff_t increment) {
    return _sbrk(increment);
}

int gettimeofday(struct timeval* time_value, void* timezone_value) {
    return _gettimeofday(time_value, timezone_value);
}

int getpid(void) {
    return _getpid();
}

int kill(int pid, int signal_number) {
    return _kill(pid, signal_number);
}

int unlink(const char* path) {
    return _unlink(path);
}

int mkdir(const char* path, mode_t mode) {
    return _mkdir(path, mode);
}

int rmdir(const char* path) {
    return _rmdir(path);
}

int chdir(const char* path) {
    return _chdir(path);
}

char* getcwd(char* buffer, size_t size) {
    return _getcwd(buffer, size);
}

void _init(void) {
}

void _fini(void) {
}
