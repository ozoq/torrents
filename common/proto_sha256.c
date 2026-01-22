#include "proto_sha256.h"
#include <string.h>

static uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }
static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t bsig0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static uint32_t bsig1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static uint32_t ssig0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
static uint32_t ssig1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }

static const uint32_t k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void store32be(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v);
}

static uint32_t load32be(const uint8_t in[4]) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

int protoSha256(const uint8_t *data, size_t len, uint8_t out32[32]) {
    if (!out32) return -1;

    uint32_t h[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };

    uint8_t block[64];
    uint64_t bitLen = (uint64_t)len * 8ull;

    size_t offset = 0;
    while (offset + 64 <= len) {
        const uint8_t *chunk = data + offset;
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = load32be(chunk + (size_t)i * 4);
        for (int i = 16; i < 64; i++) w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + bsig1(e) + ch(e,f,g) + k[i] + w[i];
            uint32_t t2 = bsig0(a) + maj(a,b,c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;

        offset += 64;
    }

    size_t rem = len - offset;
    memset(block, 0, sizeof block);
    if (rem && data) memcpy(block, data + offset, rem);
    block[rem] = 0x80;

    if (rem >= 56) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = load32be(block + (size_t)i * 4);
        for (int i = 16; i < 64; i++) w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + bsig1(e) + ch(e,f,g) + k[i] + w[i];
            uint32_t t2 = bsig0(a) + maj(a,b,c);
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;

        memset(block, 0, sizeof block);
    }

    block[56] = (uint8_t)(bitLen >> 56);
    block[57] = (uint8_t)(bitLen >> 48);
    block[58] = (uint8_t)(bitLen >> 40);
    block[59] = (uint8_t)(bitLen >> 32);
    block[60] = (uint8_t)(bitLen >> 24);
    block[61] = (uint8_t)(bitLen >> 16);
    block[62] = (uint8_t)(bitLen >> 8);
    block[63] = (uint8_t)(bitLen);

    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = load32be(block + (size_t)i * 4);
        for (int i = 16; i < 64; i++) w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + bsig1(e) + ch(e,f,g) + k[i] + w[i];
            uint32_t t2 = bsig0(a) + maj(a,b,c);
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    for (int i = 0; i < 8; i++) store32be(out32 + (size_t)i * 4, h[i]);
    return 0;
}

int protoSha256Hex(const uint8_t *data, size_t len, char outHex65[65]) {
    if (!outHex65) return -1;
    uint8_t digest[32];
    if (protoSha256(data, len, digest) != 0) return -1;

    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        outHex65[i * 2] = hex[(digest[i] >> 4) & 0xF];
        outHex65[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    outHex65[64] = 0;
    return 0;
}
