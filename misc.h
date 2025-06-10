/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * misc.h
 */

#ifndef YAMUX_MISC_H
#define YAMUX_MISC_H

#include <time.h>
#include <stddef.h>
#include <stdbool.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

size_t xstrftime(char *s, size_t size, time_t time);
const char *xbasename(const char *str);
char *xstrdup(const char *str);
int setblock(int fd);
int setnonblock(int fd);
bool would_block(int fd);

#endif /* YAMUX_MISC_H */
