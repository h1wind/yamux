/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * debug.c
 */

#include "debug.h"

#include <stdio.h>

#include "misc.h"

#if !defined(NDEBUG)

static void vdebug(const char *file, int line, const char *fmt, va_list ap) {
    char buf[8192];

    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (file && line > 0) {
        fprintf(stderr, "%s:%d: %s\n", xbasename(file), line, buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
    fflush(stderr);
}

void debug(const char *file, int line, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vdebug(file, line, fmt, ap);
    va_end(ap);
}

#endif
