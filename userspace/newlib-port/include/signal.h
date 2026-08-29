#pragma once

/* Ask Newlib for its POSIX sigaction shape, including sa_sigaction. */
#ifndef _POSIX_REALTIME_SIGNALS
#define _POSIX_REALTIME_SIGNALS 200809L
#endif
#define __NEUTRINO_NEWLIB_RTEMS_SIGNAL_SHIM 1
#define __rtems__ 1
#include_next <signal.h>
#undef __rtems__

/* The Newlib realtime siginfo payload uses sigval; fault delivery uses it as
 * an address until the native signal ABI grows a fuller siginfo union. */
#define si_addr si_value.sival_ptr

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0x40000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif
#ifndef SEGV_ACCERR
#define SEGV_ACCERR 2
#endif
