#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : ".";
    DIR* directory = opendir(path);
    if (directory == NULL) {
        fprintf(stderr, "ls: unable to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    int status = 0;
    errno = 0;
    for (;;) {
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                fprintf(stderr, "ls: unable to read %s: %s\n",
                        path,
                        strerror(errno));
                status = 1;
            }
            break;
        }
        printf("%s%s\n",
               entry->d_name,
               entry->d_type == DT_DIR ? "/" : "");
        errno = 0;
    }

    if (closedir(directory) < 0) {
        fprintf(stderr, "ls: unable to close %s: %s\n", path, strerror(errno));
        status = 1;
    }
    return status;
}
