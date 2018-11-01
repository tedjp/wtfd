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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
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

static const char response[] = "HTTP/1.1 200 OK\r\n"
"Server: wtfd\r\n"
"Content-Type: text/plain; charset=us-ascii\r\n"
"Content-Length: 4\r\n"
"\r\n"
"wtf\n";

static void unsubscribe(int epfd, int fd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1)
        perror("epoll_ctl DEL");
}

static void unsubscribe_and_close(int epfd, int fd) {
    unsubscribe(epfd, fd);

    if (close(fd) == -1)
        perror("close");
}

static struct epoll_event *setup_event(struct epoll_event *event, int fd, uint32_t events) {
    event->events = events;
    event->data.fd = fd;

    return event;
}

static void ep_mod_or_cleanup(int epfd, int fd, struct epoll_event *event) {
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, event) == -1) {
        perror("epoll_ctl MOD EPOLLOUT");
        // This call might print spurious errors, but it's better than leaking.
        unsubscribe_and_close(epfd, fd);
    }
}

static void notify_when_readable(int epfd, int fd) {
    struct epoll_event event;
    setup_event(&event, fd, EPOLLIN | EPOLLONESHOT);
    ep_mod_or_cleanup(epfd, fd, &event);
}

static void notify_when_writable(int epfd, int fd) {
    struct epoll_event event;
    setup_event(&event, fd, EPOLLOUT | EPOLLONESHOT);
    ep_mod_or_cleanup(epfd, fd, &event);
}

static void read_client(int epfd, const struct epoll_event *event) {
    int client = event->data.fd;

    // Drain receive buffer
    char buf[4096];

    ssize_t len;
    while ((len = recv(client, buf, sizeof(buf), 0)) == sizeof(buf))
        ;

    if (len == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recv");
        unsubscribe_and_close(epfd, client);
        return;
    }

    notify_when_writable(epfd, client);
}

static void write_client(int epfd, const struct epoll_event *event) {
    int client = event->data.fd;

    const size_t resp_sz = sizeof(response) - 1;

    ssize_t sz = send(client, response, resp_sz, 0);
    if (sz == -1) {
        perror("send()");
        unsubscribe_and_close(epfd, client);
        return;
    }

    if (sz < resp_sz)
        fprintf(stderr, "short write\n");

    notify_when_readable(epfd, client);
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

    struct epoll_event revent = {
        .events = EPOLLIN,
        .data = {
            .fd = client,
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
        unsubscribe_and_close(epfd, event->data.fd);
        return;
    }

    if (event->events & EPOLLIN) {
        if (event->data.fd == listensock)
            new_client(epfd, event);
        else
            read_client(epfd, event);
    }

    if (event->events & EPOLLOUT) {
        write_client(epfd, event);
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

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in6 sin6;
    uint16_t portnum = 23206;
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

    // TODO: SO_REUSEPORT a unique socket per child
    // TODO: Enable TCP_FASTOPEN
    // TODO: Enable TCP_NODELAY (since we send the response in one go)
    // TODO: Set TCP_DEFER_ACCEPT
    sock = socket(PF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock == -1) 
        die("socket");

    reuseaddr(sock);

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(portnum);
    sin6.sin6_addr = in6addr_any;

    if (-1 == bind(sock, (struct sockaddr *)&sin6, sizeof(sin6)))
        die("bind");

    if (-1 == listen(sock, 1000))
        die("listen");

    multiply();

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
