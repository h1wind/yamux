/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * mux.c
 */

#include "mux.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "debug.h"
#include "misc.h"

#define VERSION 0

/* type */
#define TYPE_DATA          0x00
#define TYPE_WINDOW_UPDATE 0x01
#define TYPE_PING          0x02
#define TYPE_GO_AWAY       0x03

/* flags */
#define FLAG_SYN 0x01
#define FLAG_ACK 0x02
#define FLAG_FIN 0x04
#define FLAG_RST 0x08

/* go away */
#define GO_AWAY_NORMAL       0x00
#define GO_AWAY_PROTO_ERR    0x01
#define GO_AWAY_INTERNAL_ERR 0x02

/* config */
#define MAX_STREAM_WINDOW_SIZE 1024 * 256 /* 256kb */

#define hdr_encode(h, type, flags, id, len)    \
    do {                                       \
        *(uint8_t *)(h) = VERSION;             \
        *(uint8_t *)((h) + 1) = (type);        \
        *(uint16_t *)((h) + 2) = htons(flags); \
        *(uint32_t *)((h) + 4) = htonl(id);    \
        *(uint32_t *)((h) + 8) = htonl(len);   \
    } while (false)

#define hdr_version(h)   (*(uint8_t *)(h))
#define hdr_type(h)      (*(uint8_t *)(h + 1))
#define hdr_flags(h)     (ntohs(*(uint16_t *)(h + 2)))
#define hdr_stream_id(h) (ntohl(*(uint32_t *)(h + 4)))
#define hdr_length(h)    (ntohl(*(uint32_t *)(h + 8)))

enum {
    STATE_INIT = 0x00,
    STATE_SYN_SENT,
    STATE_SYN_RECEIVED,
    STATE_ESTABLISHED,
    STATE_LOCAL_CLOSE,
    STATE_REMOTE_CLOSE,
    STATE_CLOSED,
    STATE_RESET,
};

static int stream_compare(stream_t *s1, stream_t *s2);

RB_PROTOTYPE_STATIC(streamtree, stream, entry, stream_compare)
RB_GENERATE_STATIC(streamtree, stream, entry, stream_compare)

static int stream_compare(stream_t *s1, stream_t *s2) {
    return (s1->id < s2->id ? -1 : s1->id > s2->id);
}

static void session_insert_stream(session_t *session, stream_t *stream) {
    stream_t *res;

    pthread_mutex_lock(&session->streams_lock);
    res = RB_INSERT(streamtree, &session->streams, stream);
    assert(res == NULL);
    pthread_mutex_unlock(&session->streams_lock);
}

static void session_remove_stream(session_t *session, stream_t *stream) {
    stream_t *res;

    pthread_mutex_lock(&session->streams_lock);
    res = RB_FIND(streamtree, &session->streams, stream);
    if (res->id == stream->id) {
        RB_REMOVE(streamtree, &session->streams, stream);
    }
    pthread_mutex_unlock(&session->streams_lock);
}

static stream_t *session_find_stream(session_t *session, uint32_t id) {
    stream_t find, *res;

    find.id = id;
    pthread_mutex_lock(&session->streams_lock);
    res = RB_FIND(streamtree, &session->streams, &find);
    pthread_mutex_unlock(&session->streams_lock);
    return (res && res->id == id) ? res : NULL;
}

static int stream_writeable_notify(stream_t *stream) {
    char buf[4096], *ptr = buf;

    pthread_mutex_lock(&stream->send_lock);
    if (!stream->writeable) {
        if (stream->send_padding > sizeof(buf)) {
            ptr = malloc(stream->send_padding);
            if (!ptr) {
                dbg("malloc: %s", strerror(errno));
                pthread_mutex_unlock(&stream->send_lock);
                return -1;
            }
        }
        if (read(stream->private_fd, ptr, stream->send_padding) <= 0) {
            dbg("read: %s", strerror(errno));
            if (ptr != buf) {
                free(ptr);
            }
            pthread_mutex_unlock(&stream->send_lock);
            return -1;
        }
        if (ptr != buf) {
            free(ptr);
        }
        stream->writeable = true;
    }
    pthread_mutex_unlock(&stream->send_lock);
    return 0;
}

static int stream_writeable_waiting(stream_t *stream) {
    char buf[4096], *ptr = buf;

    pthread_mutex_lock(&stream->send_lock);
    if (stream->writeable) {
        if (stream->send_padding > sizeof(buf)) {
            ptr = malloc(stream->send_padding);
            if (!ptr) {
                dbg("malloc: %s", strerror(errno));
                if (ptr != buf) {
                    free(ptr);
                }
                pthread_mutex_unlock(&stream->send_lock);
                return -1;
            }
        }
        if (write(stream->fd, ptr, stream->send_padding) <= 0) {
            dbg("write: %s", strerror(errno));
            if (ptr != buf) {
                free(ptr);
            }
            pthread_mutex_unlock(&stream->send_lock);
            return -1;
        }
        if (ptr != buf) {
            free(ptr);
        }
        stream->writeable = false;
    }
    pthread_mutex_unlock(&stream->send_lock);
    return 0;
}

static int stream_readable_notify(stream_t *stream) {
    uint8_t event = 0;

    pthread_mutex_lock(&stream->recv_lock);
    /* It is already in a readable state and does not need to be set again. */
    if (!stream->readable) {
        if (write(stream->private_fd, &event, sizeof(event)) <= 0) {
            /* Trigger the readable event of fd by writing one byte of data to
             * private_fd */
            dbg("write: %s", strerror(errno));
            pthread_mutex_unlock(&stream->recv_lock);
            return -1;
        }
        stream->readable = true;
    }
    pthread_mutex_unlock(&stream->recv_lock);
    return 0;
}

static int stream_readable_waiting(stream_t *stream) {
    uint8_t event;

    pthread_mutex_lock(&stream->recv_lock);
    if (stream->readable) {
        if (read(stream->fd, &event, sizeof(event)) <= 0) {
            dbg("read: %s", strerror(errno));
            pthread_mutex_unlock(&stream->recv_lock);
            return -1;
        }
        stream->readable = false;
    }
    pthread_mutex_unlock(&stream->recv_lock);
    return 0;
}

static void stream_notify_waiting(stream_t *stream) {
    stream_writeable_notify(stream);
    stream_readable_notify(stream);
}

static int stream_setup_fd(stream_t *stream) {
    socklen_t len;
    int size;

    len = sizeof(size);
    size = 256;

    if (setsockopt(stream->fd, SOL_SOCKET, SO_RCVBUF, &size, len) ||
        setsockopt(stream->fd, SOL_SOCKET, SO_SNDBUF, &size, len) ||
        setsockopt(stream->private_fd, SOL_SOCKET, SO_RCVBUF, &size, len) ||
        setsockopt(stream->private_fd, SOL_SOCKET, SO_SNDBUF, &size, len)) {
        dbg("setsockopt: %s", strerror(errno));
        return -1;
    }

    if (getsockopt(stream->private_fd, SOL_SOCKET, SO_RCVBUF, &size, &len)) {
        dbg("getsockopt: %s", strerror(errno));
        return -1;
    }
    stream->send_padding = size;

    return 0;
}

static stream_t *stream_new(session_t *session, uint32_t id, uint8_t state) {
    stream_t *stream;
    int fds[2];

    stream = malloc(sizeof(stream_t));
    if (!stream) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }

    stream->session = session;
    stream->state = state;
    stream->id = id;
    stream->readable = false;
    stream->writeable = true;
    stream->send_window = MAX_STREAM_WINDOW_SIZE;
    stream->recv_window = MAX_STREAM_WINDOW_SIZE;

    stream->recv_buf = bio_new();
    if (!stream->recv_buf) {
        dbg("bio_new");
        goto err_bio_new;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, IPPROTO_IP, fds)) {
        dbg("socketpair: %s", strerror(errno));
        goto err_socketpair;
    }

    stream->fd = fds[0];
    stream->private_fd = fds[1];

    if (stream_setup_fd(stream)) {
        dbg("stream_setup_fd");
        goto err;
    }
    session_insert_stream(session, stream);

    pthread_mutex_init(&stream->state_lock, NULL);
    pthread_mutex_init(&stream->send_lock, NULL);
    pthread_mutex_init(&stream->recv_lock, NULL);

    return stream;
err:
    close(stream->fd);
    close(stream->private_fd);
err_socketpair:
    bio_free(stream->recv_buf);
err_bio_new:
    free(stream);
    return NULL;
}

static void stream_free(stream_t *stream) {
    session_remove_stream(stream->session, stream);

    pthread_mutex_destroy(&stream->state_lock);
    pthread_mutex_destroy(&stream->send_lock);
    pthread_mutex_destroy(&stream->recv_lock);

    bio_free(stream->recv_buf);
    close(stream->fd);
    close(stream->private_fd);
    free(stream);
}

#if 0
static void stream_close_force(stream_t *stream)
{
    pthread_mutex_lock(&stream->state_lock);
    stream->state = STATE_CLOSED;
    pthread_mutex_unlock(&stream->state_lock);
    stream_notify_waiting(stream);
}
#endif

static void session_exit(session_t *session) {
    (void)session;
    /* session_close(session->fd); */
    /* TODO: */
}

static int stream_process_flags(stream_t *stream, uint16_t flags) {
    pthread_mutex_lock(&stream->state_lock);
    if (flags & FLAG_ACK) {
        if (stream->state == STATE_SYN_SENT) {
            stream->state = STATE_ESTABLISHED;
        }
        /* TODO: */
    }
    if (flags & FLAG_FIN) {
        switch (stream->state) {
        case STATE_SYN_SENT:
        case STATE_SYN_RECEIVED:
        case STATE_ESTABLISHED:
            stream->state = STATE_REMOTE_CLOSE;
            stream_notify_waiting(stream);
            break;
        case STATE_LOCAL_CLOSE:
            stream->state = STATE_CLOSED;
            stream_notify_waiting(stream);
            break;
        default:
            dbg("TODO: ");
            pthread_mutex_unlock(&stream->state_lock);
            return -1;
        }
    }
    if (flags & FLAG_RST) {
        stream->state = STATE_RESET;
        stream_notify_waiting(stream);
    }
    pthread_mutex_unlock(&stream->state_lock);
    return 0;
}

static int session_send_buf_write(session_t *session, const void *data,
                                  size_t n) {
    int r;

    pthread_mutex_lock(&session->send_lock);
    r = bio_write(session->send_buf, data, n);
    pthread_mutex_unlock(&session->send_lock);
    if (r == -1) {
        dbg("bio_write");
        return -1;
    }
    return 0;
}

static int session_go_away_for_reason(session_t *session, uint32_t reason) {
    char hdr[12];

    atomic_store(&session->local_go_away, true);
    hdr_encode(hdr, TYPE_GO_AWAY, 0, 0, reason);
    if (session_send_buf_write(session, hdr, sizeof(hdr))) {
        dbg("session_send_buf_write");
        return -1;
    }
    return 0;
}

static int notify_accept(session_t *session, uint32_t id) {
    int r;

    /* Because the communication is performed locally, there will be no
     * inconsistency between big and small ends. */
    r = write(session->private_fd, &id, sizeof(id));
    if (r == -1) {
        dbg("write: %s", strerror(errno));
        if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }
    assert(r == sizeof(id));

    return 0;
}

static uint16_t stream_send_flags(stream_t *stream) {
    uint16_t flags = 0;

    pthread_mutex_lock(&stream->state_lock);
    switch (stream->state) {
    case STATE_INIT:
        flags |= FLAG_SYN;
        stream->state = STATE_SYN_SENT;
        break;
    case STATE_SYN_RECEIVED:
        flags |= FLAG_ACK;
        stream->state = STATE_ESTABLISHED;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&stream->state_lock);
    return flags;
}

static int send_window_update(stream_t *stream) {
    char hdr[12];
    uint32_t delta, max, buf_len;
    uint16_t flags;

    flags = stream_send_flags(stream);

    pthread_mutex_lock(&stream->recv_lock);
    max = MAX_STREAM_WINDOW_SIZE;
    buf_len = bio_pending(stream->recv_buf);
    delta = (max - buf_len) - stream->recv_window;

    if (delta < (max / 2) && flags == 0) {
        /* No need to send window update */
        pthread_mutex_unlock(&stream->recv_lock);
        return 0;
    }

    stream->recv_window += delta;
    pthread_mutex_unlock(&stream->recv_lock);

    hdr_encode(hdr, TYPE_WINDOW_UPDATE, flags, stream->id, delta);
    if (session_send_buf_write(stream->session, hdr, sizeof(hdr))) {
        dbg("session_send_buf_write");
        return -1;
    }
    return 0;
}

#if 0
static int stream_send_close(stream_t *stream)
{
    char hdr[12];
    uint16_t flags;

    flags = stream_send_flags(stream);
    flags |= FLAG_FIN;
    hdr_encode(hdr, TYPE_WINDOW_UPDATE, flags, stream->id, 0);
    if (session_send_buf_write(stream->session, hdr, sizeof(hdr))) {
        dbg("session_send_buf_write");
        return -1;
    }
    return 0;
}
#endif

#if 0
void stream_close(int fd)
{
    stream_t *stream;

    stream = fd_find(fd);
    if (!stream) {
        dbg("fd_find: %s", strerror(ENOENT));
        return;
    }

    dbg("close stream: %d", stream->id);

    pthread_mutex_lock(&stream->state_lock);
    dbg("stream: %d, state: %d", stream->fd, stream->state);
    switch (stream->state) {
    case STATE_SYN_SENT:
    case STATE_SYN_RECEIVED:
    case STATE_ESTABLISHED:
        stream->state = STATE_LOCAL_CLOSE;
        break;
    case STATE_LOCAL_CLOSE:
    case STATE_REMOTE_CLOSE:
        stream->state = STATE_CLOSED;
        break;
    case STATE_CLOSED:
    case STATE_RESET:
        pthread_mutex_unlock(&stream->state_lock);
        return;
    default:
        assert(false && "unhandled state");
    }
    pthread_mutex_unlock(&stream->state_lock);

    if (stream_send_close(stream)) {
        dbg("stream_send_close");
    }
    stream_notify_waiting(stream);
    stream_free(stream);
}
#endif

int stream_read(stream_t *stream, void *buf, size_t size) {
    fd_set readfds;
    int r, pending;

again:
    pthread_mutex_lock(&stream->state_lock);
    dbg("stream %d state %d", stream->id, stream->state);
    switch (stream->state) {
    case STATE_LOCAL_CLOSE:
    case STATE_REMOTE_CLOSE:
    case STATE_CLOSED:
        /* The stream has been closed and can no longer be read. */
        pthread_mutex_lock(&stream->recv_lock);
        if (bio_pending(stream->recv_buf) == 0) {
            pthread_mutex_unlock(&stream->recv_lock);
            pthread_mutex_unlock(&stream->state_lock);
            return 0; /* EOF */
        }
        pthread_mutex_unlock(&stream->recv_lock);
        break;
    case STATE_RESET:
        /* TODO: set errno */
        pthread_mutex_unlock(&stream->state_lock);
        return -1;
    }
    pthread_mutex_unlock(&stream->state_lock);

    pthread_mutex_lock(&stream->recv_lock);
    r = bio_pending(stream->recv_buf);
    pthread_mutex_unlock(&stream->recv_lock);
    if (r == 0) {
        if (!would_block(stream->fd)) {
            /* Non-blocking mode socket, returns EAGAIN for the caller to retry
             */
            errno = EAGAIN;
            return -1;
        }
        /* Blocking mode, wait for readable events, recheck stream status */
        FD_ZERO(&readfds);
        FD_SET(stream->fd, &readfds);
        if (select(stream->fd + 1, &readfds, NULL, NULL, NULL) != 1) {
            dbg("select: %s", strerror(errno));
            return -1;
        }
        assert(FD_ISSET(stream->fd, &readfds));
        goto again;
    }

    pthread_mutex_lock(&stream->recv_lock);
    r = bio_read(stream->recv_buf, buf, size);
    assert(r > 0);
    pending = bio_pending(stream->recv_buf);
    pthread_mutex_unlock(&stream->recv_lock);

    /* No data left, setting socket unreadable */
    if (pending == 0 && stream_readable_waiting(stream)) {
        dbg("stream_readable_waiting");
        return -1;
    }

    /* After the data is read, it may be necessary to modify the readable event
     * status of the file descriptor and update the receiving window and the
     * sending window of the other party. */
    if (send_window_update(stream)) {
        dbg("send_window_update");
        /* TODO: handle error */
        return r;
    }
    return r;
}

int stream_write(stream_t *stream, const void *data, size_t n) {
    session_t *session;
    char hdr[12];
    fd_set writefds;
    uint16_t flags;
    uint32_t window, max;
    int r;

again:
    pthread_mutex_lock(&stream->state_lock);
    switch (stream->state) {
    case STATE_LOCAL_CLOSE:
    case STATE_CLOSED:
    case STATE_RESET:
        pthread_mutex_unlock(&stream->state_lock);
        return 0;
    default:
        break;
    }
    pthread_mutex_unlock(&stream->state_lock);

    pthread_mutex_lock(&stream->send_lock);
    window = stream->send_window;
    pthread_mutex_unlock(&stream->send_lock);
    if (window == 0) {
        if (!would_block(stream->fd)) {
            /* Non-blocking mode socket, returns EAGAIN for the caller to retry */
            errno = EAGAIN;
            return -1;
        }
        FD_ZERO(&writefds);
        FD_SET(stream->fd, &writefds);
        if (select(stream->fd + 1, NULL, &writefds, NULL, NULL) <= 0) {
            dbg("select: %s", strerror(errno));
            return -1;
        }
        assert(FD_ISSET(stream->fd, &writefds));
        goto again;
    }

    flags = stream_send_flags(stream);
    max = MIN(window, n);
    hdr_encode(hdr, TYPE_DATA, flags, stream->id, max);

    session = stream->session;

    r = session_send_buf_write(session, hdr, sizeof(hdr));
    if (r == -1) {
        dbg("session_send_buf_write");
        return -1;
    }
    r = session_send_buf_write(session, data, max);
    if (r == -1) {
        dbg("session_send_buf_write");
        return -1;
    }

    /* Reduce our send window */
    pthread_mutex_lock(&stream->send_lock);
    stream->send_window -= max;
    window = stream->send_window;
    pthread_mutex_unlock(&stream->send_lock);

    if (window == 0 && stream_writeable_waiting(stream)) {
        if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
            dbg("session_go_away_for_reason");
        }
    }
    return (int)max;
}

/* TODO: */
void stream_close(stream_t *stream) {
    (void)stream;
}

int session_init(session_t *session, bool client) {
    int fds[2];

    session->recv_buf = bio_new();
    if (!session->recv_buf) {
        dbg("bio_new");
        return -1;
    }

    session->send_buf = bio_new();
    if (!session->send_buf) {
        dbg("bio_new");
        goto err_new_send_buf;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, IPPROTO_IP, fds)) {
        dbg("socketpair: %s", strerror(errno));
        goto err_socketpair_fds;
    }

    atomic_init(&session->remote_go_away, false);
    atomic_init(&session->local_go_away, false);
    atomic_init(&session->next_id, client ? 1 : 2);

    RB_INIT(&session->streams);
    memset(session->hdr, 0, sizeof(session->hdr));

    session->new_frame = true;
    session->fd = fds[0];
    session->private_fd = fds[1];
    session->frame_remain = 0;

    pthread_mutex_init(&session->recv_lock, NULL);
    pthread_mutex_init(&session->send_lock, NULL);
    pthread_mutex_init(&session->streams_lock, NULL);

    return 0;

err_socketpair_fds:
    bio_free(session->send_buf);
err_new_send_buf:
    bio_free(session->recv_buf);
    return -1;
}

int session_close(session_t *session) {
    (void)session;
    /* TODO */
    return 0;
}

static int incoming_stream(session_t *session) {
    stream_t *stream;
    uint32_t id;
    char hdr[12];

    assert(session->new_frame);
    id = hdr_stream_id(session->hdr);

    /* Reject immediately if we are doing a go away */
    if (atomic_load(&session->local_go_away)) {
        hdr_encode(hdr, TYPE_WINDOW_UPDATE, FLAG_RST, id, 0);
        if (session_send_buf_write(session, hdr, sizeof(hdr))) {
            dbg("session_send_buf_write");
            return -1;
        }
        return 0;
    }

    if (session_find_stream(session, id)) {
        /* There is already a stream with the same id. There is a problem with
         * the session protocol. */
        dbg("duplicate stream declared");
        if (session_go_away_for_reason(session, GO_AWAY_PROTO_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }

    stream = stream_new(session, id, STATE_SYN_RECEIVED);
    if (!stream) {
        /* An internal error occurred, notify the peer */
        dbg("stream_new");
        if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }

    if (notify_accept(session, id)) {
        dbg("notify_accept");
        stream_free(stream);
    }
    return 0;
}

static int session_handle_data(session_t *session) {
    stream_t *stream;
    uint32_t id, len;
    char buf[4096];
    int size, r;

    if (session->new_frame && (hdr_flags(session->hdr) & FLAG_SYN)) {
        if (incoming_stream(session)) {
            dbg("incoming_stream");
            return -1;
        }
    }

    len = hdr_length(session->hdr);
    id = hdr_stream_id(session->hdr);
    stream = session_find_stream(session, id);

    if (session->new_frame) {
        if (stream) {
            if (stream_process_flags(stream, hdr_flags(session->hdr))) {
                dbg("stream_process_flags");
                if (session_go_away_for_reason(session, GO_AWAY_PROTO_ERR)) {
                    dbg("session_go_away_for_reason");
                }
                return -1;
            }
            if (len == 0) {
                /* There is no data to be processed in the current frame */
                return 0;
            }
            pthread_mutex_lock(&stream->recv_lock);
            if ((len > stream->recv_window)) {
                /* The data in the frame exceeds the data receiving window of
                 * stream */
                dbg("receive window exceeded");
                if (session_go_away_for_reason(session, GO_AWAY_PROTO_ERR)) {
                    dbg("session_go_away_for_reason");
                }
                pthread_mutex_unlock(&stream->recv_lock);
                return -1;
            }
            pthread_mutex_unlock(&stream->recv_lock);
        }
        /* Before the data in the current frame is processed, any received data
         * is not a new frame and should be returned to this function to
         * continue processing. */
        session->frame_remain = len;
        session->new_frame = false;
    }

    while (session->frame_remain) {
        if (bio_pending(session->recv_buf) == 0) {
            /* Not enough data in receive data buffer */
            break;
        }
        size = MIN(session->frame_remain, sizeof(buf));
        if (stream) {
            r = bio_peek(session->recv_buf, buf, size);
            assert(r > 0);
            pthread_mutex_lock(&stream->recv_lock);
            r = bio_write(stream->recv_buf, buf, r);
            if (r == -1) {
                dbg("bio_write");
                if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
                    dbg("session_go_away_for_reason");
                }
                pthread_mutex_unlock(&stream->recv_lock);
                return -1;
            }
            /* Decrement the receive window */
            stream->recv_window -= r;
            /* dbg("recv_window -= %d, recv_window: %d", r,
             * stream->recv_window); */
            pthread_mutex_unlock(&stream->recv_lock);
            /* Set the readable status of the socket */
            if (stream_readable_notify(stream)) {
                dbg("stream_readable_notify");
                if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
                    dbg("session_go_away_for_reason");
                }
                return -1;
            }
        } else {
            /* If there is no stream, an RST may have been sent, and the data of
             * the frame should be processed to prevent affecting other streams.
             */
            r = size;
        }
        /* dbg("id: %d, size: %d", id, r); */
        bio_consume(session->recv_buf, r);
        session->frame_remain -= r;
    }

    if (session->frame_remain == 0) {
        /* The data reception of the current frame is completed, allowing new
         * frames to be processed. */
        session->new_frame = true;
    }
    return 0;
}

static int session_handle_window_update(session_t *session) {
    stream_t *stream;
    uint32_t len;

    assert(session->new_frame);
    if ((hdr_flags(session->hdr) & FLAG_SYN)) {
        if (incoming_stream(session)) {
            dbg("incoming_stream");
            return -1;
        }
    }

    stream = session_find_stream(session, hdr_stream_id(session->hdr));
    if (!stream) {
        dbg("frame for missing stream");
        return 0;
    }

    if (stream_process_flags(stream, hdr_flags(session->hdr))) {
        dbg("stream_process_flags");
        if (session_go_away_for_reason(session, GO_AWAY_PROTO_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }

    len = hdr_length(session->hdr);
    if (len == 0) {
        return 0;
    }

    pthread_mutex_lock(&stream->send_lock);
    stream->send_window += len;
    pthread_mutex_unlock(&stream->send_lock);

    if (stream_writeable_notify(stream)) {
        if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }
    return 0;
}

static int session_handle_ping(session_t *session) {
    char hdr[12];

    assert(session->new_frame);

    if (hdr_flags(session->hdr) & FLAG_SYN) {
        hdr_encode(hdr, TYPE_PING, FLAG_ACK, 0, hdr_length(session->hdr));
        if (session_send_buf_write(session, hdr, sizeof(hdr))) {
            dbg("session_send_buf_write");
            /* An internal error occurred, notify the peer */
            if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
                dbg("session_go_away_for_reason");
            }
            return -1;
        }
        return 0;
    }

    /* TODO: Handle ping response */

    return 0;
}

static int session_handle_go_away(session_t *session) {
    uint32_t code;

    assert(session->new_frame);

    code = hdr_length(session->hdr);
    dbg("%s: code: %d", __func__, code);
    switch (code) {
    case GO_AWAY_NORMAL:
        atomic_store(&session->remote_go_away, true);
        break;
    case GO_AWAY_PROTO_ERR:
        dbg("received protocol error go away");
        return -1;
    case GO_AWAY_INTERNAL_ERR:
        dbg("received internal error go away");
        return -1;
    default:
        dbg("received unexpected go away");
        return -1;
    }
    return 0;
}

static int session_handle(session_t *session) {
    uint8_t type;

    /* Start or continue processing a request */
    type = hdr_type(session->hdr);
    switch (type) {
    case TYPE_DATA:
        return session_handle_data(session);
    case TYPE_WINDOW_UPDATE:
        return session_handle_window_update(session);
    case TYPE_PING:
        return session_handle_ping(session);
    case TYPE_GO_AWAY:
        return session_handle_go_away(session);
    default:
        dbg("unsupported message type: %d", type);
        return -1;
    }
    return 0;
}

int session_process(session_t *session) {
    int r;

    pthread_mutex_lock(&session->recv_lock);
    while (bio_pending(session->recv_buf)) {
        if (session->new_frame) {
            if (bio_pending(session->recv_buf) < (int)sizeof(session->hdr)) {
                /* There is insufficient data in the buffer. Wait for the next
                 * time new data is received before reading the hdr. */
                pthread_mutex_unlock(&session->recv_lock);
                return 0;
            }
            /* Read hdrs from new frame */
            r = bio_read(session->recv_buf, session->hdr, sizeof(session->hdr));
            assert(r == sizeof(session->hdr));
            /* Verify protocol version */
            if (hdr_version(session->hdr) != VERSION) {
                dbg("unsupported protocol version: %d",
                    hdr_version(session->hdr));
                pthread_mutex_unlock(&session->recv_lock);
                return -1;
            }
        }
        if (session_handle(session)) {
            session_exit(session);
            pthread_mutex_unlock(&session->recv_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&session->recv_lock);
    return 0;
}

int session_ping(session_t *session) {
    (void)session;
    return 0;
}

int session_go_away(session_t *session) {
    return session_go_away_for_reason(session, GO_AWAY_NORMAL);
}

int session_recv_buf_write(session_t *session, const void *data, size_t n) {
    pthread_mutex_lock(&session->recv_lock);
    if (bio_write(session->recv_buf, data, n) == -1) {
        dbg("bio_write");
        pthread_mutex_unlock(&session->recv_lock);
        return -1;
    }
    pthread_mutex_unlock(&session->recv_lock);
    return n;
}

int session_send_buf_pending(session_t *session) {
    int r;

    pthread_mutex_lock(&session->send_lock);
    r = bio_pending(session->send_buf);
    pthread_mutex_unlock(&session->send_lock);
    return r;
}

int session_send_buf_peek(session_t *session, void *buf, size_t n) {
    int r;

    pthread_mutex_lock(&session->send_lock);
    r = bio_peek(session->send_buf, buf, n);
    pthread_mutex_unlock(&session->send_lock);
    return r;
}

int session_send_buf_consume(session_t *session, size_t n) {
    int r;

    pthread_mutex_lock(&session->send_lock);
    r = bio_consume(session->send_buf, n);
    /* TODO: */
    pthread_mutex_unlock(&session->send_lock);
    return r;
}

int session_open(session_t *session, stream_t **stream) {
    atomic_uint v;
    uint32_t id;

    if (atomic_load(&session->remote_go_away)) {
        dbg("remote go away");
        return -1;
    }
    atomic_init(&v, 0);

    do {
        id = atomic_load(&session->next_id);
        if (id >= UINT32_MAX) {
            dbg("streams exhausted");
            return -1;
        }
        atomic_store(&v, id);
    } while (!atomic_compare_exchange_strong(&session->next_id, &v, id + 2));

    *stream = stream_new(session, id, STATE_INIT);
    if (!*stream) {
        dbg("stream_new");
        return -1;
    }

    if (send_window_update(*stream)) {
        dbg("send_window_update");
        stream_free(*stream);
        return -1;
    }
    return 0;
}

int session_accept(session_t *session, stream_t **stream) {
    uint32_t id;
    int r;

    /* Read the new stream id */
    r = read(session->fd, &id, sizeof(id));
    if (r == -1) {
        if (session_go_away_for_reason(session, GO_AWAY_INTERNAL_ERR)) {
            dbg("session_go_away_for_reason");
        }
        return -1;
    }
    assert(r == sizeof(id));

    *stream = session_find_stream(session, id);
    assert(*stream);

    if (send_window_update(*stream)) {
        dbg("send_window_update");
        /* TODO: process stream */
        return -1;
    }
    return 0;
}
