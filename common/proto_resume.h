#pragma once
#include <stddef.h>
#include <stdint.h>

int protoResumeLoad(const char *path,
                   size_t *outTotalSizeBytes,
                   size_t *outChunkSizeBytes,
                   size_t *outChunkCount,
                   uint8_t **outBitfield,
                   size_t *outBitfieldBytes);

int protoResumeSave(const char *path,
                   size_t totalSizeBytes,
                   size_t chunkSizeBytes,
                   size_t chunkCount,
                   const uint8_t *bitfield,
                   size_t bitfieldBytes);
