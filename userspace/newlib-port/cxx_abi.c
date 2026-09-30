/* Minimal C++ ABI support for C++ DSOs loaded by a static Newlib launcher. */
#include <stddef.h>
#include <stdlib.h>

/*
 * The Itanium C++ ABI uses the first byte of an eight-byte guard object for
 * initialization state.  State 2 denotes an initializer in progress, and
 * state 1 denotes completion.  Keeping this here, rather than in every DSO,
 * gives process-wide function-local-static semantics.
 */
int __cxa_guard_acquire(long long* guard) {
  unsigned char* state = (unsigned char*)guard;
  for (;;) {
    unsigned char value = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    if (value == 1) {
      return 0;
    }
    if (value == 0) {
      unsigned char expected = 0;
      if (__atomic_compare_exchange_n(state, &expected, 2, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 1;
      }
    }
  }
}

void __cxa_guard_release(long long* guard) {
  __atomic_store_n((unsigned char*)guard, 1, __ATOMIC_RELEASE);
}

void __cxa_guard_abort(long long* guard) {
  __atomic_store_n((unsigned char*)guard, 0, __ATOMIC_RELEASE);
}

static void* cxx_allocate(size_t size) {
  if (size == 0) {
    size = 1;
  }
  void* result = malloc(size);
  if (result == NULL) {
    abort();
  }
  return result;
}

void* cxx_operator_new(size_t size) __asm__("_Znwm");
void* cxx_operator_new(size_t size) { return cxx_allocate(size); }
void* cxx_operator_new_array(size_t size) __asm__("_Znam");
void* cxx_operator_new_array(size_t size) { return cxx_allocate(size); }
void cxx_operator_delete(void* pointer) __asm__("_ZdlPv");
void cxx_operator_delete(void* pointer) { free(pointer); }
void cxx_operator_delete_array(void* pointer) __asm__("_ZdaPv");
void cxx_operator_delete_array(void* pointer) { free(pointer); }
void cxx_operator_delete_sized(void* pointer, size_t size) __asm__("_ZdlPvm");
void cxx_operator_delete_sized(void* pointer, size_t size) {
  (void)size;
  free(pointer);
}
void cxx_operator_delete_array_sized(void* pointer, size_t size) __asm__("_ZdaPvm");
void cxx_operator_delete_array_sized(void* pointer, size_t size) {
  (void)size;
  free(pointer);
}

void __cxa_pure_virtual(void) { abort(); }
