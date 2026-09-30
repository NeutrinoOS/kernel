#pragma once

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unnamed semaphores are local to an address space.  The fields are public
 * only so applications can allocate sem_t; treat them as opaque.
 */
typedef struct {
    uint32_t __value;
    uint32_t __waiters;
    uint32_t __state;
} sem_t;

#define SEM_FAILED ((sem_t*)0)
#define SEM_VALUE_MAX 2147483647

int sem_init(sem_t* sem, int pshared, unsigned int value);
int sem_destroy(sem_t* sem);
int sem_wait(sem_t* sem);
int sem_trywait(sem_t* sem);
int sem_timedwait(sem_t* sem, const struct timespec* absolute_timeout);
int sem_post(sem_t* sem);
int sem_getvalue(sem_t* sem, int* value);

/* Named semaphores are not implemented yet. */
sem_t* sem_open(const char* name, int oflag, ...);
int sem_close(sem_t* sem);
int sem_unlink(const char* name);

#ifdef __cplusplus
}
#endif
