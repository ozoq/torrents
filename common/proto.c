#include "proto.h"
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

int t_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

int t_read_line(int fd, char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    size_t n = 0;
    while (n + 1 < outsz) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { out[n] = 0; return (int)n; }
        out[n++] = c;
        if (c == '\n') break;
    }
    out[n] = 0;
    return (int)n;
}

static int t_getaddr(const char *host, const char *port, int socktype, struct addrinfo **out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    return getaddrinfo(host, port, &hints, out);
}

int t_connect_tcp(const char *host, const char *port) {
    struct addrinfo *ai = NULL;
    if (t_getaddr(host, port, SOCK_STREAM, &ai) != 0) return -1;
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

int t_listen_tcp(const char *bind_ip, const char *port, int backlog) {
    struct addrinfo *ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(bind_ip, port, &hints, &ai) != 0) return -1;

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

uint8_t *t_bitmap_alloc(size_t nbits) {
    size_t nb = t_bitmap_nbytes(nbits);
    uint8_t *p = (uint8_t*)calloc(nb ? nb : 1, 1);
    return p;
}
size_t t_bitmap_nbytes(size_t nbits) { return (nbits + 7) / 8; }
void t_bitmap_set(uint8_t *bm, size_t i) { bm[i/8] |= (uint8_t)(1u << (i%8)); }
int t_bitmap_get(const uint8_t *bm, size_t i) { return (bm[i/8] >> (i%8)) & 1; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int t_hex_encode(const uint8_t *in, size_t inlen, char *out, size_t outsz) {
    static const char *H = "0123456789abcdef";
    if (outsz < inlen*2 + 1) return -1;
    for (size_t i=0; i<inlen; i++) {
        out[i*2+0] = H[(in[i]>>4)&0xF];
        out[i*2+1] = H[in[i]&0xF];
    }
    out[inlen*2] = 0;
    return 0;
}

int t_hex_decode(const char *hex, uint8_t *out, size_t outlen) {
    size_t hl = strlen(hex);
    if (hl != outlen*2) return -1;
    for (size_t i=0; i<outlen; i++) {
        int hi = hexval(hex[i*2]);
        int lo = hexval(hex[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi<<4) | lo);
    }
    return 0;
}

void t_log(const char *component, const char *fmt, ...) {
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

void t_perr(const char *component, const char *ctx) {
    int e = errno;
    t_log(component, "%s: %s (errno=%d)", ctx ? ctx : "error", strerror(e), e);
}
