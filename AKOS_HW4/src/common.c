#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool number(const char *s, int lo, int hi, int *out) {
    char *end;
    if (!s || *s < '0' || *s > '9') return false;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno || *end || val < lo || val > hi) return false;
    *out = (int)val;
    return true;
}

int split(char *s, char **words, int cap) {
    char *save = NULL;
    int n = 0;
    for (char *w = strtok_r(s, " \t\r", &save); w; w = strtok_r(NULL, " \t\r", &save)) {
        if (n == cap) return -1;
        words[n++] = w;
    }
    return n;
}

uint64_t fibonacci(int n) {
    uint64_t a = 0, b = 1;
    for (int i = 2; i <= n; ++i) {
        uint64_t next = a + b;
        a = b;
        b = next;
    }
    return n ? b : 0;
}

int64_t millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int tcp_open(const char *ip, const char *port, bool listener) {
    int n;
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (!number(port, 1, 65535, &n) || inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address.sin_port = htons((uint16_t)n);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (listener) {
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
            bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 128) == 0)
            return fd;
    } else if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        return fd;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

// TCP передаёт поток байтов. Даже одна короткая команда может прийти частями.
int send_line(int fd, const char *fmt, ...) {
    char buf[MAX_FRAME];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) return -1;
    buf[n++] = '\n';
    for (int off = 0; off < n;) {
        ssize_t sent = send(fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        off += (int)sent;
    }
    return 0;
}

int read_line(int fd, char *buf, size_t cap) {
    for (size_t i = 0; i + 1 < cap;) {
        char c;
        ssize_t got = recv(fd, &c, 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return got == 0 && i == 0 ? 0 : -1;
        if (c == '\0') return -1;
        if (c == '\n') {
            buf[i] = '\0';
            return 1;
        }
        buf[i++] = c;
    }
    return -1;
}
