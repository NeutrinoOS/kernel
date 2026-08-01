#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DT_UNKNOWN = 0,
    DT_REG = 8,
    DT_DIR = 4,
};

struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct __neutrino_dir DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* directory);
int closedir(DIR* directory);
void rewinddir(DIR* directory);
long telldir(DIR* directory);
void seekdir(DIR* directory, long location);

#ifdef __cplusplus
}
#endif
