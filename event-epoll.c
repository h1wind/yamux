/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * event-epoll.c
 */

#include "event.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "debug.h"
#include "tree.h"

struct event {
    // clang-format off
    RB_ENTRY(event) entry;
    // clang-format on
    int fd;
    uint32_t flags;
    event_fn_t *send_fn;
    event_fn_t *recv_fn;
    void *send_data;
    void *recv_data;
};

RB_HEAD(event_tree, event);

struct event_loop {
    /* Perhaps it can be modified into an array to exchange resources for time. */
    struct event_tree events;
    int epfd;
    bool stop;
};

static int event_compare(struct event *e1, struct event *e2);

RB_PROTOTYPE_STATIC(event_tree, event, entry, event_compare)
RB_GENERATE_STATIC(event_tree, event, entry, event_compare)

static int event_compare(struct event *e1, struct event *e2) {
    return e1->fd - e2->fd;
}

static struct event *find_event(event_loop_t *loop, int fd) {
    struct event find, *res;

    find.fd = fd;
    res = RB_FIND(event_tree, &loop->events, &find);

    return (res && res->fd == fd) ? res : NULL;
}

event_loop_t *event_loop_new(void) {
    event_loop_t *loop;

    loop = malloc(sizeof(*loop));
    if (!loop) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }

    RB_INIT(&loop->events);

    /* the size argument is ignored, but must be greater than zero */
    loop->epfd = epoll_create(1024);
    if (loop->epfd == -1) {
        dbg("epoll_create: %s", strerror(errno));
        goto err;
    }

    loop->stop = false;

    return loop;
err:
    free(loop);
    return NULL;
}

void event_loop_free(event_loop_t *loop) {
    struct event *event, *tmp;

    RB_FOREACH_SAFE(event, event_tree, &loop->events, tmp) {
        free(event);
    }

    close(loop->epfd);
    free(loop);
}

static void handle_event(event_loop_t *loop, struct epoll_event *ee) {
    struct event *event;

    event = ee->data.ptr;
    if ((ee->events & EPOLLIN) && event->recv_fn) {
        event->recv_fn(loop, event->fd, event->recv_data);
    }
    if ((ee->events & EPOLLOUT) && event->send_fn) {
        event->send_fn(loop, event->fd, event->send_data);
    }
}

int event_process(event_loop_t *loop) {
    struct epoll_event events[256];
    int i, r;

    while (!loop->stop) {
        r = epoll_wait(loop->epfd, events, 256, -1);
        if (r == -1) {
            dbg("epoll_wait: %s", strerror(errno));
            return -1;
        }
        for (i = 0; i < r; i++) {
            handle_event(loop, &events[i]);
        }
    }
    return 0;
}

void event_stop(event_loop_t *loop) {
    loop->stop = true;
}

static struct event *event_new(int fd) {
    struct event *event;

    event = malloc(sizeof(*event));
    if (!event) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }

    event->fd = fd;
    event->flags = EVENT_NONE;
    event->send_fn = NULL;
    event->recv_fn = NULL;
    event->send_data = NULL;
    event->recv_data = NULL;

    return event;
}

static void event_free(struct event *event) {
    free(event);
}

int event_add(event_loop_t *loop, int fd, uint32_t flags, event_fn_t *fn,
              void *data) {
    struct epoll_event ee;
    struct event *event;
    int r, op;

    event = find_event(loop, fd);
    if (!event) {
        event = event_new(fd);
        if (!event) {
            dbg("event_new");
            return -1;
        }
        RB_INSERT(event_tree, &loop->events, event);
    }

    if (flags & EVENT_IN) {
        event->recv_fn = fn;
        event->recv_data = data;
    }
    if (flags & EVENT_OUT) {
        event->send_fn = fn;
        event->send_data = data;
    }

    ee.data.ptr = event;
    ee.events = 0;

    op = (event->flags == EVENT_NONE) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    event->flags |= flags;

    if (event->flags & EVENT_IN) {
        ee.events |= EPOLLIN;
    }
    if (event->flags & EVENT_OUT) {
        ee.events |= EPOLLOUT;
    }

    r = epoll_ctl(loop->epfd, op, fd, &ee);
    if (r) {
        dbg("epoll_ctl: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int event_delete(event_loop_t *loop, int fd, uint32_t flags) {
    struct epoll_event ee;
    struct event *event;
    int r, op;

    event = find_event(loop, fd);
    if (!event) {
        dbg("event not found");
        return -1;
    }

    event->flags &= (~flags);
    ee.events = 0;
    ee.data.ptr = event;
    op = (event->flags == EVENT_NONE) ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;

    if (event->flags & EVENT_IN) {
        ee.events |= EPOLLIN;
    }
    if (event->flags & EVENT_OUT) {
        ee.events |= EPOLLOUT;
    }

    /* Remove (deregister) the target file descriptor fd from the interest list.
     * The event argument is ignored and can be NULL */
    r = epoll_ctl(loop->epfd, op, fd, &ee);
    if (r) {
        dbg("epoll_ctl: %s", strerror(errno));
        return -1;
    }

    if (!event->flags) {
        RB_REMOVE(event_tree, &loop->events, event);
        event_free(event);
    }
    return 0;
}
