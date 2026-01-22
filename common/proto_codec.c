#include "proto_codec.h"
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int protoHexEncode(const uint8_t *input, size_t inputLength, char *output, size_t outputSize) {
    static const char *HEX = "0123456789abcdef";
    if (outputSize < inputLength * 2 + 1) return -1;
    for (size_t i = 0; i < inputLength; i++) {
        output[i * 2 + 0] = HEX[(input[i] >> 4) & 0xF];
        output[i * 2 + 1] = HEX[input[i] & 0xF];
    }
    output[inputLength * 2] = 0;
    return 0;
}

int protoHexDecode(const char *hex, uint8_t *output, size_t outputLength) {
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
