#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413ul

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
