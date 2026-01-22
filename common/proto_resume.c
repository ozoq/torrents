#include "proto_resume.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int writeAll(FILE *f, const void *buf, size_t n) {
    return (fwrite(buf, 1, n, f) == n) ? 0 : -1;
}

static int readAll(FILE *f, void *buf, size_t n) {
    return (fread(buf, 1, n, f) == n) ? 0 : -1;
}

int protoResumeLoad(const char *path,
                   size_t *outTotalSizeBytes,
                   size_t *outChunkSizeBytes,
                   size_t *outChunkCount,
                   uint8_t **outBitfield,
                   size_t *outBitfieldBytes) {
    if (!path || !outTotalSizeBytes || !outChunkSizeBytes || !outChunkCount || !outBitfield || !outBitfieldBytes) return -1;

    *outTotalSizeBytes = 0;
    *outChunkSizeBytes = 0;
    *outChunkCount = 0;
    *outBitfield = NULL;
    *outBitfieldBytes = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char magic[8];
    if (readAll(f, magic, sizeof magic) != 0) { fclose(f); return -1; }
    if (memcmp(magic, "Z78RSM1\0", 8) != 0) { fclose(f); return -1; }

    uint32_t version = 0;
    if (readAll(f, &version, sizeof version) != 0) { fclose(f); return -1; }
    if (version != 1) { fclose(f); return -1; }

    uint64_t total = 0;
    uint64_t chunkSize = 0;
    uint64_t chunkCount = 0;
    uint64_t bitfieldBytes = 0;

    if (readAll(f, &total, sizeof total) != 0) { fclose(f); return -1; }
    if (readAll(f, &chunkSize, sizeof chunkSize) != 0) { fclose(f); return -1; }
    if (readAll(f, &chunkCount, sizeof chunkCount) != 0) { fclose(f); return -1; }
    if (readAll(f, &bitfieldBytes, sizeof bitfieldBytes) != 0) { fclose(f); return -1; }

    if (bitfieldBytes > (1024ull * 1024ull * 64ull)) { fclose(f); return -1; }

    uint8_t *bitfield = (uint8_t*)calloc((size_t)bitfieldBytes ? (size_t)bitfieldBytes : 1, 1);
    if (!bitfield) { fclose(f); return -1; }

    if (bitfieldBytes && readAll(f, bitfield, (size_t)bitfieldBytes) != 0) {
        free(bitfield);
        fclose(f);
        return -1;
    }

    fclose(f);

    *outTotalSizeBytes = (size_t)total;
    *outChunkSizeBytes = (size_t)chunkSize;
    *outChunkCount = (size_t)chunkCount;
    *outBitfield = bitfield;
    *outBitfieldBytes = (size_t)bitfieldBytes;
    return 0;
}

int protoResumeSave(const char *path,
                   size_t totalSizeBytes,
                   size_t chunkSizeBytes,
                   size_t chunkCount,
                   const uint8_t *bitfield,
                   size_t bitfieldBytes) {
    if (!path || !bitfield) return -1;

    char tmpPath[1024];
    int n = snprintf(tmpPath, sizeof tmpPath, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmpPath) return -1;

    FILE *f = fopen(tmpPath, "wb");
    if (!f) return -1;

    unsigned char magic[8] = { 'Z','7','8','R','S','M','1',0 };
    uint32_t version = 1;
    uint64_t total = (uint64_t)totalSizeBytes;
    uint64_t chunkSize = (uint64_t)chunkSizeBytes;
    uint64_t chunks = (uint64_t)chunkCount;
    uint64_t bytes = (uint64_t)bitfieldBytes;

    int ok = 0;
    if (writeAll(f, magic, sizeof magic) != 0) ok = -1;
    if (!ok && writeAll(f, &version, sizeof version) != 0) ok = -1;
    if (!ok && writeAll(f, &total, sizeof total) != 0) ok = -1;
    if (!ok && writeAll(f, &chunkSize, sizeof chunkSize) != 0) ok = -1;
    if (!ok && writeAll(f, &chunks, sizeof chunks) != 0) ok = -1;
    if (!ok && writeAll(f, &bytes, sizeof bytes) != 0) ok = -1;
    if (!ok && bitfieldBytes && writeAll(f, bitfield, bitfieldBytes) != 0) ok = -1;

    if (fflush(f) != 0) ok = -1;
    if (fclose(f) != 0) ok = -1;

    if (ok != 0) {
        unlink(tmpPath);
        return -1;
    }

    if (rename(tmpPath, path) != 0) {
        unlink(tmpPath);
        return -1;
    }

    return 0;
}
