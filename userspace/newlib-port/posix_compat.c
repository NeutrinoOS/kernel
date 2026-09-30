#include "neutrino_syscall.h"
#include "socket_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

enum {
    kFileDescriptorOffset = 3,
    kWaitRead = 1u << 0,
    kWaitWrite = 1u << 1,
};

enum { kNeutrinoPageSize = 4096 };

#ifndef SIG_SETMASK
#define SIG_SETMASK 0
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#endif

/* Neutrino does not yet deliver asynchronous signals.  Keep the POSIX signal
 * bookkeeping here so programs can install dispositions and maintain masks
 * without pretending that delivery is supported. */
static struct sigaction g_signal_actions[NSIG];
static sigset_t g_signal_mask;
static mode_t g_creation_mask = 022;

static char g_root_name[] = "root";
static char g_root_password[] = "x";
static char g_root_gecos[] = "root";
static char g_root_home[] = "/";
static char g_root_shell[] = "/binary/sh";
static struct passwd g_root_passwd = {
    .pw_name = g_root_name,
    .pw_passwd = g_root_password,
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_comment = g_root_gecos,
    .pw_gecos = g_root_gecos,
    .pw_dir = g_root_home,
    .pw_shell = g_root_shell,
};

static int valid_signal_number(int signal_number) {
    return signal_number > 0 && signal_number < NSIG &&
           signal_number < (int)(sizeof(sigset_t) * CHAR_BIT);
}

_Static_assert(sizeof(struct utsname) == 325,
               "Neutrino system-info ABI mismatch");

int uname(struct utsname* information) {
    if (information == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (neutrino_raw_syscall2(NEUTRINO_SYSTEM_INFO,
                              (long)(uintptr_t)information,
                              (long)sizeof(*information)) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void* mmap(void* address,
           size_t length,
           int protection,
           int flags,
           int fd,
           off_t offset) {
    if (length == 0 || offset < 0 ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 ||
        (flags & MAP_PRIVATE) == 0 ||
        ((flags & MAP_FIXED) != 0 && address == NULL)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    unsigned map_flags = 0;
    if ((protection & PROT_WRITE) != 0) map_flags |= NEUTRINO_MAP_WRITE;
    if ((protection & PROT_EXEC) != 0) map_flags |= NEUTRINO_MAP_EXECUTE;
    long result;
    if ((flags & MAP_ANONYMOUS) != 0) {
        if ((flags & MAP_FIXED) != 0) {
            result = neutrino_raw_syscall3(
                NEUTRINO_MAP_AT,
                (long)(uintptr_t)address,
                (long)length,
                map_flags);
        } else {
            /* A non-fixed address is only a hint; let the VM choose safely. */
            result = neutrino_raw_syscall2(
                NEUTRINO_MAP_ANONYMOUS, (long)length, map_flags);
        }
    } else {
        if (address != NULL) {
            errno = ENOTSUP;
            return MAP_FAILED;
        }
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
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
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

int neutrino_enable_write_execute(void) {
    if (neutrino_raw_syscall0(NEUTRINO_MEMORY_WRITE_EXECUTE_ENABLE) < 0) {
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

int gethostname(char* name, size_t length) {
    if (name == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    struct utsname information;
    if (uname(&information) != 0) return -1;
    size_t hostname_length = 0;
    while (hostname_length < sizeof(information.nodename) &&
           information.nodename[hostname_length] != '\0') {
        ++hostname_length;
    }
    size_t copy_length = hostname_length < length - 1
        ? hostname_length : length - 1;
    memcpy(name, information.nodename, copy_length);
    name[copy_length] = '\0';
    return 0;
}

int getpagesize(void) { return kNeutrinoPageSize; }

long sysconf(int name) {
    struct neutrino_process_limits limits;
    if (neutrino_raw_syscall2(NEUTRINO_PROCESS_GET_LIMITS,
                              0,
                              (long)(uintptr_t)&limits) < 0) {
        errno = EIO;
        return -1;
    }

    switch (name) {
        case _SC_ARG_MAX:
            return 131072;
        case _SC_CLK_TCK:
            return 100;
        case _SC_OPEN_MAX:
            return (long)limits.max_descriptors;
        case _SC_PAGESIZE:
            return kNeutrinoPageSize;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:
            /* The process API currently exposes no CPU-affinity query. */
            return 1;
        case _SC_PHYS_PAGES:
        case _SC_AVPHYS_PAGES:
            /* The VM limit is the memory capacity available to this process.
             * Reporting that limit prevents runtimes from sizing themselves
             * against host memory they cannot actually map. */
            if (limits.max_virtual_bytes / kNeutrinoPageSize > LONG_MAX)
                return LONG_MAX;
            return (long)(limits.max_virtual_bytes / kNeutrinoPageSize);
        case _SC_THREAD_THREADS_MAX:
            return (long)limits.max_threads;
        case _SC_GETGR_R_SIZE_MAX:
        case _SC_GETPW_R_SIZE_MAX:
            return 1024;
        case _SC_IOV_MAX:
            return 16;
        default:
            errno = EINVAL;
            return -1;
    }
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

int getgroups(int count, gid_t groups[]) {
    if (count < 0) {
        errno = EINVAL;
        return -1;
    }
    if (count == 0) return 1;
    if (groups == NULL) {
        errno = EFAULT;
        return -1;
    }
    groups[0] = 0;
    return 1;
}

struct passwd* getpwuid(uid_t uid) {
    return uid == 0 ? &g_root_passwd : NULL;
}

static int copy_passwd_string(char** cursor,
                              size_t* remaining,
                              const char* value,
                              char** output) {
    size_t length = strlen(value) + 1;
    if (length > *remaining) return ERANGE;
    memcpy(*cursor, value, length);
    *output = *cursor;
    *cursor += length;
    *remaining -= length;
    return 0;
}

int getpwuid_r(uid_t uid,
               struct passwd* password,
               char* buffer,
               size_t buffer_size,
               struct passwd** result) {
    if (password == NULL || buffer == NULL || result == NULL) return EINVAL;
    *result = NULL;
    if (uid != 0) return 0;

    struct passwd copy = {.pw_uid = 0, .pw_gid = 0};
    char* cursor = buffer;
    size_t remaining = buffer_size;
    int error;
    if ((error = copy_passwd_string(&cursor, &remaining, g_root_name,
                                    &copy.pw_name)) != 0 ||
        (error = copy_passwd_string(&cursor, &remaining, g_root_password,
                                    &copy.pw_passwd)) != 0 ||
        (error = copy_passwd_string(&cursor, &remaining, g_root_gecos,
                                    &copy.pw_gecos)) != 0 ||
        (error = copy_passwd_string(&cursor, &remaining, g_root_home,
                                    &copy.pw_dir)) != 0 ||
        (error = copy_passwd_string(&cursor, &remaining, g_root_shell,
                                    &copy.pw_shell)) != 0) {
        return error;
    }
    copy.pw_comment = copy.pw_gecos;
    *password = copy;
    *result = password;
    return 0;
}

mode_t umask(mode_t mask) {
    return __atomic_exchange_n(&g_creation_mask, mask & 0777, __ATOMIC_SEQ_CST);
}

int chmod(const char* path, mode_t mode) {
    (void)mode;
    if (path == NULL) {
        errno = EFAULT;
    } else {
        /* File permission mutation is not present in the filesystem ABI. */
        errno = ENOTSUP;
    }
    return -1;
}

int utimes(const char* path, const struct timeval times[2]) {
    (void)times;
    if (path == NULL) {
        errno = EFAULT;
    } else {
        /* Timestamp mutation is not present in the filesystem ABI. */
        errno = ENOTSUP;
    }
    return -1;
}

int dup2(int old_fd, int new_fd) {
    if (old_fd < 0 || new_fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (old_fd == new_fd) return new_fd;
    errno = ENOSYS;
    return -1;
}

int pipe(int descriptors[2]) {
    if (descriptors == NULL) {
        errno = EFAULT;
    } else {
        /* POSIX descriptor duplication is required before pipes can be
         * represented safely in Newlib's integer descriptor namespace. */
        errno = ENOSYS;
    }
    return -1;
}

int execvp(const char* file, char* const arguments[]) {
    (void)file;
    (void)arguments;
    errno = ENOSYS;
    return -1;
}

pid_t vfork(void) {
    errno = ENOSYS;
    return (pid_t)-1;
}

int posix_spawn(pid_t* restrict pid,
                const char* restrict path,
                const posix_spawn_file_actions_t* actions,
                const posix_spawnattr_t* restrict attributes,
                char* const arguments[],
                char* const environment[]) {
    (void)pid;
    (void)path;
    (void)actions;
    (void)attributes;
    (void)arguments;
    (void)environment;
    return ENOSYS;
}

int ftruncate(int fd, off_t length) {
    if (fd < 0) {
        errno = EBADF;
    } else if (length < 0) {
        errno = EINVAL;
    } else {
        /* The file ABI currently has no resize operation. */
        errno = ENOTSUP;
    }
    return -1;
}

pid_t fork(void) {
    /* Process cloning is not a kernel capability yet. */
    errno = ENOSYS;
    return (pid_t)-1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    (void)pid;
    (void)status;
    if ((options & ~(WNOHANG | WUNTRACED)) != 0) {
        errno = EINVAL;
    } else {
        errno = ECHILD;
    }
    return (pid_t)-1;
}

int sched_yield(void) {
    /* The kernel scheduler is preemptive; this is a successful yield point. */
    __asm__ volatile("pause" ::: "memory");
    return 0;
}

int sigemptyset(sigset_t* set) {
    if (set == NULL) { errno = EINVAL; return -1; }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t* set) {
    if (set == NULL) { errno = EINVAL; return -1; }
    *set = ~(sigset_t)0;
    return 0;
}

int sigaddset(sigset_t* set, int signal_number) {
    if (set == NULL || !valid_signal_number(signal_number)) {
        errno = EINVAL;
        return -1;
    }
    *set |= (sigset_t)1u << signal_number;
    return 0;
}

int sigdelset(sigset_t* set, int signal_number) {
    if (set == NULL || !valid_signal_number(signal_number)) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~((sigset_t)1u << signal_number);
    return 0;
}

int sigismember(const sigset_t* set, int signal_number) {
    if (set == NULL || !valid_signal_number(signal_number)) {
        errno = EINVAL;
        return -1;
    }
    return (*set & ((sigset_t)1u << signal_number)) != 0;
}

int sigaction(int signal_number,
              const struct sigaction* action,
              struct sigaction* old_action) {
    if (!valid_signal_number(signal_number) || signal_number == SIGKILL ||
        signal_number == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    if (old_action != NULL) *old_action = g_signal_actions[signal_number];
    if (action != NULL) g_signal_actions[signal_number] = *action;
    return 0;
}

int sigprocmask(int operation, const sigset_t* set, sigset_t* old_set) {
    if (old_set != NULL) *old_set = g_signal_mask;
    if (set == NULL) return 0;
    switch (operation) {
        case SIG_BLOCK: g_signal_mask |= *set; break;
        case SIG_UNBLOCK: g_signal_mask &= ~*set; break;
        case SIG_SETMASK: g_signal_mask = *set; break;
        default: errno = EINVAL; return -1;
    }
    return 0;
}

int pthread_sigmask(int operation, const sigset_t* set, sigset_t* old_set) {
    if (sigprocmask(operation, set, old_set) == 0) return 0;
    return errno;
}

int pthread_kill(pthread_t thread, int signal_number) {
    (void)thread;
    if (signal_number != 0 && !valid_signal_number(signal_number)) return EINVAL;
    return ENOSYS;
}

int sigsuspend(const sigset_t* mask) {
    if (mask == NULL) { errno = EINVAL; return -1; }
    /* There is no signal-delivery syscall to wait on yet. */
    errno = ENOSYS;
    return -1;
}

int getrlimit(int resource, struct rlimit* limits) {
    if (limits == NULL) {
        errno = EFAULT;
        return -1;
    }

    struct neutrino_process_limits process_limits;
    if (neutrino_raw_syscall2(
            NEUTRINO_PROCESS_GET_LIMITS,
            0,
            (long)(uintptr_t)&process_limits) < 0) {
        errno = EIO;
        return -1;
    }

    uint64_t value;
    switch (resource) {
        case RLIMIT_AS:
            value = process_limits.max_virtual_bytes;
            break;
        case RLIMIT_NPROC:
            value = process_limits.max_threads;
            break;
        case RLIMIT_NOFILE:
            value = process_limits.max_descriptors;
            break;
        case RLIMIT_STACK:
        case RLIMIT_CORE:
        case RLIMIT_CPU:
        case RLIMIT_FSIZE:
        case RLIMIT_DATA:
        case RLIMIT_RSS:
        case RLIMIT_MEMLOCK:
            value = RLIM_INFINITY;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    limits->rlim_cur = value;
    limits->rlim_max = value;
    return 0;
}

int setrlimit(int resource, const struct rlimit* limits) {
    if (limits == NULL) {
        errno = EFAULT;
        return -1;
    }
    /* Neutrino currently has one enforced value per resource, rather than
     * distinct soft and hard limits. */
    if (limits->rlim_cur != limits->rlim_max) {
        errno = EINVAL;
        return -1;
    }

    struct neutrino_process_limits process_limits;
    if (neutrino_raw_syscall2(
            NEUTRINO_PROCESS_GET_LIMITS,
            0,
            (long)(uintptr_t)&process_limits) < 0) {
        errno = EIO;
        return -1;
    }

    switch (resource) {
        case RLIMIT_AS:
            process_limits.max_virtual_bytes = limits->rlim_cur;
            break;
        case RLIMIT_NPROC:
            if (limits->rlim_cur > UINT32_MAX) {
                errno = EINVAL;
                return -1;
            }
            process_limits.max_threads = (uint32_t)limits->rlim_cur;
            break;
        case RLIMIT_NOFILE:
            if (limits->rlim_cur > UINT32_MAX) {
                errno = EINVAL;
                return -1;
            }
            process_limits.max_descriptors = (uint32_t)limits->rlim_cur;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    if (neutrino_raw_syscall2(
            NEUTRINO_PROCESS_SET_LIMITS,
            0,
            (long)(uintptr_t)&process_limits) < 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int fcntl(int fd, int command, ...) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    int value = 0;
    if (command == F_SETFL || command == F_SETFD) {
        va_list arguments;
        va_start(arguments, command);
        value = va_arg(arguments, int);
        va_end(arguments);
    }
    if (neutrino_socket_is_fd(fd))
        return neutrino_socket_fcntl(fd, command, value);
    switch (command) {
        case F_GETFD:
            return 0;
        case F_SETFD:
            return 0;
        case F_GETFL:
            return fd < kFileDescriptorOffset ? O_RDWR : O_RDWR;
        case F_SETFL: {
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
        uint32_t socket_handle = 0;
        int socket_state = 0;
        if (neutrino_socket_poll_handle(descriptors[i].fd,
                                        &socket_handle,
                                        &socket_state)) {
            if (socket_state == 0) {
                descriptors[i].revents = POLLERR;
                ++ready;
                continue;
            }
            if (socket_state == 1 && (descriptors[i].events & POLLOUT) != 0) {
                descriptors[i].revents |= POLLOUT;
                ++ready;
                continue;
            }
            if (wait_count >= 64) { errno = EINVAL; return -1; }
            waits[wait_count].handle = socket_handle;
            waits[wait_count].events = kWaitRead;
            waits[wait_count].reserved = (uint32_t)i;
            ++wait_count;
            continue;
        }
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
