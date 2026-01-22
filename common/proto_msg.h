#pragma once
#include <stddef.h>

typedef enum {
    PROTO_MSG_NONE = 0,
    PROTO_MSG_ANNOUNCE,
    PROTO_MSG_QUERY,
    PROTO_MSG_PEERS,
    PROTO_MSG_PEER,
    PROTO_MSG_GET,
    PROTO_MSG_DATA,
    PROTO_MSG_HASH_REQ,
    PROTO_MSG_HASH,
    PROTO_MSG_OK,
    PROTO_MSG_ERR,
} ProtoMsgType;

typedef struct {
    ProtoMsgType type;

    union {
        struct {
            int peerPort;
            const char *fileName;
            size_t fullSizeBytes;
            size_t haveSizeBytes;
            size_t chunkSizeBytes;
            const char *bitfieldHex;
        } announce;

        struct {
            const char *fileName;
        } query;

        struct {
            const char *fileName;
            size_t totalSizeBytes;
            size_t chunkSizeBytes;
            int peerCount;
        } peers;

        struct {
            const char *ip;
            int port;
            const char *bitfieldHex;
        } peer;

        struct {
            const char *fileName;
            size_t chunkIndex;
            size_t chunkSizeBytes;
        } get;

        struct {
            size_t byteCount;
        } data;

        struct {
            const char *fileName;
            size_t chunkIndex;
            size_t chunkSizeBytes;
        } hashReq;

        struct {
            size_t chunkIndex;
            const char *sha256Hex;
        } hash;

        struct {
            const char *reason;
        } err;
    } as;
} ProtoMsg;

int protoParseLine(char *line, ProtoMsg *out);

int protoBuildAnnounce(char *out, size_t outSize, int peerPort, const char *fileName,
                       size_t fullSizeBytes, size_t haveSizeBytes, size_t chunkSizeBytes,
                       const char *bitfieldHex);
int protoBuildQuery(char *out, size_t outSize, const char *fileName);
int protoBuildPeersHeader(char *out, size_t outSize, const char *fileName,
                          size_t totalSizeBytes, size_t chunkSizeBytes, int peerCount);
int protoBuildPeerRow(char *out, size_t outSize, const char *ip, int port, const char *bitfieldHex);
int protoBuildGet(char *out, size_t outSize, const char *fileName, size_t chunkIndex, size_t chunkSizeBytes);
int protoBuildDataHeader(char *out, size_t outSize, size_t byteCount);
int protoBuildHashReq(char *out, size_t outSize, const char *fileName, size_t chunkIndex, size_t chunkSizeBytes);
int protoBuildHashResp(char *out, size_t outSize, size_t chunkIndex, const char *sha256Hex);
int protoBuildOk(char *out, size_t outSize);
int protoBuildErr(char *out, size_t outSize, const char *reason);
