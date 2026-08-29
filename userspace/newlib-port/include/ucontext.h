#pragma once

#include <signal.h>
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

int getcontext(ucontext_t*);
int setcontext(const ucontext_t*);
void makecontext(ucontext_t*, void (*)(), int, ...);
int swapcontext(ucontext_t*, const ucontext_t*);

#ifdef __cplusplus
}
#endif
