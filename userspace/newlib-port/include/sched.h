#pragma once

#include_next <sched.h>
#include <stddef.h>
#include <string.h>

#ifndef _NEUTRINO_CPU_SET_T
#define _NEUTRINO_CPU_SET_T
typedef struct {
    unsigned long bits[16];
} cpu_set_t;
#endif

#define CPU_ZERO(set) memset((set), 0, sizeof(*(set)))
#define CPU_SET(cpu, set) \
    ((set)->bits[(size_t)(cpu) / (8u * sizeof(unsigned long))] |= \
     1ul << ((size_t)(cpu) % (8u * sizeof(unsigned long))))
#define CPU_ISSET(cpu, set) \
    (((set)->bits[(size_t)(cpu) / (8u * sizeof(unsigned long))] & \
      (1ul << ((size_t)(cpu) % (8u * sizeof(unsigned long))))) != 0)

int sched_getaffinity(pid_t pid, size_t set_size, cpu_set_t* set);
