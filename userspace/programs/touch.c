#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: touch <path>\n", stderr);
        return 1;
    }

    int descriptor = open(argv[1], O_WRONLY | O_CREAT, 0666);
    if (descriptor < 0) {
        fprintf(stderr, "touch: unable to touch %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }
    if (close(descriptor) < 0) {
        fprintf(stderr, "touch: unable to close %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }
    return 0;
}
