#pragma once
#include <stddef.h>
#include <stdint.h>

int protoSha256(const uint8_t *data, size_t len, uint8_t out32[32]);
int protoSha256Hex(const uint8_t *data, size_t len, char outHex65[65]);
