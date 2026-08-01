#include <stdio.h>

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (index > 1) {
            putchar(' ');
        }
        fputs(argv[index], stdout);
    }
    putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
