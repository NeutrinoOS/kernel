#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: rm <path>\n", stderr);
        return 1;
    }
    if (unlink(argv[1]) == 0 || rmdir(argv[1]) == 0) {
        return 0;
    }
    fprintf(stderr, "rm: unable to remove %s: %s\n",
            argv[1],
            strerror(errno));
    return 1;
}
