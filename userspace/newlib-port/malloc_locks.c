/* Newlib calls these around every allocation.  Before pthread_create there
 * cannot be a concurrent allocator user, so avoiding a lock is safe.  Once
 * a thread has been created, retain the recursive locking contract Newlib
 * requires. */
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>

extern int neutrino_userspace_is_multithreaded(void);

static pthread_mutex_t g_malloc_mutex;
static uint32_t g_malloc_owner;
static uint32_t g_malloc_recursion;

static uint32_t current_thread(void) {
    return (uint32_t)pthread_self();
}

void __malloc_lock(struct _reent* reent) {
    (void)reent;
    if (!neutrino_userspace_is_multithreaded()) return;

    uint32_t self = current_thread();
    if (__atomic_load_n(&g_malloc_owner, __ATOMIC_ACQUIRE) == self) {
        ++g_malloc_recursion;
        return;
    }
    (void)pthread_mutex_lock(&g_malloc_mutex);
    __atomic_store_n(&g_malloc_owner, self, __ATOMIC_RELEASE);
    g_malloc_recursion = 1;
}

void __malloc_unlock(struct _reent* reent) {
    (void)reent;
    if (!neutrino_userspace_is_multithreaded()) return;

    uint32_t self = current_thread();
    if (__atomic_load_n(&g_malloc_owner, __ATOMIC_ACQUIRE) != self) return;
    if (g_malloc_recursion > 1) {
        --g_malloc_recursion;
        return;
    }
    g_malloc_recursion = 0;
    __atomic_store_n(&g_malloc_owner, 0, __ATOMIC_RELEASE);
    (void)pthread_mutex_unlock(&g_malloc_mutex);
}
