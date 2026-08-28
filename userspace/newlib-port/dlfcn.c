#include "neutrino_syscall.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>

static const char* g_dlerror;

void* dlopen(const char* path, int mode) {
    if (path == NULL || path[0] == '\0' ||
        (mode & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL)) != 0 ||
        (mode & (RTLD_LAZY | RTLD_NOW)) == 0) {
        errno = EINVAL;
        g_dlerror = "invalid dlopen arguments";
        return NULL;
    }
    long result = neutrino_raw_syscall2(NEUTRINO_DYNAMIC_LOAD,
                                        (long)(uintptr_t)path,
                                        mode);
    if (result <= 0) {
        errno = ENOENT;
        g_dlerror = "unable to load shared object";
        return NULL;
    }
    g_dlerror = NULL;
    return (void*)(uintptr_t)result;
}

void* dlsym(void* handle, const char* name) {
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        g_dlerror = "invalid symbol name";
        return NULL;
    }
    long result = neutrino_raw_syscall2(NEUTRINO_DYNAMIC_SYMBOL,
                                        (long)(uintptr_t)handle,
                                        (long)(uintptr_t)name);
    if (result == 0) {
        g_dlerror = "symbol not found";
        return NULL;
    }
    g_dlerror = NULL;
    return (void*)(uintptr_t)result;
}

int dlclose(void* handle) {
    if (handle == NULL ||
        neutrino_raw_syscall1(NEUTRINO_DYNAMIC_CLOSE,
                              (long)(uintptr_t)handle) < 0) {
        errno = EINVAL;
        g_dlerror = "invalid dynamic-library handle";
        return -1;
    }
    g_dlerror = NULL;
    return 0;
}

char* dlerror(void) {
    const char* result = g_dlerror;
    g_dlerror = NULL;
    return (char*)result;
}
