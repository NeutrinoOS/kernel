#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static int parse_duration(const char* text, struct timespec* duration) {
    if (text == NULL || text[0] == '\0' || duration == NULL) {
        return -1;
    }

    uint64_t seconds = 0;
    size_t index = 0;
    int saw_digit = 0;
    while (text[index] >= '0' && text[index] <= '9') {
        saw_digit = 1;
        uint64_t digit = (uint64_t)(text[index] - '0');
        if (seconds > (UINT64_MAX - digit) / 10u) {
            return -1;
        }
        seconds = seconds * 10u + digit;
        ++index;
    }

    long nanoseconds = 0;
    long place = 100000000L;
    if (text[index] == '.') {
        ++index;
        while (text[index] >= '0' && text[index] <= '9') {
            saw_digit = 1;
            if (place != 0) {
                nanoseconds += (long)(text[index] - '0') * place;
                place /= 10;
            }
            ++index;
        }
    }

    if (!saw_digit || text[index] != '\0' ||
        seconds > (uint64_t)INT64_MAX) {
        return -1;
    }
    duration->tv_sec = (time_t)seconds;
    duration->tv_nsec = nanoseconds;
    return 0;
}

int main(int argc, char** argv) {
    struct timespec duration;
    if (argc != 2 || parse_duration(argv[1], &duration) < 0) {
        fputs("usage: sleep <seconds>\n", stderr);
        return 1;
    }
    if (nanosleep(&duration, NULL) < 0) {
        perror("sleep");
        return errno == EINTR ? 128 : 1;
    }
    return 0;
}
