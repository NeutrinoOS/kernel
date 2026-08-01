#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: cat <path>\n", stderr);
        return 1;
    }

    FILE* input = fopen(argv[1], "r");
    if (input == NULL) {
        fprintf(stderr, "cat: unable to open %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }

    unsigned char buffer[1024];
    int status = 0;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count != 0 && fwrite(buffer, 1, count, stdout) != count) {
            fprintf(stderr, "cat: write failed: %s\n", strerror(errno));
            status = 1;
            break;
        }
        if (count != sizeof(buffer)) {
            if (ferror(input)) {
                fprintf(stderr, "cat: read failed: %s\n", strerror(errno));
                status = 1;
            }
            break;
        }
    }

    if (fclose(input) != 0) {
        fprintf(stderr, "cat: unable to close %s: %s\n",
                argv[1],
                strerror(errno));
        status = 1;
    }
    return status;
}
