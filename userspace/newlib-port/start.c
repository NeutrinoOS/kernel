#include <stddef.h>
#include <stdlib.h>

enum {
    kMaxArguments = 64,
};

extern int main(int argc, char** argv, char** envp);
extern void __libc_init_array(void);

extern char** environ;

static char g_empty_argument[] = "";
static char* g_empty_environment[] = {NULL};

static int split_arguments(char* input, char** argv, int capacity) {
    int argc = 1;
    argv[0] = g_empty_argument;

    if (input == NULL) {
        argv[argc] = NULL;
        return argc;
    }

    char* read = input;
    char* write = input;
    while (*read != '\0' && argc < capacity - 1) {
        while (*read == ' ' || *read == '\t' || *read == '\n') {
            ++read;
        }
        if (*read == '\0') {
            break;
        }

        argv[argc++] = write;
        char quote = '\0';
        while (*read != '\0') {
            char ch = *read++;
            if (quote != '\0') {
                if (ch == quote) {
                    quote = '\0';
                    continue;
                }
                if (ch == '\\' && quote == '"' && *read != '\0') {
                    ch = *read++;
                }
                *write++ = ch;
                continue;
            }

            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (ch == '\\' && *read != '\0') {
                *write++ = *read++;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n') {
                break;
            }
            *write++ = ch;
        }
        *write++ = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

__attribute__((noreturn))
void neutrino_crt_start(char* raw_arguments) {
    char* argv[kMaxArguments];
    int argc = split_arguments(raw_arguments, argv, kMaxArguments);

    environ = g_empty_environment;
    __libc_init_array();
    exit(main(argc, argv, environ));
}
