#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: rmdir <path>\n", stderr);
        return 1;
    }
    if (rmdir(argv[1]) < 0) {
        fprintf(stderr, "rmdir: unable to remove %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }
    return 0;
}
