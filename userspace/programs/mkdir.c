#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: mkdir <path>\n", stderr);
        return 1;
    }
    if (mkdir(argv[1], 0777) < 0) {
        fprintf(stderr, "mkdir: unable to create %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }
    return 0;
}
