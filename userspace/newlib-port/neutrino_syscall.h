#pragma once

#include <stddef.h>
#include <stdint.h>

enum neutrino_system_call {
    NEUTRINO_EXIT = 2,
    NEUTRINO_SLEEP = 4,
    NEUTRINO_DESCRIPTOR_OPEN = 5,
    NEUTRINO_DESCRIPTOR_READ = 6,
    NEUTRINO_DESCRIPTOR_WRITE = 7,
    NEUTRINO_DESCRIPTOR_CLOSE = 8,
    NEUTRINO_FILE_OPEN = 15,
    NEUTRINO_FILE_CLOSE = 16,
    NEUTRINO_FILE_READ = 17,
    NEUTRINO_FILE_WRITE = 18,
    NEUTRINO_FILE_CREATE = 19,
    NEUTRINO_FILE_REMOVE = 22,
    NEUTRINO_DIRECTORY_OPEN = 23,
    NEUTRINO_DIRECTORY_READ = 24,
    NEUTRINO_DIRECTORY_CLOSE = 25,
    NEUTRINO_DIRECTORY_CREATE = 28,
    NEUTRINO_DIRECTORY_REMOVE = 29,
    NEUTRINO_PROCESS_SET_CWD = 32,
    NEUTRINO_PROCESS_GET_CWD = 33,
    NEUTRINO_MAP_ANONYMOUS = 34,
    NEUTRINO_TIME_GET = 38,
    NEUTRINO_FILE_SYNC = 50,
    NEUTRINO_SYNC = 51,
    NEUTRINO_RANDOM_GET = 56,
    NEUTRINO_PROCESS_ID = 66,
    NEUTRINO_FILE_OPEN_FLAGS = 83,
    NEUTRINO_FILE_SEEK = 84,
    NEUTRINO_FILE_STAT = 85,
    NEUTRINO_PATH_STAT = 86,
    NEUTRINO_FILE_READ_AT = 87,
    NEUTRINO_CLOCK_GET = 88,
    NEUTRINO_THREAD_CREATE = 60,
    NEUTRINO_THREAD_EXIT = 61,
    NEUTRINO_THREAD_JOIN = 62,
    NEUTRINO_FUTEX_WAIT = 63,
    NEUTRINO_FUTEX_WAKE = 64,
    NEUTRINO_THREAD_ID = 65,
    NEUTRINO_THREAD_DETACH = 89,
    NEUTRINO_FUTEX_WAIT_TIMED = 90,
    NEUTRINO_MAP_FILE_PRIVATE = 59,
    NEUTRINO_UNMAP = 36,
    NEUTRINO_PROTECT_MEMORY = 91,
    NEUTRINO_SYSTEM_INFO = 92,
};

enum {
    NEUTRINO_MAP_WRITE = 1u << 0,
    NEUTRINO_MAP_EXECUTE = 1u << 1,
    NEUTRINO_DESCRIPTOR_CONSOLE = 0x001u,
    NEUTRINO_STDIN = 0x00010000u,
    NEUTRINO_STDOUT = 0x00010001u,
    NEUTRINO_STDERR = 0x00010002u,
    NEUTRINO_FILE_OPEN_READ = 1u << 0,
    NEUTRINO_FILE_OPEN_WRITE = 1u << 1,
    NEUTRINO_FILE_OPEN_CREATE = 1u << 2,
    NEUTRINO_FILE_OPEN_EXCLUSIVE = 1u << 3,
    NEUTRINO_FILE_OPEN_APPEND = 1u << 4,
    NEUTRINO_FILE_METADATA_DIRECTORY = 1u << 0,
};

struct neutrino_descriptor_wait {
    uint32_t handle;
    uint32_t events;
    uint32_t revents;
    uint32_t reserved;
};

struct neutrino_file_metadata {
    uint64_t size;
    uint64_t modified_nanoseconds;
    uint32_t flags;
    uint32_t reserved;
};

struct neutrino_wall_time {
    uint64_t unix_seconds;
    uint32_t nanoseconds;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
    uint8_t reserved[3];
};

enum {
    NEUTRINO_DIRECTORY_ENTRY_DIRECTORY = 1u << 0,
};

struct neutrino_directory_entry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};

_Static_assert(sizeof(struct neutrino_wall_time) == 24,
               "Neutrino wall-time ABI mismatch");
_Static_assert(sizeof(struct neutrino_directory_entry) == 80,
               "Neutrino directory-entry ABI mismatch");
_Static_assert(sizeof(struct neutrino_file_metadata) == 24,
               "Neutrino file-metadata ABI mismatch");

static inline long neutrino_raw_syscall4(long number,
                                         long argument1,
                                         long argument2,
                                         long argument3,
                                         long argument4) {
    register long r10 __asm__("r10") = argument4;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number),
                       "D"(argument1),
                       "S"(argument2),
                       "d"(argument3),
                       "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long neutrino_raw_syscall3(long number,
                                         long argument1,
                                         long argument2,
                                         long argument3) {
    return neutrino_raw_syscall4(
        number, argument1, argument2, argument3, 0);
}

static inline long neutrino_raw_syscall2(long number,
                                         long argument1,
                                         long argument2) {
    return neutrino_raw_syscall4(number, argument1, argument2, 0, 0);
}

static inline long neutrino_raw_syscall1(long number, long argument1) {
    return neutrino_raw_syscall4(number, argument1, 0, 0, 0);
}

static inline long neutrino_raw_syscall0(long number) {
    return neutrino_raw_syscall4(number, 0, 0, 0, 0);
}
