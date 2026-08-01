#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[512];
    if (getcwd(path, sizeof(path)) == NULL) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(path);
    return 0;
}
