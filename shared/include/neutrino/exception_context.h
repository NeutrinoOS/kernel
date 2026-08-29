#pragma once

#include <stdint.h>

/*
 * x86-64 general-purpose state at an exception boundary.
 *
 * This is an ABI type: keep existing members and their order stable.  It
 * deliberately excludes FS/GS bases and floating-point/SIMD state because
 * those are not yet captured by the interrupt path.
 */
typedef struct NeuExceptionContext {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi;
    uint64_t rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;

    uint64_t rip;
    uint64_t rflags;

    uint64_t fault_address;
    uint64_t error_code;
    uint64_t exception;

    /* Selectors are retained for kernel-originated exceptions. */
    uint64_t cs;
    uint64_t ss;
} NeuExceptionContext;

#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(NeuExceptionContext) == 184,
              "x86-64 exception context ABI mismatch");
#elif defined(__cplusplus)
typedef char neutrino_exception_context_size_must_be_184[
    sizeof(NeuExceptionContext) == 184 ? 1 : -1];
#else
_Static_assert(sizeof(NeuExceptionContext) == 184,
               "x86-64 exception context ABI mismatch");
#endif
