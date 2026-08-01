#pragma once

#include <stdint.h>

#include "arch/x86_64/isr.hpp"

namespace error_screen {
[[noreturn]] void display(const char* primary,
                         const char* secondary,
                         const InterruptFrame* regs);
}

extern "C" [[noreturn]] void kernel_assertion_failed(const char* expression,
                                                     const char* message,
                                                     const char* file,
                                                     uint32_t line,
                                                     const char* function);

#define KERNEL_ASSERT(condition)                                             \
    do {                                                                     \
        if (__builtin_expect(!(condition), 0)) {                             \
            kernel_assertion_failed(#condition,                              \
                                    nullptr,                                 \
                                    __FILE__,                                \
                                    static_cast<uint32_t>(__LINE__),         \
                                    __func__);                               \
        }                                                                    \
    } while (false)

#define KERNEL_ASSERT_MSG(condition, message)                                \
    do {                                                                     \
        if (__builtin_expect(!(condition), 0)) {                             \
            kernel_assertion_failed(#condition,                              \
                                    (message),                               \
                                    __FILE__,                                \
                                    static_cast<uint32_t>(__LINE__),         \
                                    __func__);                               \
        }                                                                    \
    } while (false)
