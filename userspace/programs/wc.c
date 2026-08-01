#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fputs("usage: wc <path>\n", stderr);
        return 1;
    }

    FILE* input = fopen(argv[1], "r");
    if (input == NULL) {
        fprintf(stderr, "wc: unable to open %s: %s\n",
                argv[1],
                strerror(errno));
        return 1;
    }

    uintmax_t lines = 0;
    uintmax_t words = 0;
    uintmax_t bytes = 0;
    bool in_word = false;
    unsigned char buffer[1024];
    int status = 0;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        bytes += count;
        for (size_t index = 0; index < count; ++index) {
            unsigned char character = buffer[index];
            if (character == '\n') {
                ++lines;
            }
            if (isspace(character)) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                ++words;
            }
        }
        if (count != sizeof(buffer)) {
            if (ferror(input)) {
                fprintf(stderr, "wc: read failed: %s\n", strerror(errno));
                status = 1;
            }
            break;
        }
    }

    if (fclose(input) != 0) {
        status = 1;
    }
    if (status == 0) {
        printf("%" PRIuMAX " %" PRIuMAX " %" PRIuMAX " %s\n",
               lines,
               words,
               bytes,
               argv[1]);
    }
    return status;
}
