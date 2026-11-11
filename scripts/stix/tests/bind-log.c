/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * LD_PRELOAD bind(2) logger for test-qtest-ports.sh: a stand-in for
 * `strace -f -e trace=bind` on hosts without strace. Every bind() of an
 * AF_INET/AF_INET6 socket calls the real bind() and then appends
 * "<pid> <tcp|udp|other> <port> <ok|errno name>" to the file named by
 * BIND_LOG_FILE. LD_PRELOAD is inherited, so the QEMU processes the
 * qtest spawns are logged too.
 *
 * Build: cc -shared -fPIC -o bind-log.so bind-log.c -ldl
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real_bind)(int, const struct sockaddr *, socklen_t);
    const char *path = getenv("BIND_LOG_FILE");
    int port = -1;
    int ret, err;

    if (!real_bind) {
        real_bind = (int (*)(int, const struct sockaddr *, socklen_t))
            dlsym(RTLD_NEXT, "bind");
    }
    if (addr && addr->sa_family == AF_INET &&
        len >= (socklen_t)sizeof(struct sockaddr_in)) {
        port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
    } else if (addr && addr->sa_family == AF_INET6 &&
               len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        port = ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
    }
    ret = real_bind(fd, addr, len);
    err = errno;
    if (path && port >= 0) {
        int type = 0;
        socklen_t tlen = sizeof(type);
        const char *kind = "other";
        char line[96];
        int n, log;

        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0) {
            kind = type == SOCK_STREAM ? "tcp" :
                   type == SOCK_DGRAM ? "udp" : "other";
        }
        n = snprintf(line, sizeof(line), "%d %s %d %s\n", (int)getpid(),
                     kind, port, ret == 0 ? "ok" :
                     err == EADDRINUSE ? "EADDRINUSE" : "error");
        log = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (log >= 0) {
            if (write(log, line, n) < 0) {
                /* best effort: the bind itself must not change */
            }
            close(log);
        }
    }
    errno = err;
    return ret;
}
