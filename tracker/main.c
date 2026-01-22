#include "../common/proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_FILES 128
#define MAX_PEERS 256
#define MAX_NAME  256
#define BACKLOG   16

typedef struct {
    char ip[64];
    int port;
    size_t bitfieldBytes;
    uint8_t *bitfield;
} PeerInfo;

typedef struct {
    char name[MAX_NAME];
    size_t totalSizeBytes;
    size_t chunkSizeBytes;
    size_t chunkCount;
    size_t bitfieldBytes;

    PeerInfo peers[MAX_PEERS];
    int peerCount;
} FileEntry;

static FileEntry g_files[MAX_FILES];
static int g_fileCount = 0;

// getFileEntry - find file by name or create new entry
static FileEntry *getFileEntry(const char *name) {
    for (int i = 0; i < g_fileCount; i++) {
        if (strcmp(g_files[i].name, name) == 0) return &g_files[i];
    }
    if (g_fileCount >= MAX_FILES) return NULL;

    FileEntry *entry = &g_files[g_fileCount++];
    memset(entry, 0, sizeof *entry);
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    return entry;
}

// getPeerForFile - find peer by ip and port or create new peer record
static PeerInfo *getPeerForFile(FileEntry *fileEntry, const char *ip, int port) {
    for (int i = 0; i < fileEntry->peerCount; i++) {
        if (strcmp(fileEntry->peers[i].ip, ip) == 0 && fileEntry->peers[i].port == port) {
            return &fileEntry->peers[i];
        }
    }
    if (fileEntry->peerCount >= MAX_PEERS) return NULL;

    PeerInfo *peer = &fileEntry->peers[fileEntry->peerCount++];
    memset(peer, 0, sizeof *peer);
    strncpy(peer->ip, ip, sizeof(peer->ip) - 1);
    peer->port = port;
    return peer;
}

// handleClient - read one line then handle ANNOUNCE or QUERY and send reply
static void handleClient(int clientFd, const char *remoteIp) {
    char line[PROTO_MAX_LINE];
    int bytesRead = protoReadLine(clientFd, line, sizeof line);
    if (bytesRead <= 0) return;

    char rawLine[PROTO_MAX_LINE];
    strncpy(rawLine, line, sizeof rawLine - 1);
    rawLine[sizeof rawLine - 1] = 0;

    ProtoMsg msg;
    if (protoParseLine(line, &msg) != 0) {
        protoLog("tracker", "bad request from %s: %s", remoteIp, rawLine);
        if (strncmp(rawLine, "ANNOUNCE ", 9) == 0) {
            protoWriteAll(clientFd, "ERR bad_announce\n", 17);
        } else if (strncmp(rawLine, "QUERY ", 6) == 0) {
            protoWriteAll(clientFd, "ERR bad_query\n", 14);
        } else {
            protoWriteAll(clientFd, "ERR bad_request\n", 16);
        }
        return;
    }

    // step 1 read command and parse it
    if (msg.type == PROTO_MSG_ANNOUNCE) {
        int peerPort = msg.as.announce.peerPort;
        const char *fileName = msg.as.announce.fileName;
        size_t fullSizeBytes = msg.as.announce.fullSizeBytes;
        size_t haveSizeBytes = msg.as.announce.haveSizeBytes;
        size_t chunkSizeBytes = msg.as.announce.chunkSizeBytes;
        const char *hexBitfield = msg.as.announce.bitfieldHex;

        // step 2 find file entry and check file meta match
        FileEntry *fileEntry = getFileEntry(fileName);
        if (!fileEntry) { protoWriteAll(clientFd, "ERR file_table_full\n", 20); return; }

        if (fileEntry->totalSizeBytes == 0) {
            fileEntry->totalSizeBytes = (size_t)fullSizeBytes;
            fileEntry->chunkSizeBytes = (size_t)chunkSizeBytes;

            fileEntry->chunkCount = 0;
            if (fileEntry->chunkSizeBytes) {
                fileEntry->chunkCount = (fileEntry->totalSizeBytes + fileEntry->chunkSizeBytes - 1) / fileEntry->chunkSizeBytes;
            }
            fileEntry->bitfieldBytes = protoBitmapByteCount(fileEntry->chunkCount);
        } else {
            if (fileEntry->totalSizeBytes != (size_t)fullSizeBytes || fileEntry->chunkSizeBytes != (size_t)chunkSizeBytes) {
                protoLog("tracker", "ANNOUNCE from %s:%d file=%s: meta mismatch", remoteIp, peerPort, fileName);
                protoWriteAll(clientFd, "ERR meta_mismatch\n", 18);
                return;
            }
        }

        // step 3 find peer record and store peer bitmap
        PeerInfo *peer = getPeerForFile(fileEntry, remoteIp, peerPort);
        if (!peer) { protoWriteAll(clientFd, "ERR peer_table_full\n", 20); return; }

        if (peer->bitfield) free(peer->bitfield);
        peer->bitfieldBytes = fileEntry->bitfieldBytes;
        peer->bitfield = (uint8_t*)calloc(peer->bitfieldBytes ? peer->bitfieldBytes : 1, 1);
        if (!peer->bitfield) { protoWriteAll(clientFd, "ERR oom\n", 8); return; }

        if (protoHexDecode(hexBitfield, peer->bitfield, peer->bitfieldBytes) != 0) {
            protoLog("tracker", "ANNOUNCE from %s:%d file=%s: bad bitmap", remoteIp, peerPort, fileName);
            protoWriteAll(clientFd, "ERR bad_bitmap\n", 15);
            return;
        }

        // step 4 reply OK
        protoLog("tracker", "ANNOUNCE from %s:%d file=%s full=%zu have=%zu chunk=%zu peers_now=%d",
              remoteIp, peerPort, fileEntry->name, fileEntry->totalSizeBytes, haveSizeBytes,
              fileEntry->chunkSizeBytes, fileEntry->peerCount);

        protoWriteAll(clientFd, "OK\n", 3);
        return;
    }

    if (msg.type == PROTO_MSG_QUERY) {
        const char *fileName = msg.as.query.fileName;

        // step 2 find file entry
        FileEntry *fileEntry = NULL;
        for (int i = 0; i < g_fileCount; i++) {
            if (strcmp(g_files[i].name, fileName) == 0) { fileEntry = &g_files[i]; break; }
        }
        if (!fileEntry) {
            protoLog("tracker", "QUERY from %s file=%s: not found", remoteIp, fileName);
            protoWriteAll(clientFd, "ERR not_found\n", 14);
            return;
        }

        // step 3 send peers header then send each peer row
        protoLog("tracker", "QUERY from %s file=%s: returning %d peers", remoteIp, fileEntry->name, fileEntry->peerCount);

        char header[256];
        int headerBytes = protoBuildPeersHeader(header, sizeof header, fileEntry->name, fileEntry->totalSizeBytes, fileEntry->chunkSizeBytes, fileEntry->peerCount);
        if (headerBytes >= 0) protoWriteAll(clientFd, header, (size_t)headerBytes);

        for (int i = 0; i < fileEntry->peerCount; i++) {
            PeerInfo *peer = &fileEntry->peers[i];
            if (!peer->bitfield) continue;

            char hexOut[PROTO_MAX_LINE];
            if (protoHexEncode(peer->bitfield, peer->bitfieldBytes, hexOut, sizeof hexOut) != 0) continue;

            char row[512 + PROTO_MAX_LINE];
            int rowBytes = protoBuildPeerRow(row, sizeof row, peer->ip, peer->port, hexOut);
            if (rowBytes >= 0) protoWriteAll(clientFd, row, (size_t)rowBytes);
        }
        return;
    }

    protoWriteAll(clientFd, "ERR unknown_cmd\n", 16);
}

// main - listen on tcp port then accept clients and handle one request per connection
int main(int argc, char **argv) {
    const char *portString = (argc >= 2) ? argv[1] : "9000";
    int listenFd = protoListenTcp("0.0.0.0", portString, BACKLOG);
    if (listenFd < 0) { perror("tracker listen"); return 1; }
    fprintf(stderr, "tracker listening on :%s\n", portString);
    fflush(stderr);

    for (;;) {
        struct sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof clientAddr;
        int clientFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientFd < 0) { perror("accept"); continue; }

        char remoteIp[64];
        inet_ntop(AF_INET, &clientAddr.sin_addr, remoteIp, sizeof remoteIp);

        protoLog("tracker", "client %s connected", remoteIp);
        handleClient(clientFd, remoteIp);
        close(clientFd);
    }
}
