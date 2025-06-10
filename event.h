/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * event.h
 */

#ifndef YAMUX_EVENT_H
#define YAMUX_EVENT_H

#include <stddef.h>
#include <stdint.h>

#define EVENT_NONE 0x00
#define EVENT_IN   0x01
#define EVENT_OUT  0x02

typedef struct event_loop event_loop_t;

typedef void event_fn_t(event_loop_t *loop, int fd, void *data);

event_loop_t *event_loop_new(void);
void event_loop_free(event_loop_t *loop);
int event_process(event_loop_t *loop);
void event_stop(event_loop_t *loop);

/**
 * @brief Add specified read and write events
 *
 * @param loop
 * @param fd
 * @param flags EVENT_IN | EVENT_OUT
 * @param fn
 * @param data
 * @return int Returns zero on success, non-zero on failure
 */
int event_add(event_loop_t *loop, int fd, uint32_t flags, event_fn_t *fn,
              void *data);

/**
 * @brief Delete the specified read and write event
 *
 * @param loop
 * @param fd
 * @param flags
 * @return int Returns zero on success, non-zero on failure
 */
int event_delete(event_loop_t *loop, int fd, uint32_t flags);

#endif /* YAMUX_EVENT_H */
