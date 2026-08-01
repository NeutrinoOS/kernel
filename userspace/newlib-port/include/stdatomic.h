#pragma once

/*
 * Newlib's GCC-provided stdatomic.h names the stdint_t family without
 * including <stdint.h>.  Hosted toolchains often expose those names through
 * other headers, but freestanding consumers such as FFmpeg include
 * <stdatomic.h> directly.
 */
#include <stdint.h>
#include_next <stdatomic.h>

/* Newlib 4.5's initializer macros still assume its pre-C11 wrapper structs,
 * while GCC represents _Atomic(T) directly.  The operation macros already
 * use GCC's __atomic builtins, so only initialization needs correcting. */
#if defined(__GNUC__) && !defined(__clang__)
#undef ATOMIC_VAR_INIT
#undef atomic_init
#define ATOMIC_VAR_INIT(value) (value)
#define atomic_init(object, value) \
    atomic_store_explicit((object), (value), memory_order_relaxed)
#endif
