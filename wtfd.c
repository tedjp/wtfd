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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>

static void die(const char *cause) __attribute__((noreturn));
void die(const char *cause) {
    perror(cause);
    exit(1);
}

static void signull(int signum) {
    /* The sound of silence. */
}

static void serve_client(int epfd, const struct epoll_event *event) {
    int client = event->data.fd;

    const char msg[] = "wtf\n";
    ssize_t sz = send(client, msg, sizeof(msg) - 1, 0);
    if (sz == -1)
        perror("send()");
    else if (sz < sizeof(msg) - 1)
        fprintf(stderr, "short write\n");

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, client, NULL) == -1)
        perror("epoll_ctl DEL");

    if (close(client) == -1)
        perror("close");
}

static void new_client(int epfd, const struct epoll_event *event) {
    int client = accept4(event->data.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client == -1) {
        perror("accept4");
        return;
    }

    struct epoll_event wrevent = {
        .events = EPOLLOUT,
        .data = {
            .fd = client,
        },
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client, &wrevent) == -1) {
        perror("epoll_ctl ADD");
        if (close(client) == -1)
            perror("close");
    }
}
static void dispatch(int epfd, const struct epoll_event *event) {
    if (event->events & EPOLLIN) {
        new_client(epfd, event);
    } else if (event->events & EPOLLOUT) {
        serve_client(epfd, event);
    }
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

    sock = socket(PF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock == -1) 
        die("socket");

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(portnum);
    sin6.sin6_addr = in6addr_any;

    if (-1 == bind(sock, (struct sockaddr *)&sin6, sizeof(sin6)))
        die("bind");

    if (-1 == listen(sock, 1))
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

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &event) == -1)
        die("epoll_ctl add");

    for (;;) {
        struct epoll_event events[2];
        int count = epoll_wait(epfd, events, sizeof(events) / sizeof(events[0]), -1);
        if (count == -1)
            die("epoll_wait");

        if (count == 0)
            continue;

        for (unsigned i = 0; i < count; ++i)
            dispatch(epfd, &events[i]);
    }

    close(sock);

    return 0;
}

/* vim: ts=4 sw=4 et
 */
