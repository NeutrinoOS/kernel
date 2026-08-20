#include "neutrino_syscall.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/reent.h>
#include <time.h>

enum {
    kMaxThreads = 256,
    kMaxKeys = 64,
    kDefaultStackSize = 1024 * 1024,
};

struct thread_context {
    void* (*start)(void*);
    void* argument;
    void* result;
    struct _reent reent;
    void* key_values[kMaxKeys];
    uint32_t tid;
    uint32_t ready;
    uint32_t detached;
    uint32_t completed;
};

static struct thread_context* g_threads[kMaxThreads];
static uint32_t g_thread_table_lock;
static void* g_main_key_values[kMaxKeys];
static void (*g_key_destructors[kMaxKeys])(void*);
static uint64_t g_key_bitmap;
static uint32_t g_threads_started;

static long raw_thread_id(void) {
    return neutrino_raw_syscall0(NEUTRINO_THREAD_ID);
}

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

static void table_lock(void) {
    while (__atomic_exchange_n(&g_thread_table_lock, 1, __ATOMIC_ACQUIRE)) {
        (void)raw_futex_wait(&g_thread_table_lock, 1);
    }
}

static void table_unlock(void) {
    __atomic_store_n(&g_thread_table_lock, 0, __ATOMIC_RELEASE);
    raw_futex_wake(&g_thread_table_lock, 1);
}

static struct thread_context* find_context(uint32_t tid) {
    struct thread_context* result = NULL;
    table_lock();
    for (size_t i = 0; i < kMaxThreads; ++i) {
        if (g_threads[i] != NULL && g_threads[i]->tid == tid) {
            result = g_threads[i];
            break;
        }
    }
    table_unlock();
    return result;
}

static int insert_context(struct thread_context* context) {
    int inserted = 0;
    table_lock();
    for (size_t i = 0; i < kMaxThreads; ++i) {
        if (g_threads[i] == NULL) {
            g_threads[i] = context;
            inserted = 1;
            break;
        }
    }
    table_unlock();
    return inserted;
}

static struct thread_context* remove_context(uint32_t tid) {
    struct thread_context* result = NULL;
    table_lock();
    for (size_t i = 0; i < kMaxThreads; ++i) {
        if (g_threads[i] != NULL && g_threads[i]->tid == tid) {
            result = g_threads[i];
            g_threads[i] = NULL;
            break;
        }
    }
    table_unlock();
    return result;
}

static void run_key_destructors(struct thread_context* context) {
    if (context == NULL) return;
    for (size_t pass = 0; pass < 4; ++pass) {
        int called = 0;
        for (size_t key = 0; key < kMaxKeys; ++key) {
            void* value = context->key_values[key];
            void (*destructor)(void*) = g_key_destructors[key];
            if (value != NULL && destructor != NULL) {
                context->key_values[key] = NULL;
                destructor(value);
                called = 1;
            }
        }
        if (!called) break;
    }
}

static void release_detached_context(struct thread_context* context) {
    uint32_t expected = 1;
    if (!__atomic_compare_exchange_n(&context->detached,
                                     &expected,
                                     2,
                                     0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    (void)remove_context(context->tid);
    free(context);
}

__attribute__((noreturn))
static void thread_trampoline(void* opaque) {
    struct thread_context* context = opaque;
    while (__atomic_load_n(&context->ready, __ATOMIC_ACQUIRE) == 0) {
        (void)raw_futex_wait(&context->ready, 0);
    }
    _REENT_INIT_PTR(&context->reent);
    context->result = context->start(context->argument);
    run_key_destructors(context);
    __atomic_store_n(&context->completed, 1, __ATOMIC_RELEASE);
    release_detached_context(context);
    (void)neutrino_raw_syscall1(NEUTRINO_THREAD_EXIT, 0);
    __builtin_unreachable();
}

struct _reent* __getreent(void) {
    struct thread_context* context = find_context((uint32_t)raw_thread_id());
    return context != NULL ? &context->reent : _impure_ptr;
}

int pthread_attr_init(pthread_attr_t* attr) {
    if (attr == NULL) return EINVAL;
    *attr = (pthread_attr_t){0};
    attr->is_initialized = 1;
    attr->stacksize = kDefaultStackSize;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    if (attr == NULL) return EINVAL;
    attr->is_initialized = 0;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t size) {
    if (attr == NULL || size < 16384 || size > INT32_MAX) return EINVAL;
    attr->stacksize = (int)size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* size) {
    if (attr == NULL || size == NULL) return EINVAL;
    *size = (size_t)attr->stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int state) {
    if (attr == NULL ||
        (state != PTHREAD_CREATE_JOINABLE &&
         state != PTHREAD_CREATE_DETACHED)) return EINVAL;
    attr->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* state) {
    if (attr == NULL || state == NULL) return EINVAL;
    *state = attr->detachstate;
    return 0;
}

int pthread_create(pthread_t* thread,
                   const pthread_attr_t* attr,
                   void* (*start)(void*),
                   void* argument) {
    if (thread == NULL || start == NULL) return EINVAL;
    struct thread_context* context = calloc(1, sizeof(*context));
    if (context == NULL) return EAGAIN;
    context->start = start;
    context->argument = argument;
    context->detached =
        attr != NULL && attr->detachstate == PTHREAD_CREATE_DETACHED;
    size_t stack_size = attr != NULL && attr->stacksize > 0
                            ? (size_t)attr->stacksize
                            : kDefaultStackSize;
    long tid = neutrino_raw_syscall3(
        NEUTRINO_THREAD_CREATE,
        (long)(uintptr_t)thread_trampoline,
        (long)(uintptr_t)context,
        (long)stack_size);
    if (tid <= 0 || tid > UINT32_MAX) {
        free(context);
        return EAGAIN;
    }
    context->tid = (uint32_t)tid;
    if (!insert_context(context)) {
        (void)neutrino_raw_syscall1(NEUTRINO_THREAD_DETACH, tid);
        context->detached = 1;
        __atomic_store_n(&context->ready, 1, __ATOMIC_RELEASE);
        raw_futex_wake(&context->ready, 1);
        return EAGAIN;
    }
    if (context->detached) {
        (void)neutrino_raw_syscall1(NEUTRINO_THREAD_DETACH, tid);
    }
    *thread = (pthread_t)tid;
    __atomic_store_n(&g_threads_started, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&context->ready, 1, __ATOMIC_RELEASE);
    raw_futex_wake(&context->ready, 1);
    return 0;
}

int neutrino_userspace_is_multithreaded(void) {
    return __atomic_load_n(&g_threads_started, __ATOMIC_ACQUIRE) != 0;
}

int pthread_join(pthread_t thread, void** result) {
    struct thread_context* context = find_context((uint32_t)thread);
    if (context == NULL ||
        __atomic_load_n(&context->detached, __ATOMIC_ACQUIRE) != 0)
        return EINVAL;
    if (neutrino_raw_syscall1(NEUTRINO_THREAD_JOIN, thread) < 0) {
        return ESRCH;
    }
    context = remove_context((uint32_t)thread);
    if (context == NULL) return ESRCH;
    if (result != NULL) *result = context->result;
    free(context);
    return 0;
}

int pthread_detach(pthread_t thread) {
    struct thread_context* context = find_context((uint32_t)thread);
    if (context == NULL ||
        __atomic_load_n(&context->detached, __ATOMIC_ACQUIRE) != 0)
        return EINVAL;
    if (neutrino_raw_syscall1(NEUTRINO_THREAD_DETACH, thread) < 0) {
        return ESRCH;
    }
    __atomic_store_n(&context->detached, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&context->completed, __ATOMIC_ACQUIRE) != 0)
        release_detached_context(context);
    return 0;
}

pthread_t pthread_self(void) { return (pthread_t)raw_thread_id(); }
int pthread_equal(pthread_t left, pthread_t right) { return left == right; }

void pthread_exit(void* result) {
    struct thread_context* context = find_context((uint32_t)raw_thread_id());
    if (context != NULL) {
        context->result = result;
        run_key_destructors(context);
        __atomic_store_n(&context->completed, 1, __ATOMIC_RELEASE);
        release_detached_context(context);
    }
    (void)neutrino_raw_syscall1(NEUTRINO_THREAD_EXIT, 0);
    __builtin_unreachable();
}

static uint32_t* mutex_word(pthread_mutex_t* mutex) {
    uint32_t* word = (uint32_t*)mutex;
    uint32_t expected = UINT32_MAX;
    (void)__atomic_compare_exchange_n(word, &expected, 0, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return word;
}

int pthread_mutex_init(pthread_mutex_t* mutex,
                       const pthread_mutexattr_t* attr) {
    (void)attr;
    if (mutex == NULL) return EINVAL;
    __atomic_store_n((uint32_t*)mutex, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (mutex == NULL || __atomic_load_n(mutex_word(mutex), __ATOMIC_ACQUIRE))
        return EBUSY;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (mutex == NULL) return EINVAL;
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(mutex_word(mutex), &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
               ? 0 : EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (mutex == NULL) return EINVAL;
    uint32_t* word = mutex_word(mutex);
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    while (__atomic_exchange_n(word, 2, __ATOMIC_ACQUIRE) != 0) {
        (void)raw_futex_wait(word, 2);
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (mutex == NULL) return EINVAL;
    uint32_t* word = mutex_word(mutex);
    uint32_t old = __atomic_exchange_n(word, 0, __ATOMIC_RELEASE);
    if (old == 0) return EPERM;
    if (old == 2) raw_futex_wake(word, 1);
    return 0;
}

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    (void)attr;
    if (cond == NULL) return EINVAL;
    __atomic_store_n((uint32_t*)cond, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    return cond == NULL ? EINVAL : 0;
}

static uint32_t* cond_word(pthread_cond_t* cond) {
    uint32_t* word = (uint32_t*)cond;
    uint32_t expected = UINT32_MAX;
    (void)__atomic_compare_exchange_n(word, &expected, 0, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return word;
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (cond == NULL) return EINVAL;
    uint32_t* word = cond_word(cond);
    (void)__atomic_add_fetch(word, 1, __ATOMIC_RELEASE);
    raw_futex_wake(word, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (cond == NULL) return EINVAL;
    uint32_t* word = cond_word(cond);
    (void)__atomic_add_fetch(word, 1, __ATOMIC_RELEASE);
    raw_futex_wake(word, kMaxThreads);
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (cond == NULL || mutex == NULL) return EINVAL;
    uint32_t* word = cond_word(cond);
    uint32_t sequence = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    int error = pthread_mutex_unlock(mutex);
    if (error != 0) return error;
    (void)raw_futex_wait(word, sequence);
    return pthread_mutex_lock(mutex);
}

static int remaining_ns(const struct timespec* absolute,
                        uint64_t* remaining) {
    struct timespec now;
    if (absolute == NULL || remaining == NULL || absolute->tv_sec < 0 ||
        absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L)
        return EINVAL;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return errno;
    if (absolute->tv_sec < now.tv_sec ||
        (absolute->tv_sec == now.tv_sec && absolute->tv_nsec <= now.tv_nsec))
        return ETIMEDOUT;
    uint64_t seconds = (uint64_t)(absolute->tv_sec - now.tv_sec);
    int64_t nanos = absolute->tv_nsec - now.tv_nsec;
    if (nanos < 0) {
        --seconds;
        nanos += 1000000000L;
    }
    if (seconds > (UINT64_MAX - (uint64_t)nanos) / 1000000000ull)
        return EINVAL;
    *remaining = seconds * 1000000000ull + (uint64_t)nanos;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t* cond,
                           pthread_mutex_t* mutex,
                           const struct timespec* absolute) {
    if (cond == NULL || mutex == NULL) return EINVAL;
    uint64_t timeout = 0;
    int error = remaining_ns(absolute, &timeout);
    if (error != 0) return error;
    uint32_t* word = cond_word(cond);
    uint32_t sequence = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    error = pthread_mutex_unlock(mutex);
    if (error != 0) return error;
    long wait_result = raw_futex_wait_timed(word, sequence, timeout);
    error = pthread_mutex_lock(mutex);
    if (error != 0) return error;
    return wait_result == -3 ? ETIMEDOUT : 0;
}

int pthread_once(pthread_once_t* once, void (*routine)(void)) {
    if (once == NULL || routine == NULL) return EINVAL;
    int state = __atomic_load_n(&once->init_executed, __ATOMIC_ACQUIRE);
    if (state == 2) return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(&once->init_executed, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        routine();
        __atomic_store_n(&once->init_executed, 2, __ATOMIC_RELEASE);
        raw_futex_wake((uint32_t*)&once->init_executed, kMaxThreads);
        return 0;
    }
    while (__atomic_load_n(&once->init_executed, __ATOMIC_ACQUIRE) != 2)
        (void)raw_futex_wait((uint32_t*)&once->init_executed, 1);
    return 0;
}

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    if (key == NULL) return EINVAL;
    table_lock();
    for (uint32_t i = 0; i < kMaxKeys; ++i) {
        uint64_t bit = 1ull << i;
        if ((g_key_bitmap & bit) == 0) {
            g_key_bitmap |= bit;
            g_key_destructors[i] = destructor;
            *key = i;
            table_unlock();
            return 0;
        }
    }
    table_unlock();
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= kMaxKeys) return EINVAL;
    table_lock();
    g_key_bitmap &= ~(1ull << key);
    g_key_destructors[key] = NULL;
    table_unlock();
    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    if (key >= kMaxKeys || (g_key_bitmap & (1ull << key)) == 0) return NULL;
    struct thread_context* context = find_context((uint32_t)raw_thread_id());
    return context != NULL ? context->key_values[key] : g_main_key_values[key];
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= kMaxKeys || (g_key_bitmap & (1ull << key)) == 0) return EINVAL;
    struct thread_context* context = find_context((uint32_t)raw_thread_id());
    if (context != NULL) context->key_values[key] = (void*)value;
    else g_main_key_values[key] = (void*)value;
    return 0;
}

struct __lock {
    pthread_mutex_t mutex;
    uint32_t owner;
    uint32_t recursion;
    int recursive;
    uint32_t dynamic_in_use;
};

struct __lock __lock___sfp_recursive_mutex = {.recursive = 1};
struct __lock __lock___atexit_recursive_mutex = {.recursive = 1};
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex = {.recursive = 1};
struct __lock __lock___env_recursive_mutex = {.recursive = 1};
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct __lock g_dynamic_locks[256];

void __retarget_lock_init(_LOCK_T* lock) {
    if (lock == NULL) return;
    for (size_t index = 0; index < 256; ++index) {
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(
                &g_dynamic_locks[index].dynamic_in_use,
                &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        g_dynamic_locks[index].mutex = 0;
        g_dynamic_locks[index].owner = 0;
        g_dynamic_locks[index].recursion = 0;
        g_dynamic_locks[index].recursive = 0;
        *lock = &g_dynamic_locks[index];
        return;
    }
    abort();
}

void __retarget_lock_init_recursive(_LOCK_T* lock) {
    __retarget_lock_init(lock);
    (*lock)->recursive = 1;
}

void __retarget_lock_close(_LOCK_T lock) {
    uintptr_t address = (uintptr_t)lock;
    if (address >= (uintptr_t)&g_dynamic_locks[0] &&
        address < (uintptr_t)&g_dynamic_locks[256]) {
        __atomic_store_n(&lock->dynamic_in_use, 0, __ATOMIC_RELEASE);
    }
}
void __retarget_lock_close_recursive(_LOCK_T lock) {
    __retarget_lock_close(lock);
}

void __retarget_lock_acquire(_LOCK_T lock) {
    if (lock == NULL) return;
    uint32_t self = (uint32_t)raw_thread_id();
    if (lock->recursive && lock->owner == self) {
        ++lock->recursion;
        return;
    }
    (void)pthread_mutex_lock(&lock->mutex);
    lock->owner = self;
    lock->recursion = 1;
}

void __retarget_lock_acquire_recursive(_LOCK_T lock) {
    __retarget_lock_acquire(lock);
}

int __retarget_lock_try_acquire(_LOCK_T lock) {
    if (lock == NULL) return 1;
    uint32_t self = (uint32_t)raw_thread_id();
    if (lock->recursive && lock->owner == self) {
        ++lock->recursion;
        return 1;
    }
    if (pthread_mutex_trylock(&lock->mutex) != 0) return 0;
    lock->owner = self;
    lock->recursion = 1;
    return 1;
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock) {
    return __retarget_lock_try_acquire(lock);
}

void __retarget_lock_release(_LOCK_T lock) {
    if (lock == NULL || lock->owner != (uint32_t)raw_thread_id()) return;
    if (lock->recursive && lock->recursion > 1) {
        --lock->recursion;
        return;
    }
    lock->owner = 0;
    lock->recursion = 0;
    (void)pthread_mutex_unlock(&lock->mutex);
}

void __retarget_lock_release_recursive(_LOCK_T lock) {
    __retarget_lock_release(lock);
}
