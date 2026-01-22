#pragma once
#include <stddef.h>
#include <stdint.h>

uint8_t *protoBitmapAlloc(size_t bitCount);
void protoBitmapSet(uint8_t *bitfield, size_t bitIndex);
int protoBitmapGet(const uint8_t *bitfield, size_t bitIndex);
size_t protoBitmapByteCount(size_t bitCount);
