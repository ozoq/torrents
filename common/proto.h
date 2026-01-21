#pragma once
#include <stddef.h>
#include <stdint.h>

#define T_MAX_LINE 4096
#define T_CHUNK_SIZE 4096

int tWriteAll(int fd, const void *buffer, size_t byteCount);
int tReadLine(int fd, char *output, size_t outputSize); /* returns bytes, 0 on EOF, -1 on error */
int tConnectTcp(const char *host, const char *port);
int tListenTcp(const char *bindIp, const char *port, int backlog);

uint8_t *tBitmapAlloc(size_t bitCount);
void tBitmapSet(uint8_t *bitfield, size_t bitIndex);
int tBitmapGet(const uint8_t *bitfield, size_t bitIndex);
size_t tBitmapByteCount(size_t bitCount);

int tHexEncode(const uint8_t *input, size_t inputLength, char *output, size_t outputSize);
int tHexDecode(const char *hex, uint8_t *output, size_t outputLength);

void tLog(const char *component, const char *fmt, ...);
void tPerr(const char *component, const char *ctx);