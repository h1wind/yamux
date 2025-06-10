/*
 * Copyright (c) 2025 hi <hi@nosec.me>
 * main.c
 */

#define _GNU_SOURCE

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

#include "debug.h"
#include "bio.h"
#include "mux.h"

static session_t session;
// static int session = -1;
static int fd = -1;

static void *recv_loop(void *arg) {
    char buf[4096];
    int r;

    pthread_detach(pthread_self());
    (void)arg;

    while (true) {
        r = read(fd, buf, sizeof(buf));
        if (r == -1) {
            dbg("read: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            dbg("eof");
            break;
        }
        r = session_recv_buf_write(&session, buf, r);
        if (r == -1) {
            dbg("session_recv_buf_write");
            break;
        }
        if (session_process(&session)) {
            dbg("session_process");
            break;
        }
    }
    return NULL;
}

static void *send_loop(void *arg) {
    char buf[4096];
    int r;

    pthread_detach(pthread_self());
    (void)arg;

    while (true) {
        if (session_send_buf_pending(&session) == 0) {
            /* sleep(1); */
            usleep(100);
            continue;
        }
        r = session_send_buf_peek(&session, buf, sizeof(buf));
        assert(r > 0);
        r = write(fd, buf, r);
        if (r == -1) {
            dbg("write: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            dbg("eof");
            break;
        }
        session_send_buf_consume(&session, r);
    }
    return NULL;
}

static int tcp_listen(void) {
    struct sockaddr_in si;
    int fd, on;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd == -1) {
        dbg("socket: %s", strerror(errno));
        return -1;
    }

    on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) {
        dbg("setsocopt: %s", strerror(errno));
        goto err;
    }

    memset(&si, 0, sizeof(si));
    si.sin_addr.s_addr = INADDR_ANY;
    si.sin_port = htons(2323);
    si.sin_family = AF_INET;

    if (bind(fd, (struct sockaddr *)&si, sizeof(si))) {
        dbg("bind: %s", strerror(errno));
        goto err;
    }

    if (listen(fd, SOMAXCONN)) {
        dbg("listen: %s", strerror(errno));
        goto err;
    }

    return fd;
err:
    close(fd);
    return -1;
}

void *handle(void *arg) {
    stream_t *stream = arg;

    char buf[256];
    int r;

    while (true) {
        r = stream_read(stream, buf, sizeof(buf) - 1);
        if (r <= 0) {
            if (r == 0) {
                dbg("EOF");
                return NULL;
            }
            dbg("stream_read fail");
            return NULL;
        }
        buf[r] = 0;
        dbg("stream %d, %s", stream->id, buf);
        stream_write(stream, "helloworld", 10);
        // stream_close(fd);
        // return NULL;
    }
    return NULL;
}

int test_mux(void) {
    pthread_t thrid;
    int lfd;
    /* int r; */
    /* char buf[256]; */

    lfd = tcp_listen();
    if (lfd == -1) {
        dbg("tcp_listen");
        return -1;
    }

    fd = accept(lfd, NULL, NULL);
    if (fd == -1) {
        dbg("accept: %s", strerror(errno));
        close(lfd);
        return -1;
    }
    close(lfd);

    // session = session_server();
    session_server_init(&session);
    dbg("session: %d", session.fd);

    pthread_create(&thrid, NULL, recv_loop, NULL);
    pthread_create(&thrid, NULL, send_loop, NULL);

    /* stream = session_open(session);
    dbg("stream: %d", stream); */
    /* s2 = session_open(session); */

    /* sleep(1); */

    /* char buf[256];
    int n; */

    while (true) {
        stream_t *stream;
        if (session_accept(&session, &stream)) {
            dbg("_session_accept");
            break;
        }
        dbg("recv new stream: %d", stream->id);
        pthread_create(&thrid, NULL, handle, stream);
        // int *arg = malloc(sizeof(int));
        // assert(arg);
        // *arg = session_accept(session);
        // dbg("recv new stream: %d", *arg);
        // pthread_create(&thrid, NULL, handle, arg);
        /* stream_write(stream, "how are you?", 13); */
        /* n = stream_read(stream, buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        buf[n] = '\0';
        dbg("%s", buf); */
        /* usleep(10000); */
    }

    /* sleep(1); */

    /* r = stream_write(stream, "helloworld", 11);
    dbg("r: %d", r); */

    /* stream_close(stream); */

    // mux_test();

    /* stream = session_accept(session);
    dbg("stream: %d", stream); */

    /* r = stream_read(stream, buf, sizeof(buf));
    if (r == -1) {
        dbg("stream_read: %s", strerror(errno));
        exit(-1);
    }
    dbg("stream_read, stream: %d, r: %d", stream, r); */

    /* stream_close(stream); */

    /* stream_close(s1); */
    /* stream_close(s2); */
    /* session_close(session); */
    /* close(fd); */

    return 0;
}

void test_pipe(void) {
    char buf[1024];
    socklen_t len;
    int var, total, r;
    int fds[2];

    pipe(fds);

    len = sizeof(var);
    getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &var, &len);
    dbg("fds[0] sndbuf size: %d", var);
    getsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &var, &len);
    dbg("fds[0] rcvbuf size: %d", var);

    getsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &var, &len);
    dbg("fds[1] sndbuf size: %d", var);
    getsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &var, &len);
    dbg("fds[1] rcvbuf size: %d", var);

    while (true) {
        break;
        r = write(fds[1], buf, sizeof(buf));
        if (r <= 0) {
            dbg("write: %s", strerror(errno));
            return;
        }
        total += r;
        dbg("total: %d", total);
    }
}

void test_socketpair(void) {
    int fds[2];
    int size, r /* , total */;
    char buf[4096];
    fd_set rset, wset;
    socklen_t len;

    r = socketpair(AF_UNIX, SOCK_DGRAM, IPPROTO_IP, fds);
    if (r == -1) {
        dbg("socketpair: %s", strerror(errno));
        return;
    }
    /* setnonblocking(fds[1]); */

    len = sizeof(size);

    getsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &size, &len);
    getsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &size, &len);
    getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, &len);
    getsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &size, &len);

    len = sizeof(size);
    size = 256;

    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &size, len);
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, len);
    setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &size, len);
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &size, len);

    getsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &size, &len);
    getsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &size, &len);
    getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &size, &len);
    getsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &size, &len);
    dbg("rcvbuf: %d", size);

    dbg("write padding, size: %d", size);
    r = write(fds[0], buf, size);
    if (r == -1) {
        dbg("write: %s", strerror(errno));
    }
    dbg("write r: %d", r);

    read(fds[1], buf, 1);

    write(fds[0], buf, size);

    dbg("start read");
    while (true) {
        r = read(fds[1], buf, sizeof(buf));
        dbg("size: %d", r);
        if (r != sizeof(buf)) {
            break;
        }
    }
    dbg("end read");

    FD_ZERO(&rset);
    FD_ZERO(&wset);

    FD_SET(fds[0], &rset);
    FD_SET(fds[0], &wset);

    dbg("start select");
    r = select(fds[0] + 1, &rset, &wset, NULL, NULL);
    if (r == -1) {
        dbg("select: %s", strerror(errno));
        return;
    }

    if (FD_ISSET(fds[0], &rset)) {
        dbg("read event");
    }
    if (FD_ISSET(fds[0], &wset)) {
        dbg("write event");
    }
}

void test_event(void) {
    fd_set rset, wset;
    int fd, r /* , size */;
    /* socklen_t len; */

    fd = open("/dev/zero", O_RDONLY);
    dbg("fd: %d", fd);
    dbg("flags: %d", fcntl(fd, F_GETFL));

    /* len = sizeof(size);

    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, &len);
    dbg("fd sndbuf size: %d", size);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, &len);
    dbg("fd rcvbuf size: %d", size);

    getsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &size, &len);
    dbg("rcvlowat: %d", size);
    getsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &size, &len);
    dbg("sndlowat: %d", size); */

    dbg("seek: %ld", lseek(fd, 0, SEEK_END));

    FD_ZERO(&rset);
    FD_ZERO(&wset);

    FD_SET(fd, &rset);
    FD_SET(fd, &wset);

    r = select(fd + 1, &rset, &wset, NULL, NULL);
    if (r == -1) {
        dbg("select: %s", strerror(errno));
        return;
    }

    if (FD_ISSET(fd, &rset)) {
        dbg("read event");
    }
    if (FD_ISSET(fd, &wset)) {
        dbg("write event");
    }
}

int main(void) {
    test_mux();
    /* test_socketpair(); */
    return 0;
}
