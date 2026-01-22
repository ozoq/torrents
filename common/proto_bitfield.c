#include "proto_bitfield.h"
#include <stdlib.h>

uint8_t *protoBitmapAlloc(size_t bitCount) {
    size_t byteCount = protoBitmapByteCount(bitCount);
    return (uint8_t*)calloc(byteCount ? byteCount : 1, 1);
}

size_t protoBitmapByteCount(size_t bitCount) { return (bitCount + 7) / 8; }

void protoBitmapSet(uint8_t *bitfield, size_t bitIndex) { bitfield[bitIndex / 8] |= (uint8_t)(1u << (bitIndex % 8)); }

int protoBitmapGet(const uint8_t *bitfield, size_t bitIndex) { return (bitfield[bitIndex / 8] >> (bitIndex % 8)) & 1; }
