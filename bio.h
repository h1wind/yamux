/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * bio.h
 */

#ifndef YAMUX_BIO_H
#define YAMUX_BIO_H

#include <stddef.h>

typedef struct bio bio_t;

bio_t *bio_new(void);
void bio_free(bio_t *bio);

/**
 * @brief reset buffer
 *
 * @param bio
 */
void bio_reset(bio_t *bio);

/**
 * @brief Read n bytes of data from the buffer into buf. When buf is NULL,
 *        n bytes of data will be consumed from the buffer. In this case,
 *        it is recommended to use the bio_consume function.
 *
 * @param bio
 * @param buf
 * @param n
 * @return int Returns the actual number of bytes read, which is always greater
 *             than or equal to zero.
 */
int bio_read(bio_t *bio, void *buf, size_t n);

/**
 * @brief Append n bytes of data in buf to the buffer
 *
 * @param bio
 * @param buf
 * @param n
 * @return int Returns the actual number of bytes appended on success, -1 on
 *             failure.
 */
int bio_write(bio_t *bio, const void *buf, size_t n);

/**
 * @brief Get the number of bytes available in the buffer
 *
 * @param bio
 * @return int Returns the actual number of bytes available. The return value is
 *             always greater than or equal to zero.
 */
int bio_pending(bio_t *bio);

/**
 * @brief Read n bytes of data from the buffer into buf without consuming the
 *        data in the buffer
 *
 * @param bio
 * @param buf
 * @param n
 * @return int Returns the number of bytes successfully read. The return value
 *             is always greater than or equal to zero.
 */
int bio_peek(bio_t *bio, void *buf, size_t n);

/**
 * @brief Consume n bytes from buffer
 *
 * @param bio
 * @param n
 * @return int Returns the number of bytes consumed successfully. The return
 *             value is always greater than or equal to zero.
 */
int bio_consume(bio_t *bio, size_t n);

#endif /* YAMUX_BIO_H */
