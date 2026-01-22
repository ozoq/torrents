#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

int protoWriteAll(int fd, const void *buffer, size_t byteCount);
int protoReadLine(int fd, char *output, size_t outputSize); /* returns bytes, 0 on EOF, -1 on error */
int protoConnectTcp(const char *host, const char *port);
int protoListenTcp(const char *bindIp, const char *port, int backlog);

void protoLog(const char *component, const char *fmt, ...);
void protoPerr(const char *component, const char *ctx);
void protoVLog(const char *component, const char *fmt, va_list ap);
