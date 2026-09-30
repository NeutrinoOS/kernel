#include "neutrino_syscall.h"

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static const char* g_dlerror;

/*
 * The kernel maps and relocates ELF objects, but constructors must run in the
 * calling userspace thread.  Keep that policy here rather than attempting to
 * execute user addresses while servicing the DynamicLoad syscall.
 */
enum {
    ELF_PT_DYNAMIC = 2,
    ELF_DT_NULL = 0,
    ELF_DT_NEEDED = 1,
    ELF_DT_STRTAB = 5,
    ELF_DT_INIT = 12,
    ELF_DT_INIT_ARRAY = 25,
    ELF_DT_INIT_ARRAYSZ = 27,
    MAX_DYNAMIC_OBJECTS = 64,
};

struct elf_header {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct elf_dynamic {
    int64_t tag;
    uint64_t value;
};

struct dynamic_initialization {
    uintptr_t handle;
    uint32_t owner;
    uint32_t state;
};

static struct dynamic_initialization g_initializations[MAX_DYNAMIC_OBJECTS];
static unsigned char g_initializations_lock;

static void lock_initializations(void) {
    while (__atomic_test_and_set(&g_initializations_lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static void unlock_initializations(void) {
    __atomic_clear(&g_initializations_lock, __ATOMIC_RELEASE);
}

static struct dynamic_initialization* initialization_for(uintptr_t handle) {
    struct dynamic_initialization* free_slot = NULL;
    lock_initializations();
    for (size_t i = 0; i < MAX_DYNAMIC_OBJECTS; ++i) {
        if (g_initializations[i].handle == handle) {
            unlock_initializations();
            return &g_initializations[i];
        }
        if (free_slot == NULL && g_initializations[i].handle == 0) {
            free_slot = &g_initializations[i];
        }
    }
    if (free_slot != NULL) {
        free_slot->handle = handle;
        free_slot->owner = 0;
        free_slot->state = 0;
    }
    unlock_initializations();
    return free_slot;
}

static const struct elf_dynamic* dynamic_table(uintptr_t handle) {
    const struct elf_header* header = (const struct elf_header*)handle;
    if (header->ident[0] != 0x7f || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2 ||
        header->phentsize != sizeof(struct elf_program_header)) {
        return NULL;
    }
    const unsigned char* base = (const unsigned char*)handle;
    const struct elf_program_header* programs =
        (const struct elf_program_header*)(base + header->phoff);
    for (uint16_t i = 0; i < header->phnum; ++i) {
        if (programs[i].type == ELF_PT_DYNAMIC) {
            return (const struct elf_dynamic*)(handle + programs[i].vaddr);
        }
    }
    return NULL;
}

static long raw_dynamic_load(const char* path, int mode) {
    return neutrino_raw_syscall2(NEUTRINO_DYNAMIC_LOAD,
                                 (long)(uintptr_t)path,
                                 mode);
}

static int initialize_dynamic_object(uintptr_t handle, int mode) {
    struct dynamic_initialization* initialization = initialization_for(handle);
    if (initialization == NULL) {
        return -1;
    }

    uint32_t thread = (uint32_t)neutrino_raw_syscall0(NEUTRINO_THREAD_ID);
    for (;;) {
        uint32_t state = __atomic_load_n(&initialization->state, __ATOMIC_ACQUIRE);
        if (state == 2) {
            return 0;
        }
        if (state == 1) {
            if (__atomic_load_n(&initialization->owner, __ATOMIC_RELAXED) == thread) {
                return 0;
            }
            __asm__ volatile("pause");
            continue;
        }
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&initialization->state,
                                        &expected,
                                        1,
                                        0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&initialization->owner, thread, __ATOMIC_RELAXED);
            break;
        }
    }

    const struct elf_dynamic* dynamic = dynamic_table(handle);
    if (dynamic == NULL) {
        __atomic_store_n(&initialization->state, 0, __ATOMIC_RELEASE);
        return -1;
    }

    uintptr_t strings = 0;
    uintptr_t init = 0;
    uintptr_t init_array = 0;
    size_t init_array_size = 0;
    for (const struct elf_dynamic* entry = dynamic;
         entry->tag != ELF_DT_NULL;
         ++entry) {
        if (entry->tag == ELF_DT_STRTAB) strings = handle + entry->value;
        if (entry->tag == ELF_DT_INIT && entry->value != 0)
            init = handle + entry->value;
        if (entry->tag == ELF_DT_INIT_ARRAY) init_array = handle + entry->value;
        if (entry->tag == ELF_DT_INIT_ARRAYSZ) init_array_size = entry->value;
    }

    if (strings != 0) {
        for (const struct elf_dynamic* entry = dynamic;
             entry->tag != ELF_DT_NULL;
             ++entry) {
            if (entry->tag != ELF_DT_NEEDED) continue;
            const char* needed = (const char*)(strings + entry->value);
            long dependency = raw_dynamic_load(needed, mode);
            if (dependency <= 0 ||
                initialize_dynamic_object((uintptr_t)dependency, mode) != 0) {
                __atomic_store_n(&initialization->owner, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&initialization->state, 0, __ATOMIC_RELEASE);
                return -1;
            }
        }
    }

    if (init != 0) {
        ((void (*)(void))init)();
    }
    if (init_array != 0) {
        void (**constructors)(void) = (void (**)(void))init_array;
        size_t count = init_array_size / sizeof(*constructors);
        for (size_t i = 0; i < count; ++i) {
            uintptr_t constructor = (uintptr_t)constructors[i];
            if (constructor != 0 && constructor != UINTPTR_MAX) {
                constructors[i]();
            }
        }
    }

    __atomic_store_n(&initialization->owner, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&initialization->state, 2, __ATOMIC_RELEASE);
    return 0;
}

void* dlopen(const char* path, int mode) {
    if (path == NULL || path[0] == '\0' ||
        (mode & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL)) != 0 ||
        (mode & (RTLD_LAZY | RTLD_NOW)) == 0) {
        errno = EINVAL;
        g_dlerror = "invalid dlopen arguments";
        return NULL;
    }
    long result = raw_dynamic_load(path, mode);
    if (result <= 0 || initialize_dynamic_object((uintptr_t)result, mode) != 0) {
        errno = ENOENT;
        g_dlerror = "unable to load or initialize shared object";
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
