#pragma once
#include <stddef.h>
#include <stdint.h>

#define T_MAX_LINE 4096

// NEW: global chunk size (bytes). Keep tracker/peers consistent.
#define T_CHUNK_SIZE 4096

int t_write_all(int fd, const void *buf, size_t n);
int t_read_line(int fd, char *out, size_t outsz); /* returns bytes, 0 on EOF, -1 on error */
int t_connect_tcp(const char *host, const char *port);
int t_listen_tcp(const char *bind_ip, const char *port, int backlog);

uint8_t *t_bitmap_alloc(size_t nbits);
void t_bitmap_set(uint8_t *bm, size_t i);
int t_bitmap_get(const uint8_t *bm, size_t i);
size_t t_bitmap_nbytes(size_t nbits);

int t_hex_encode(const uint8_t *in, size_t inlen, char *out, size_t outsz);
int t_hex_decode(const char *hex, uint8_t *out, size_t outlen);

// stderr logging helpers (timestamped, component-tagged)
void t_log(const char *component, const char *fmt, ...);
void t_perr(const char *component, const char *ctx);
