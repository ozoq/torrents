#pragma once
#include <stddef.h>
#include <stdint.h>

int protoHexEncode(const uint8_t *input, size_t inputLength, char *output, size_t outputSize);
int protoHexDecode(const char *hex, uint8_t *output, size_t outputLength);
