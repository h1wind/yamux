/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * debug.h
 */

#ifndef YAMUX_DEBUG_H
#define YAMUX_DEBUG_H

#include <stdarg.h>

#if !defined(NDEBUG)
void debug(const char *file, int line, const char *fmt, ...);
#define dbg(...) debug(__FILE__, __LINE__, __VA_ARGS__)
#else
#define dbg(...) (void)0
#endif

#endif /* YAMUX_DEBUG_H */
