#include <stdio.h>
#include <time.h>

int main(void) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        fputs("date: time unavailable\n", stderr);
        return 1;
    }

    struct tm* utc = gmtime(&now);
    if (utc == NULL) {
        fputs("date: unable to convert time\n", stderr);
        return 1;
    }

    char output[32];
    if (strftime(output, sizeof(output), "%Y-%m-%d %H:%M:%S", utc) == 0) {
        fputs("date: unable to format time\n", stderr);
        return 1;
    }
    puts(output);
    return 0;
}
