/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * mux.h
 *
 * https://github.com/hashicorp/yamux/blob/master/spec.md
 */

#ifndef YAMUX_MUX_H
#define YAMUX_MUX_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "tree.h"
#include "bio.h"

typedef struct stream {
    RB_ENTRY(stream)
    entry;
    struct session *session;    /* stream session */
    uint8_t state;              /* stream state */
    uint32_t id;                /* stream id */
    uint32_t send_padding;      /* send buffer padding size */
    uint32_t recv_window;       /* recv window size, recv_lock */
    uint32_t send_window;       /* send window size, send_lock */
    bio_t *recv_buf;            /* recv buffer, recv_lock */
    bool readable;              /* fd readable, recv_lock */
    bool writeable;             /* fd writeable, send_lock */
    pthread_mutex_t state_lock; /* state lock */
    pthread_mutex_t recv_lock;  /* recv buffer lock */
    pthread_mutex_t send_lock;  /* send lock */
    int fd;                     /* public fd */
    int private_fd;             /* private fd */
} stream_t;

RB_HEAD(streamtree, stream);

typedef struct session {
    atomic_bool remote_go_away;   /* remote go away */
    atomic_bool local_go_away;    /* local go away */
    atomic_uint next_id;          /* next stream id */
    bool new_frame;               /* Whether the received data is a new frame */
    uint32_t frame_remain;        /* The remaining data length in the current frame */
    bio_t *recv_buf;              /* recv buffer, recv_lock */
    bio_t *send_buf;              /* send buffer, send_lock */
    struct streamtree streams;    /* Associate FD using ID, streams_lock */
    pthread_mutex_t recv_lock;    /* recv buffer lock */
    pthread_mutex_t send_lock;    /* send buffer lock */
    pthread_mutex_t streams_lock; /* stream tree lock */
    char hdr[12];                 /* frame header */
    int fd;                       /* public fd */
    int private_fd;               /* private fd */
} session_t;

#define session_client_init(session) session_init(session, true)
#define session_server_init(session) session_init(session, false)

int session_init(session_t *session, bool client);
int session_close(session_t *session);
int session_process(session_t *session);
int session_ping(session_t *session);
int session_go_away(session_t *session);
int session_recv_buf_write(session_t *session, const void *data, size_t n);
int session_send_buf_pending(session_t *session);
int session_send_buf_peek(session_t *session, void *buf, size_t n);
int session_send_buf_consume(session_t *session, size_t n);

int session_open(session_t *session, stream_t **stream);
int session_accept(session_t *session, stream_t **stream);

int stream_read(stream_t *stream, void *buf, size_t size);
int stream_write(stream_t *stream, const void *data, size_t n);
void stream_close(stream_t *stream);

#endif /* YAMUX_MUX_H */
