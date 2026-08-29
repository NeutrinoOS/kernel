#pragma once

#include <stdint.h>
#include <neutrino/exception_context.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t greg_t;
typedef greg_t gregset_t[23];

/* Linux-compatible indices used by x86-64 runtimes such as HotSpot. */
enum {
    REG_R8 = 0,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_RDI,
    REG_RSI,
    REG_RBP,
    REG_RBX,
    REG_RDX,
    REG_RAX,
    REG_RCX,
    REG_RSP,
    REG_RIP,
    REG_EFL,
    REG_CSGSFS,
    REG_ERR,
    REG_TRAPNO,
    REG_OLDMASK,
    REG_CR2,
};

typedef struct {
    gregset_t gregs;
    /* Canonical Neutrino representation; gregs mirrors its GPR state. */
    NeuExceptionContext native;
} mcontext_t;

static inline void neutrino_mcontext_from_exception(
    mcontext_t* destination, const NeuExceptionContext* source) {
    if (destination == 0 || source == 0) {
        return;
    }
    destination->native = *source;
    destination->gregs[REG_R8] = source->r8;
    destination->gregs[REG_R9] = source->r9;
    destination->gregs[REG_R10] = source->r10;
    destination->gregs[REG_R11] = source->r11;
    destination->gregs[REG_R12] = source->r12;
    destination->gregs[REG_R13] = source->r13;
    destination->gregs[REG_R14] = source->r14;
    destination->gregs[REG_R15] = source->r15;
    destination->gregs[REG_RDI] = source->rdi;
    destination->gregs[REG_RSI] = source->rsi;
    destination->gregs[REG_RBP] = source->rbp;
    destination->gregs[REG_RBX] = source->rbx;
    destination->gregs[REG_RDX] = source->rdx;
    destination->gregs[REG_RAX] = source->rax;
    destination->gregs[REG_RCX] = source->rcx;
    destination->gregs[REG_RSP] = source->rsp;
    destination->gregs[REG_RIP] = source->rip;
    destination->gregs[REG_EFL] = source->rflags;
    destination->gregs[REG_CSGSFS] = source->cs;
    destination->gregs[REG_ERR] = source->error_code;
    destination->gregs[REG_TRAPNO] = source->exception;
    destination->gregs[REG_OLDMASK] = 0;
    destination->gregs[REG_CR2] = source->fault_address;
}

#ifdef __cplusplus
}
#endif
