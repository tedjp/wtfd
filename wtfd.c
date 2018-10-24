/*
 * Copyright 2007  Quest Software, Inc.
 * All rights reserved.
 * Copyright 2018  Ted Percival <ted@tedp.id.au>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Quest Software, Inc. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY QUEST SOFTWARE, INC "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL QUEST SOFTWARE, INC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE 1

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define NCPUS 8

static void die(const char *cause) __attribute__((noreturn));
void die(const char *cause) {
    perror(cause);
    exit(1);
}

static void signull(int signum) {
    /* The sound of silence. */
}

struct job {
    enum { FRONTEND_READ_WAIT, BACKEND_WRITE_WAIT, BACKEND_READ_WAIT, FRONTEND_WRITE_WAIT } state;
    int client_fd;
    int backend_fd;
    char *body;
    size_t body_len;
};

static struct job *new_job(int client) {
    struct job *j = calloc(sizeof(*j), 1);
    if (j == NULL)
        return NULL;

    j->state = FRONTEND_READ_WAIT;
    j->client_fd = client;
    j->backend_fd = -1;
    j->body = NULL;
    j->body_len = 0;

    return j;
}

// TODO: Change pool to a linked list (pull from head; MRU)
#define POOL_SIZE 100
struct connection_pool {
    int fds[POOL_SIZE];
    bool in_use[POOL_SIZE];
    struct addrinfo *addrs;
};

static void wait_for_connections_to_complete(struct connection_pool *pool) {
    int cp_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (cp_epfd == -1)
        die("epoll_create1");

    struct epoll_event event = {
        .events = EPOLLOUT,
        .data = {
            .u64 = 0,
        },
    };

    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (epoll_ctl(cp_epfd, EPOLL_CTL_ADD, pool->fds[i], &event) == -1)
            die("epoll_ctl add");
    }

    struct epoll_event events[POOL_SIZE];
    static const int CONNECT_TIMEOUT_MS = 5000;

    unsigned completed_connections = 0;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time); // FIXME: check return

    for (;;) {
        // FIXME: Need to reduce the timeout by the elapsed time.
        ssize_t count = epoll_wait(cp_epfd, events, sizeof(events) / sizeof(events[0]), CONNECT_TIMEOUT_MS);

        // FIXME: iterate over each to check whether they are good or error
        completed_connections += count;

        if (completed_connections >= sizeof(pool->fds) / sizeof(pool->fds[0]))
            break; // all good

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now); // FIXME: Check return
        if (now.tv_sec - 4 > start_time.tv_sec) {
            fprintf(stderr, "backend connections took too long\n");
            exit(1);
        }
    }

    fprintf(stderr, "Backend connection pool ready\n");

    close(cp_epfd);
}

static struct connection_pool backend_pool;

static struct connection_pool setup_connection_pool() {
    const struct addrinfo hints = {
        .ai_flags = AI_V4MAPPED | AI_ADDRCONFIG,
        .ai_family = AF_INET6,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = 0,
    };

    struct addrinfo *addrs = NULL;

    int gaierr = getaddrinfo("d-gp2-neildev-1.imovetv.com", "http", &hints, &addrs);
    if (gaierr) {
        fprintf(stderr, "Failed to resolve backend host: %s\n", gai_strerror(gaierr));
        exit(1);
    }

    if (addrs == NULL) {
        fprintf(stderr, "No addresses\n");
        exit(1);
    }

    struct connection_pool pool;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        pool.fds[i] = -1;
        pool.in_use[i] = false;
    }

    for (struct addrinfo *addr = addrs; addr != NULL; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, addr->ai_protocol);
        if (fd == -1)
            die("connection pool socket");

        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1 && errno != EINPROGRESS) {
            fprintf(stderr, "Failed to connect to backend address (not fatal): %s\n", strerror(errno));
            close(fd);
            continue;
        }

        // Connect probably worked; repeat!
        pool.fds[0] = fd;

        for (size_t i = 1; i < POOL_SIZE; ++i) {
            // FIXME: copypasta of above
            if ((pool.fds[i] = socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, addr->ai_protocol)) == -1) {
                die("pool socket");
            }

            if (connect(pool.fds[i], addr->ai_addr, addr->ai_addrlen) == -1 && errno != EINPROGRESS) {
                die("pool connect failed");
            }
        }

        wait_for_connections_to_complete(&pool);

        break;
    }

    pool.addrs = addrs;
    // not freeing addrs to allow for connection reopening

    return pool;
}

static void pool_replace_socket(struct connection_pool *pool, int oldfd, int newfd) {
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (pool->fds[i] == oldfd) {
            pool->fds[i] = newfd;
            close(oldfd);
            return;
        }
    }

    fprintf(stderr, "Tried to replace unknown fd\n");
}

static int pool_get_fd(struct connection_pool *pool) {
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (pool->fds[i] != -1 && pool->in_use[i] == false) {
            pool->in_use[i] = true;
            return pool->fds[i];
        }
    }

    return -1; // no FDs available
}

static void pool_close_and_remove_fd(struct connection_pool *pool, int fd) {
    // TODO: Reconnect. (Easier when moved to linked list)
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (pool->fds[i] == fd) {
            assert(pool->in_use[i] == true);
            close(fd);
            // just blacklist it for now
            pool->fds[i] = -1;
            pool->in_use[i] = false;
            return;
        }
    }

    fprintf(stderr, "Tried to remove unknown fd\n");
}

static void pool_release_fd(struct connection_pool *pool, int fd) {
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (pool->fds[i] == fd) {
            assert(pool->in_use[i] == true);
            pool->in_use[i] = false;
            return;
        }
    }

    fprintf(stderr, "fd not found\n");
    exit(3);
}

static bool send_backend_get(int fd) {
    const char req[] = "GET /test.json HTTP/1.1\n"
        "Host: d-gp2-neildev-1.imovetv.com\n"
        "\n";

    ssize_t len = send(fd, req, sizeof(req) - 1, MSG_DONTWAIT);
    if (len == -1) {
        perror("backend send");

        // TODO: mark fd bad / try to reconnect

        return false;
    }

    if (len < sizeof(req) - 1) {
        fprintf(stderr, "short write on backend\n");
        // TODO: retry / mark bad
        return false;
    }

    return true;
}

static ssize_t get_backend_response(int fd, char *buf, size_t buflen) {
    ssize_t len = recv(fd, buf, buflen, 0);
    if (len == -1) {
        perror("backend recv");
        return -1;
    }

    if (len == 0) {
        fprintf(stderr, "backend connection closed :(\n");
        return -1;
    }

    // Just use everything that arrived after \r\n\r\n.
    // Obviously this doesn't handle chunked encoding.
    char *start = memmem(buf, len, "\r\n\r\n", 4);
    if (start == NULL)
        return -1;

    start += 4;

    ssize_t keep_len = len - (start - buf);

    memmove(buf, start, keep_len);

    return keep_len;
}

// Returns new length, or -1 on error
static ssize_t inject_string(char *buf, size_t buflen, size_t bufcap, const char *from, size_t fromlen, const char *to, size_t tolen) {
    if (buflen - fromlen + tolen > bufcap)
        return false;

    char *location = memmem(buf, buflen, from, fromlen);

    const size_t trailer_len = buflen - (location - buf) - fromlen;

    // shift keep-memory
    memmove(location + tolen, location + fromlen, trailer_len);

    // replace
    memcpy(location, to, tolen);

    return buflen - fromlen + tolen;
}

static void unsubscribe(int epfd, int fd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1)
        perror("epoll_ctl DEL");
}

static void unsubscribe_and_close(int epfd, int fd) {
    unsubscribe(epfd, fd);

    if (close(fd) == -1)
        perror("close");
}

static void close_job(int epfd, struct job *job) {
    if (job->client_fd != -1)
        unsubscribe_and_close(epfd, job->client_fd);

    if (job->backend_fd != -1) {
        unsubscribe(epfd, job->backend_fd);
        pool_release_fd(&backend_pool, job->backend_fd);
    }

    free(job);
}

static void ep_mod_or_cleanup(int epfd, int fd, struct epoll_event *event) {
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, event) == -1) {
        perror("epoll_ctl MOD EPOLLOUT");
        // This call might print spurious errors, but it's better than leaking.
        close_job(epfd, (struct job*)event->data.ptr);
    }
}

static void setup_event(struct epoll_event *event, struct job *job, uint32_t events) {
    event->events = events;
    event->data.ptr = job;
}

static void notify_when_readable(int epfd, struct job *job, int fd) {
    struct epoll_event event;
    setup_event(&event, job, EPOLLIN | EPOLLONESHOT | EPOLLET);
    ep_mod_or_cleanup(epfd, fd, &event);
}

static void notify_when_writable(int epfd, struct job *job, int fd) {
    struct epoll_event event;
    setup_event(&event, job, EPOLLOUT | EPOLLONESHOT);
    ep_mod_or_cleanup(epfd, fd, &event);
}

static void state_to_client_read(int epfd, struct job *job) {
    //fprintf(stderr, "New state: client read\n");
    job->state = FRONTEND_READ_WAIT;
    notify_when_readable(epfd, job, job->client_fd);
}

static void state_to_backend_write(int epfd, struct job *job) {
    //fprintf(stderr, "New state: backend write\n");
    job->state = BACKEND_WRITE_WAIT;
    notify_when_writable(epfd, job, job->backend_fd);
}

static void state_to_backend_read(int epfd, struct job *job) {
    //fprintf(stderr, "New state: backend read\n");
    job->state = BACKEND_READ_WAIT;
    notify_when_readable(epfd, job, job->backend_fd);
}

static void state_to_client_write(int epfd, struct job *job) {
    //fprintf(stderr, "New state: client write\n");
    job->state = FRONTEND_WRITE_WAIT;
    notify_when_writable(epfd, job, job->client_fd);
}

static void read_client_request(int epfd, struct job *job) {
    int client = job->client_fd;

    // Drain receive buffer
    char buf[4096];

    ssize_t len = recv(client, buf, sizeof(buf), 0);
    if (len == sizeof(buf)) {
        // TODO: Append incoming request if it's larger than 4 kiB.
        fprintf(stderr, "Client request too big");
        unsubscribe_and_close(epfd, client);
        return;
    }

    if (len == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recv");
        unsubscribe_and_close(epfd, client);
        return;
    }

    if (len == 0) {
        unsubscribe_and_close(epfd, client);
        return;
    }

    if (len > 0 && memmem(buf, len, "\r\n\r\n", 4) == NULL) {
        // FIXME: If the end of the request spans the edge of the buffer we
        // won't catch it.
        state_to_client_read(epfd, job);
        return;
    }

    // Grab a backend connection
    job->backend_fd = pool_get_fd(&backend_pool);
    if (job->backend_fd == -1) {
        // FIXME: Add a new BACKEND_POOL_WAIT state and push this job onto the wait
        // queue
        fprintf(stderr, "Failed to get a backend connection; probably oversubscribed\n");

        // TODO: Care about errors
        // (this could drop data if the buffer is full but NBD)
        const char msg[] = "HTTP/1.1 503 Server Overloaded - no backend connections available";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        state_to_client_read(epfd, job);
        return;
    }

    state_to_backend_write(epfd, job);
}

static void add_backend_fd_to_epoll(int epfd, int fd) {
    struct epoll_event event = {
        .events = 0,
        .data = {
            .u64 = 0,
        },
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) == -1)
        perror("epoll_ctl _add_backend_host");
}

static void read_backend_response(int epfd, struct job *job) {
    char resp[4096];

    ssize_t len = get_backend_response(job->backend_fd, resp, sizeof(resp));

    if (len == -1) {
        fprintf(stderr, "Backend was trash; replacing\n");
        unsubscribe(epfd, job->backend_fd);
        int oldfd = job->backend_fd;
        int newfd = socket(
                backend_pool.addrs->ai_family,
                backend_pool.addrs->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                backend_pool.addrs->ai_protocol);
        // FIXME: check error
        connect(newfd, backend_pool.addrs->ai_addr, backend_pool.addrs->ai_addrlen);
        // FIXME: check error
        pool_replace_socket(&backend_pool, oldfd, newfd);
        add_backend_fd_to_epoll(epfd, newfd);
        job->backend_fd = newfd;
        // resend backend request
        state_to_backend_write(epfd, job);
        return;
    }

    // TODO: If the response contained "Connection: close", close it and
    // prepare a replacement connection, but don't throw away the response
    // that we got.

    // Finished with the backend fd.
    pool_release_fd(&backend_pool, job->backend_fd);
    job->backend_fd = -1;

    if (len == -1) {
        char msg[] = "HTTP/1.1 503 Backend request failed\n";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        state_to_client_read(epfd, job);
        return;
    }

    const char from[] = "http://watch.sling.com";
    const size_t fromlen = sizeof(from) - 1;
    const char to[] = "http://hahaha.com";
    const size_t tolen = sizeof(to) - 1;

    ssize_t new_size = inject_string(resp, len, sizeof(resp), from, fromlen, to, tolen);

    if (new_size == -1) {
        char msg[] = "HTTP/1.1 500 replacement failed\r\n\r\n";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        state_to_client_read(epfd, job);
        return;
    }

    job->body = malloc(new_size);
    if (job->body == NULL) {
        char msg[] = "HTTP/1.1 503 OOM\r\n\r\n";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        state_to_client_read(epfd, job);
        return;
    }

    memcpy(job->body, resp, new_size);
    job->body_len = new_size;

    state_to_client_write(epfd, job);
}

static ssize_t get_header(char *buf, size_t buflen, size_t content_length) {
    int len = snprintf(buf, buflen,
            "HTTP/1.1 200 OK\r\n"
            "Server: Proxymoron\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", content_length);

    if (len == buflen)
        return -1; // truncated string

    return len;
}

static void write_client_response(int epfd, struct job *job) {
    // XXX: If NODELAY is enabled, it'd be worth setting TCP_CORK until the
    // entire response is written.
    char buf[4096];
    ssize_t len = get_header(buf, sizeof(buf), job->body_len);
    if (!len) {
        char msg[] = "HTTP/1.1 500 Failed to render header\r\n\r\n";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        state_to_client_read(epfd, job);
        return;
    }

    if (send(job->client_fd, buf, len, MSG_DONTWAIT) != len)
        perror("send header");

    if (send(job->client_fd, job->body, job->body_len, MSG_DONTWAIT) != job->body_len) {
        perror("send to client");
        // FIXME: If the response was big, it might take multiple sends.
    }

    free(job->body);
    job->body = NULL;
    job->body_len = 0;

    state_to_client_read(epfd, job);
}

static void close_client(int epfd, struct job *job) {
    unsubscribe_and_close(epfd, job->client_fd);
    if (job->backend_fd != -1) {
        // Kill the backend connection, it's the only way to cancel the
        // request in HTTP/1.1. Waiting for it and draining it is a waste
        // of time (potentially bigger).
        unsubscribe(epfd, job->backend_fd);
        pool_close_and_remove_fd(&backend_pool, job->backend_fd);
        job->backend_fd = -1;
    }
}

static void drain_maybe_close(int fd, int epfd, struct job *job) {
    char buf[4096];
    ssize_t len;
    while ((len = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
        ;

    if (len == 0) {
        close_client(epfd, job);
    }
}

static void read_event(int epfd, const struct epoll_event *event) {
    struct job *job = event->data.ptr;

    switch (job->state) {
    case FRONTEND_READ_WAIT:
        read_client_request(epfd, job);
        break;
    case BACKEND_READ_WAIT:
        read_backend_response(epfd, job);
        break;
    default:
        fprintf(stderr, "incoming data available but none expected, client fd %d, backend fd %d\n", job->client_fd, job->backend_fd);
        drain_maybe_close(job->client_fd, epfd, job);
#if 0 // TODO: If it's the backend socket, kill it off.
        if (job->backend_fd != -1)
            drain(job->backend_fd);
#endif
    }
}

static void write_backend(int epfd, struct job *job) {
    bool ok = send_backend_get(job->backend_fd);

    if (!ok) {
        char msg[] = "HTTP/1.1 500 Backend unavailable\r\n\r\n";
        send(job->client_fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);

        state_to_client_read(epfd, job);
    }

    state_to_backend_read(epfd, job);
}

static void write_event(int epfd, const struct epoll_event *event) {
    struct job *job = event->data.ptr;

    if (job->state == FRONTEND_WRITE_WAIT) {
        write_client_response(epfd, job);
    } else if (job->state == BACKEND_WRITE_WAIT) {
        write_backend(epfd, job);
    }
}

static void new_client(int epfd, const struct epoll_event *event) {
    int client = accept4(event->data.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // other process probably got it.
            return;
        }

        perror("accept4");
        return;
    }

    struct job *job = new_job(client);
    if (job == NULL) {
        perror("failed to allocate job");
        char msg[] = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
        send(client, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        close(client);
        return;
    }

    // XXX: We could do an immediate read here to take full advantage of TCP_FASTOPEN's payload

    struct epoll_event revent = {
        .events = EPOLLIN,
        .data = {
            .ptr = job,
        },
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client, &revent) == -1) {
        perror("epoll_ctl ADD");
        if (close(client) == -1)
            perror("close");
    }
}

static void dispatch(int epfd, const struct epoll_event *event, int listensock) {
    if (event->events & EPOLLHUP) {
        close_client(epfd, event->data.ptr);
        return;
    }

    if (event->events & EPOLLIN) {
        if (event->data.fd == listensock)
            new_client(epfd, event);
        else
            read_event(epfd, event);
    }

    if (event->events & EPOLLOUT) {
        write_event(epfd, event);
    }
}

static void multiply() {
    // parent keeps running too
    for (unsigned i = 1; i < NCPUS; ++i) {
        pid_t pid = fork();
        if (pid == -1)
            perror("fork");
        if (pid == 0)
            return;
    }
}

static void reuseaddr(int sock) {
    const int yes = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
        perror("setsockopt SO_REUSEADDR");
}

__attribute__((unused))
static void enable_defer_accept(int sock) {
#if defined (TCP_DEFER_ACCEPT)
    const int seconds = 2;
    if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, &seconds, sizeof(seconds)) == -1)
        perror("setsockopt TCP_DEFER_ACCEPT");
    else
        fprintf(stderr, "TCP_DEFER_ACCEPT enabled\n");
#endif
}

static void enable_fastopen(int sock) {
#if defined(TCP_FASTOPEN)
    const int queue_len = 100;
    if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN, &queue_len, sizeof(queue_len)) == -1)
        perror("setsockopt(TCP_FASTOPEN)");
#if 0
    else
        fprintf(stderr, "TCP_FASTOPEN enabled\n");
#endif
#endif
}

__attribute__((unused))
static void enable_nodelay(int sock) {
    const int yes = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1)
        perror("setsockopt(TCP_NODELAY)");
    else
        fprintf(stderr, "TCP_NODELAY enabled\n");
}

static void reuseport(int sock) {
    const int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) == -1)
        perror("setsockopt(SO_REUSEPORT)");
}

static void add_backend_fds_to_epoll(struct connection_pool *pool, int epfd) {
    for (size_t i = 0; i < sizeof(pool->fds) / sizeof(pool->fds[0]); ++i) {
        add_backend_fd_to_epoll(epfd, pool->fds[i]);
    }
}

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in6 sin6;
    const uint16_t portnum = 8080;
    struct sigaction sa;

    if (-1 == sigemptyset(&sa.sa_mask))
        die("sigemptyset");

    sa.sa_handler = signull;
    sa.sa_flags = 0;
    sa.sa_restorer = NULL;

    if (-1 == sigaction(SIGINT, &sa, NULL))
        die("sigaction");

    if (-1 == sigaction(SIGTERM, &sa, NULL))
        die("sigaction");

    if (-1 == sigaction(SIGQUIT, &sa, NULL))
        die("sigaction");

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal(SIGPIPE)");

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(portnum);
    sin6.sin6_addr = in6addr_any;

    multiply();

    sock = socket(PF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock == -1)
        die("socket");

    reuseaddr(sock);
    reuseport(sock);
    // This is slightly slower in this case, but probably want to enable it on a
    // real server.
    //enable_defer_accept(sock);
    // wrk doesn't seem to use fastopen, but support it anyway.
    enable_fastopen(sock);
    // nodelay reduces throughput too :(
    //enable_nodelay(sock);

    if (-1 == bind(sock, (struct sockaddr *)&sin6, sizeof(sin6)))
        die("bind");

    if (-1 == listen(sock, 1000))
        die("listen");

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
        die("epoll_create");

    struct epoll_event event = {
        .events = EPOLLIN,
        .data = {
            .fd = sock,
        },
    };

#if defined(EPOLLEXCLUSIVE)
    event.events |= EPOLLEXCLUSIVE;
#endif

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &event) == -1)
        die("epoll_ctl add");

    backend_pool = setup_connection_pool();

    add_backend_fds_to_epoll(&backend_pool, epfd);

    for (;;) {
        struct epoll_event events[10];
        int count = epoll_wait(epfd, events, sizeof(events) / sizeof(events[0]), -1);
        if (count == -1)
            die("epoll_wait");

        if (count == 0)
            continue;

        for (unsigned i = 0; i < count; ++i)
            dispatch(epfd, &events[i], sock);
    }

    close(sock);

    return 0;
}

/* vim: ts=4 sw=4 et
 */
