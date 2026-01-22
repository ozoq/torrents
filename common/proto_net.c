#include "proto_net.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static int getAddressInfoV4(const char *host, const char *port, int socketType, struct addrinfo **out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = socketType;
    return getaddrinfo(host, port, &hints, out);
}

int protoWriteAll(int fd, const void *buffer, size_t byteCount) {
    const uint8_t *p = (const uint8_t*)buffer;
    size_t offset = 0;
    while (offset < byteCount) {
        ssize_t written = write(fd, p + offset, byteCount - offset);
        if (written < 0) { if (errno == EINTR) continue; return -1; }
        if (written == 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

int protoReadLine(int fd, char *output, size_t outputSize) {
    if (!output || outputSize == 0) return -1;
    size_t length = 0;
    while (length + 1 < outputSize) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { output[length] = 0; return (int)length; }
        output[length++] = c;
        if (c == '\n') break;
    }
    output[length] = 0;
    return (int)length;
}

int protoConnectTcp(const char *host, const char *port) {
    struct addrinfo *ai = NULL;
    if (getAddressInfoV4(host, port, SOCK_STREAM, &ai) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

int protoListenTcp(const char *bindIp, const char *port, int backlog) {
    struct addrinfo *ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(bindIp, port, &hints, &ai) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, backlog) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

void protoLog(const char *component, const char *fmt, ...) {
    if (!component) component = "app";
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);

    fprintf(stderr, "[%s] (%s:%d) ", ts, component, (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void protoPerr(const char *component, const char *ctx) {
    int e = errno;
    protoLog(component, "%s: %s (errno=%d)", ctx ? ctx : "error", strerror(e), e);
}

void protoVLog(const char *component, const char *fmt, va_list ap) {
    if (!component) component = "app";
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);

    fprintf(stderr, "[%s] (%s:%d) ", ts, component, (int)getpid());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}
