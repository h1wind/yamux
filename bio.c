/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * bio.c
 */

#include "bio.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "misc.h"
#include "queue.h"

struct buffer {
    // clang-format off
    STAILQ_ENTRY(buffer) entry;
    // clang-format on
    size_t len;
    char *data;
};

STAILQ_HEAD(buffer_list, buffer);

struct bio {
    struct buffer_list buffers;
    size_t offset;
    size_t len;
};

static struct buffer *buffer_new(const char *data, size_t len) {
    struct buffer *buffer;

    buffer = malloc(sizeof(*buffer));
    if (!buffer) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }
    buffer->data = malloc(len);
    if (!buffer->data) {
        dbg("malloc: %s", strerror(errno));
        free(buffer);
        return NULL;
    }
    memcpy(buffer->data, data, len);
    buffer->len = len;
    return buffer;
}

static void buffer_free(struct buffer *buffer) {
    free(buffer->data);
    free(buffer);
}

bio_t *bio_new(void) {
    bio_t *bio;

    bio = malloc(sizeof(*bio));
    if (!bio) {
        dbg("malloc: %s", strerror(errno));
        return NULL;
    }
    STAILQ_INIT(&bio->buffers);
    bio->offset = 0;
    bio->len = 0;
    return bio;
}

void bio_free(bio_t *bio) {
    bio_reset(bio);
    free(bio);
}

void bio_reset(bio_t *bio) {
    struct buffer *buffer;

    /* Clear all data in the linked list */
    while (!STAILQ_EMPTY(&bio->buffers)) {
        buffer = STAILQ_FIRST(&bio->buffers);
        STAILQ_REMOVE_HEAD(&bio->buffers, entry);
        buffer_free(buffer);
    }
    bio->offset = 0;
    bio->len = 0;
}

int bio_write(bio_t *bio, const void *buf, size_t n) {
    struct buffer *buffer;
    size_t pos, size;

    pos = 0;
    while (pos < n) {
        /* 4096 is the maximum capacity of each buffer */
        size = MIN(4096, n - pos);
        buffer = buffer_new(((char *)buf) + pos, size);
        if (!buffer) {
            dbg("buffer_new");
            return -1;
        }
        STAILQ_INSERT_TAIL(&bio->buffers, buffer, entry);
        bio->len += size;
        pos += size;
    }
    return n;
}

int bio_pending(bio_t *bio) {
    return bio->len;
}

int bio_read(bio_t *bio, void *buf, size_t n) {
    struct buffer *buffer;
    size_t nread = 0, size;

    while (!STAILQ_EMPTY(&bio->buffers) && nread < n) {
        buffer = STAILQ_FIRST(&bio->buffers);
        /* Number of bytes remaining to be read */
        size = n - nread;
        if ((buffer->len - bio->offset) < size) {
            /* Not enough data in buffer */
            size = buffer->len - bio->offset;
        }
        if (buf) {
            memcpy((char *)buf + nread, buffer->data + bio->offset, size);
        }
        bio->offset += size;
        nread += size;
        if (bio->offset == buffer->len) {
            STAILQ_REMOVE_HEAD(&bio->buffers, entry);
            buffer_free(buffer);
            bio->offset = 0;
        }
    }
    bio->len -= nread;
    return nread;
}

int bio_peek(bio_t *bio, void *buf, size_t n) {
    size_t nread = 0, size, offset;
    struct buffer *buffer;

    offset = bio->offset;

    STAILQ_FOREACH(buffer, &bio->buffers, entry) {
        /* Number of bytes remaining to be read */
        size = n - nread;
        if (buffer->len - offset < size) {
            /* Not enough data in buffer */
            size = buffer->len - offset;
        }
        memcpy((char *)buf + nread, buffer->data + offset, size);
        nread += size;
        if (offset) {
            offset = 0;
        }
    }
    return nread;
}

int bio_consume(bio_t *bio, size_t n) {
    return bio_read(bio, NULL, n);
}
