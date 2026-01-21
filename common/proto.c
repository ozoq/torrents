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

// tWriteAll - write all bytes by looping until done or error
int tWriteAll(int fd, const void *buffer, size_t byteCount) {
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

// tReadLine - read bytes until newline or buffer full then add zero at end
int tReadLine(int fd, char *output, size_t outputSize) {
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

// getAddressInfoV4 - call getaddrinfo to get ipv4 address list for host and port
static int getAddressInfoV4(const char *host, const char *port, int socketType, struct addrinfo **out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = socketType;
    return getaddrinfo(host, port, &hints, out);
}

// tConnectTcp - try connect to each address until one work
int tConnectTcp(const char *host, const char *port) {
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

// tListenTcp - create socket then bind then listen
int tListenTcp(const char *bindIp, const char *port, int backlog) {
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

// tBitmapAlloc - allocate zeroed bytes for bitCount bits
uint8_t *tBitmapAlloc(size_t bitCount) {
    size_t byteCount = tBitmapByteCount(bitCount);
    return (uint8_t*)calloc(byteCount ? byteCount : 1, 1);
}

// tBitmapByteCount - return how many bytes needed for bitCount bits
size_t tBitmapByteCount(size_t bitCount) { return (bitCount + 7) / 8; }

// tBitmapSet - set one bit in the bitfield
void tBitmapSet(uint8_t *bitfield, size_t bitIndex) { bitfield[bitIndex / 8] |= (uint8_t)(1u << (bitIndex % 8)); }

// tBitmapGet - read one bit from the bitfield
int tBitmapGet(const uint8_t *bitfield, size_t bitIndex) { return (bitfield[bitIndex / 8] >> (bitIndex % 8)) & 1; }

// hexval - parse one hex char to number or return -1
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// tHexEncode - convert bytes to hex string
int tHexEncode(const uint8_t *input, size_t inputLength, char *output, size_t outputSize) {
    static const char *HEX = "0123456789abcdef";
    if (outputSize < inputLength * 2 + 1) return -1;
    for (size_t i = 0; i < inputLength; i++) {
        output[i * 2 + 0] = HEX[(input[i] >> 4) & 0xF];
        output[i * 2 + 1] = HEX[input[i] & 0xF];
    }
    output[inputLength * 2] = 0;
    return 0;
}

// tHexDecode - convert hex string to bytes
int tHexDecode(const char *hex, uint8_t *output, size_t outputLength) {
    size_t hexLength = strlen(hex);
    if (hexLength != outputLength * 2) return -1;
    for (size_t i = 0; i < outputLength; i++) {
        int hi = hexval(hex[i * 2]);
        int lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        output[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// tLog - print time then component then pid then message to stderr
void tLog(const char *component, const char *fmt, ...) {
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

// tPerr - log error text from errno
void tPerr(const char *component, const char *ctx) {
    int e = errno;
    tLog(component, "%s: %s (errno=%d)", ctx ? ctx : "error", strerror(e), e);
}