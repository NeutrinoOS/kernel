#include "neutrino_syscall.h"

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>

enum {
    kSemaphoreInitialized = 0x4e53454du,
};

static long raw_futex_wait(uint32_t* address, uint32_t expected) {
    return neutrino_raw_syscall2(NEUTRINO_FUTEX_WAIT,
                                 (long)(uintptr_t)address,
                                 expected);
}

static long raw_futex_wait_timed(uint32_t* address,
                                 uint32_t expected,
                                 uint64_t timeout_ns) {
    return neutrino_raw_syscall3(NEUTRINO_FUTEX_WAIT_TIMED,
                                 (long)(uintptr_t)address,
                                 expected,
                                 (long)timeout_ns);
}

static void raw_futex_wake(uint32_t* address, size_t count) {
    (void)neutrino_raw_syscall2(NEUTRINO_FUTEX_WAKE,
                                (long)(uintptr_t)address,
                                (long)count);
}

static int validate(const sem_t* sem) {
    if (sem == NULL ||
        __atomic_load_n(&sem->__state, __ATOMIC_ACQUIRE) !=
            kSemaphoreInitialized) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int remaining_ns(const struct timespec* absolute, uint64_t* remaining) {
    struct timespec now;
    if (absolute == NULL || remaining == NULL || absolute->tv_sec < 0 ||
        absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    if (absolute->tv_sec < now.tv_sec ||
        (absolute->tv_sec == now.tv_sec && absolute->tv_nsec <= now.tv_nsec)) {
        errno = ETIMEDOUT;
        return -1;
    }
    uint64_t seconds = (uint64_t)(absolute->tv_sec - now.tv_sec);
    int64_t nanoseconds = absolute->tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    if (seconds > (UINT64_MAX - (uint64_t)nanoseconds) / 1000000000ull) {
        errno = EINVAL;
        return -1;
    }
    *remaining = seconds * 1000000000ull + (uint64_t)nanoseconds;
    return 0;
}

static int try_acquire(sem_t* sem) {
    uint32_t value = __atomic_load_n(&sem->__value, __ATOMIC_ACQUIRE);
    while (value != 0) {
        if (__atomic_compare_exchange_n(&sem->__value,
                                        &value,
                                        value - 1,
                                        0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return 1;
        }
    }
    return 0;
}

int sem_init(sem_t* sem, int pshared, unsigned int value) {
    if (sem == NULL || value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (pshared != 0) {
        errno = ENOSYS;
        return -1;
    }
    __atomic_store_n(&sem->__value, value, __ATOMIC_RELAXED);
    __atomic_store_n(&sem->__waiters, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sem->__state, kSemaphoreInitialized, __ATOMIC_RELEASE);
    return 0;
}

int sem_destroy(sem_t* sem) {
    if (validate(sem) != 0) return -1;
    if (__atomic_load_n(&sem->__waiters, __ATOMIC_ACQUIRE) != 0) {
        errno = EBUSY;
        return -1;
    }
    __atomic_store_n(&sem->__state, 0, __ATOMIC_RELEASE);
    return 0;
}

int sem_trywait(sem_t* sem) {
    if (validate(sem) != 0) return -1;
    if (try_acquire(sem)) return 0;
    errno = EAGAIN;
    return -1;
}

static int wait_for_token(sem_t* sem,
                          const struct timespec* absolute_timeout) {
    for (;;) {
        if (try_acquire(sem)) return 0;

        __atomic_add_fetch(&sem->__waiters, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&sem->__value, __ATOMIC_ACQUIRE) == 0) {
            long result;
            if (absolute_timeout == NULL) {
                result = raw_futex_wait(&sem->__value, 0);
            } else {
                uint64_t timeout = 0;
                if (remaining_ns(absolute_timeout, &timeout) != 0) {
                    __atomic_sub_fetch(&sem->__waiters, 1, __ATOMIC_ACQ_REL);
                    return -1;
                }
                result = raw_futex_wait_timed(&sem->__value, 0, timeout);
            }
            __atomic_sub_fetch(&sem->__waiters, 1, __ATOMIC_ACQ_REL);
            if (result == -3) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (result == -1) {
                errno = EINVAL;
                return -1;
            }
        } else {
            __atomic_sub_fetch(&sem->__waiters, 1, __ATOMIC_ACQ_REL);
        }
    }
}

int sem_wait(sem_t* sem) {
    if (validate(sem) != 0) return -1;
    return wait_for_token(sem, NULL);
}

int sem_timedwait(sem_t* sem, const struct timespec* absolute_timeout) {
    if (validate(sem) != 0) return -1;
    if (absolute_timeout == NULL || absolute_timeout->tv_sec < 0 ||
        absolute_timeout->tv_nsec < 0 ||
        absolute_timeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    return wait_for_token(sem, absolute_timeout);
}

int sem_post(sem_t* sem) {
    if (validate(sem) != 0) return -1;
    uint32_t value = __atomic_load_n(&sem->__value, __ATOMIC_RELAXED);
    for (;;) {
        if (value >= SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        if (__atomic_compare_exchange_n(&sem->__value,
                                        &value,
                                        value + 1,
                                        0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            raw_futex_wake(&sem->__value, 1);
            return 0;
        }
    }
}

int sem_getvalue(sem_t* sem, int* value) {
    if (validate(sem) != 0) return -1;
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    *value = (int)__atomic_load_n(&sem->__value, __ATOMIC_ACQUIRE);
    return 0;
}

sem_t* sem_open(const char* name, int oflag, ...) {
    (void)name;
    (void)oflag;
    errno = ENOSYS;
    return SEM_FAILED;
}

int sem_close(sem_t* sem) {
    (void)sem;
    errno = ENOSYS;
    return -1;
}

int sem_unlink(const char* name) {
    (void)name;
    errno = ENOSYS;
    return -1;
}
