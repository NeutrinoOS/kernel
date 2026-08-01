#include "neutrino_syscall.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct __neutrino_dir {
    uint32_t handle;
    long location;
    char* path;
    struct dirent current;
};

static long open_directory(const char* path) {
    return neutrino_raw_syscall1(
        NEUTRINO_DIRECTORY_OPEN, (long)(uintptr_t)path);
}

DIR* opendir(const char* path) {
    if (path == NULL || path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    long handle = open_directory(path);
    if (handle < 0 || (unsigned long)handle > UINT32_MAX) {
        errno = ENOENT;
        return NULL;
    }

    DIR* directory = malloc(sizeof(*directory));
    if (directory == NULL) {
        (void)neutrino_raw_syscall1(
            NEUTRINO_DIRECTORY_CLOSE, handle);
        errno = ENOMEM;
        return NULL;
    }

    size_t path_length = strlen(path);
    directory->path = malloc(path_length + 1);
    if (directory->path == NULL) {
        (void)neutrino_raw_syscall1(
            NEUTRINO_DIRECTORY_CLOSE, handle);
        free(directory);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(directory->path, path, path_length + 1);

    directory->handle = (uint32_t)handle;
    directory->location = 0;
    directory->current = (struct dirent){0};
    return directory;
}

struct dirent* readdir(DIR* directory) {
    if (directory == NULL) {
        errno = EBADF;
        return NULL;
    }

    struct neutrino_directory_entry native_entry = {0};
    long result = neutrino_raw_syscall2(
        NEUTRINO_DIRECTORY_READ,
        directory->handle,
        (long)(uintptr_t)&native_entry);
    if (result < 0) {
        errno = EIO;
        return NULL;
    }
    if (result == 0) {
        return NULL;
    }

    directory->current = (struct dirent){0};
    directory->current.d_off = ++directory->location;
    directory->current.d_reclen = sizeof(directory->current);
    directory->current.d_type =
        (native_entry.flags & NEUTRINO_DIRECTORY_ENTRY_DIRECTORY) != 0
            ? DT_DIR
            : DT_REG;
    memcpy(directory->current.d_name,
           native_entry.name,
           sizeof(native_entry.name));
    directory->current.d_name[sizeof(native_entry.name)] = '\0';
    return &directory->current;
}

int closedir(DIR* directory) {
    if (directory == NULL) {
        errno = EBADF;
        return -1;
    }

    long result = neutrino_raw_syscall1(
        NEUTRINO_DIRECTORY_CLOSE, directory->handle);
    free(directory->path);
    free(directory);
    if (result < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

void rewinddir(DIR* directory) {
    if (directory == NULL) {
        errno = EBADF;
        return;
    }

    long replacement = open_directory(directory->path);
    if (replacement < 0 || (unsigned long)replacement > UINT32_MAX) {
        errno = EIO;
        return;
    }

    (void)neutrino_raw_syscall1(
        NEUTRINO_DIRECTORY_CLOSE, directory->handle);
    directory->handle = (uint32_t)replacement;
    directory->location = 0;
}

long telldir(DIR* directory) {
    if (directory == NULL) {
        errno = EBADF;
        return -1;
    }
    return directory->location;
}

void seekdir(DIR* directory, long location) {
    if (directory == NULL || location < 0) {
        errno = EINVAL;
        return;
    }

    errno = 0;
    rewinddir(directory);
    if (errno != 0 || location == 0) {
        return;
    }
    for (long index = 0; index < location; ++index) {
        errno = 0;
        if (readdir(directory) == NULL) {
            if (errno == 0) {
                errno = EINVAL;
            }
            return;
        }
    }
}
