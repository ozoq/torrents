#include "proto_msg.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

static void stripLineEnd(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = 0;
        n--;
    }
}

static int splitTokens(char *line, char **tokens, int maxTokens) {
    int count = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (count >= maxTokens) return -1;
        tokens[count++] = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = 0; p++; }
    }
    return count;
}

static int parseInt(const char *s, int *out) {
    if (!s || !*s) return -1;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end) return -1;
    if (v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}

static int parseSize(const char *s, size_t *out) {
    if (!s || !*s) return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end) return -1;
    *out = (size_t)v;
    return 0;
}

int protoParseLine(char *line, ProtoMsg *out) {
    if (!line || !out) return -1;
    memset(out, 0, sizeof *out);
    stripLineEnd(line);

    char *tokens[8];
    int n = splitTokens(line, tokens, (int)(sizeof tokens / sizeof tokens[0]));
    if (n <= 0) return -1;

    const char *cmd = tokens[0];
    if (strcmp(cmd, "OK") == 0) {
        out->type = PROTO_MSG_OK;
        return 0;
    }

    if (strcmp(cmd, "ERR") == 0) {
        out->type = PROTO_MSG_ERR;
        out->as.err.reason = (n >= 2) ? tokens[1] : "";
        return 0;
    }

    if (strcmp(cmd, "ANNOUNCE") == 0) {
        if (n != 7) return -1;
        out->type = PROTO_MSG_ANNOUNCE;
        if (parseInt(tokens[1], &out->as.announce.peerPort) != 0) return -1;
        out->as.announce.fileName = tokens[2];
        if (parseSize(tokens[3], &out->as.announce.fullSizeBytes) != 0) return -1;
        if (parseSize(tokens[4], &out->as.announce.haveSizeBytes) != 0) return -1;
        if (parseSize(tokens[5], &out->as.announce.chunkSizeBytes) != 0) return -1;
        out->as.announce.bitfieldHex = tokens[6];
        return 0;
    }

    if (strcmp(cmd, "QUERY") == 0) {
        if (n != 2) return -1;
        out->type = PROTO_MSG_QUERY;
        out->as.query.fileName = tokens[1];
        return 0;
    }

    if (strcmp(cmd, "PEERS") == 0) {
        if (n != 5) return -1;
        out->type = PROTO_MSG_PEERS;
        out->as.peers.fileName = tokens[1];
        if (parseSize(tokens[2], &out->as.peers.totalSizeBytes) != 0) return -1;
        if (parseSize(tokens[3], &out->as.peers.chunkSizeBytes) != 0) return -1;
        if (parseInt(tokens[4], &out->as.peers.peerCount) != 0) return -1;
        return 0;
    }

    if (strcmp(cmd, "P") == 0) {
        if (n != 4) return -1;
        out->type = PROTO_MSG_PEER;
        out->as.peer.ip = tokens[1];
        if (parseInt(tokens[2], &out->as.peer.port) != 0) return -1;
        out->as.peer.bitfieldHex = tokens[3];
        return 0;
    }

    if (strcmp(cmd, "GET") == 0) {
        if (n != 3 && n != 4) return -1;
        out->type = PROTO_MSG_GET;
        out->as.get.fileName = tokens[1];
        if (parseSize(tokens[2], &out->as.get.chunkIndex) != 0) return -1;
        out->as.get.chunkSizeBytes = 0;
        if (n == 4) {
            if (parseSize(tokens[3], &out->as.get.chunkSizeBytes) != 0) return -1;
        }
        return 0;
    }

    if (strcmp(cmd, "DATA") == 0) {
        if (n != 2) return -1;
        out->type = PROTO_MSG_DATA;
        if (parseSize(tokens[1], &out->as.data.byteCount) != 0) return -1;
        return 0;
    }

    if (strcmp(cmd, "HASH") == 0) {
        // Request:  HASH <file_name> <chunk_index> <chunk_size_bytes>
        // Response: HASH <chunk_index> <sha256_hex>
        if (n == 4) {
            out->type = PROTO_MSG_HASH_REQ;
            out->as.hashReq.fileName = tokens[1];
            if (parseSize(tokens[2], &out->as.hashReq.chunkIndex) != 0) return -1;
            if (parseSize(tokens[3], &out->as.hashReq.chunkSizeBytes) != 0) return -1;
            return 0;
        }
        if (n == 3) {
            out->type = PROTO_MSG_HASH;
            if (parseSize(tokens[1], &out->as.hash.chunkIndex) != 0) return -1;
            out->as.hash.sha256Hex = tokens[2];
            return 0;
        }
        return -1;
    }

    return -1;
}

int protoBuildAnnounce(char *out, size_t outSize, int peerPort, const char *fileName,
                       size_t fullSizeBytes, size_t haveSizeBytes, size_t chunkSizeBytes,
                       const char *bitfieldHex) {
    int n = snprintf(out, outSize, "ANNOUNCE %d %s %zu %zu %zu %s\n",
                     peerPort, fileName, fullSizeBytes, haveSizeBytes, chunkSizeBytes, bitfieldHex);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildQuery(char *out, size_t outSize, const char *fileName) {
    int n = snprintf(out, outSize, "QUERY %s\n", fileName);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildPeersHeader(char *out, size_t outSize, const char *fileName,
                          size_t totalSizeBytes, size_t chunkSizeBytes, int peerCount) {
    int n = snprintf(out, outSize, "PEERS %s %zu %zu %d\n", fileName, totalSizeBytes, chunkSizeBytes, peerCount);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildPeerRow(char *out, size_t outSize, const char *ip, int port, const char *bitfieldHex) {
    int n = snprintf(out, outSize, "P %s %d %s\n", ip, port, bitfieldHex);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildGet(char *out, size_t outSize, const char *fileName, size_t chunkIndex, size_t chunkSizeBytes) {
    if (chunkSizeBytes) {
        int n = snprintf(out, outSize, "GET %s %zu %zu\n", fileName, chunkIndex, chunkSizeBytes);
        return (n < 0 || (size_t)n >= outSize) ? -1 : n;
    }
    int n = snprintf(out, outSize, "GET %s %zu\n", fileName, chunkIndex);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildDataHeader(char *out, size_t outSize, size_t byteCount) {
    int n = snprintf(out, outSize, "DATA %zu\n", byteCount);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildHashReq(char *out, size_t outSize, const char *fileName, size_t chunkIndex, size_t chunkSizeBytes) {
    int n = snprintf(out, outSize, "HASH %s %zu %zu\n", fileName, chunkIndex, chunkSizeBytes);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildHashResp(char *out, size_t outSize, size_t chunkIndex, const char *sha256Hex) {
    if (!sha256Hex) sha256Hex = "";
    int n = snprintf(out, outSize, "HASH %zu %s\n", chunkIndex, sha256Hex);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildOk(char *out, size_t outSize) {
    int n = snprintf(out, outSize, "OK\n");
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}

int protoBuildErr(char *out, size_t outSize, const char *reason) {
    if (!reason) reason = "error";
    int n = snprintf(out, outSize, "ERR %s\n", reason);
    return (n < 0 || (size_t)n >= outSize) ? -1 : n;
}
