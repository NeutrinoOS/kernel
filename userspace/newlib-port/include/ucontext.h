#pragma once

#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucontext {
    struct ucontext* uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
} ucontext_t;

/* Newlib provides setjmp/longjmp but not the POSIX spellings.  Neutrino has
 * no process signal-mask state to preserve yet, so save_mask is intentionally
 * ignored. */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(environment, save_mask) setjmp(environment)
#define siglongjmp(environment, value) longjmp(environment, value)

int getcontext(ucontext_t*);
int setcontext(const ucontext_t*);
void makecontext(ucontext_t*, void (*)(), int, ...);
int swapcontext(ucontext_t*, const ucontext_t*);

#ifdef __cplusplus
}
#endif
