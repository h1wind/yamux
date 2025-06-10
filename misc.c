/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * misc.c
 */

#include "misc.h"

#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"

size_t xstrftime(char *s, size_t size, time_t time) {
    struct tm *tp = localtime(&time);

    if (time > 0) {
        return strftime(s, size, "%Y-%m-%d %H:%M:%S", tp);
    }
    return snprintf(s, size, "0000-00-00 00:00:00");
}

const char *xbasename(const char *str) {
    const char *s = NULL;

    while (*str) {
        if (*str++ == '/') {
            s = str;
        }
    }
    return s;
}

char *xstrdup(const char *str) {
    char *ptr, *s;
    size_t n = 0;

    assert(str);

    while ('\0' != str[n++]) {
    }

    ptr = malloc(n);
    if (!ptr) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }

    s = ptr;
    while (n--) {
        *s++ = *str++;
    }

    return ptr;
}

int setblock(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
}

int setnonblock(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

bool would_block(int fd) {
    int r;

    r = fcntl(fd, F_GETFL);
    if (r == -1) {
        dbg("fcntl: %s", strerror(errno));
        return false;
    }
    if (r & O_NONBLOCK) {
        return false;
    }
    return true;
}
