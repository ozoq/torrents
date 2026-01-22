#include "../common/proto.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#define BACKLOG 16
#define MAX_PEERS 64
#define MAX_FILE 256
#define WORKERS  4

typedef struct {
    char ip[64];
    int port;

    uint8_t *bitfield;
    size_t bitfieldBytes;

    size_t chunksSent;
    size_t bytesSent;
} SeedPeer;

typedef struct {
    char fileName[MAX_FILE];
    char outputPath[512];

    size_t totalSizeBytes;
    size_t chunkSizeBytes;
    size_t chunkCount;

    SeedPeer peers[MAX_PEERS];
    int peerCount;

    uint8_t *haveBitfield;
    uint8_t *inFlightBitfield;

    pthread_mutex_t mutex;

    char resumePath[768];
    time_t lastResumeSave;
} DownloadJob;

static volatile sig_atomic_t g_stopRequested = 0;

static void handleStopSignal(int sig) {
    (void)sig;
    g_stopRequested = 1;
}

// clearBit - clear one bit in bitfield by masking that bit out
static void clearBit(uint8_t *bitfield, size_t bitIndex) {
    bitfield[bitIndex / 8] &= (uint8_t)~(1u << (bitIndex % 8));
}

// getFileSizeBytes - use stat to get file size and put it to outSizeBytes
static int getFileSizeBytes(const char *path, size_t *outSizeBytes) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *outSizeBytes = (size_t)st.st_size;
    return 0;
}

static int buildResumePath(char *out, size_t outSize, const char *path) {
    int n = snprintf(out, outSize, "%s.resume", path);
    return (n < 0 || (size_t)n >= outSize) ? -1 : 0;
}

static int loadOrInitHaveBitfieldForPath(const char *path,
                                        size_t totalSizeBytes,
                                        size_t chunkSizeBytes,
                                        uint8_t *outBitfield,
                                        size_t outBitfieldBytes) {
    if (!path || !outBitfield) return -1;

    char resumePath[768];
    if (buildResumePath(resumePath, sizeof resumePath, path) != 0) return -1;

    size_t rTotal = 0, rChunk = 0, rCount = 0, rBytes = 0;
    uint8_t *rBits = NULL;
    if (protoResumeLoad(resumePath, &rTotal, &rChunk, &rCount, &rBits, &rBytes) == 0) {
        size_t expectedCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
        if (rTotal == totalSizeBytes && rChunk == chunkSizeBytes && rCount == expectedCount && rBytes == outBitfieldBytes) {
            memcpy(outBitfield, rBits, outBitfieldBytes);
            free(rBits);
            return 0;
        }
        free(rBits);
    }

    // Fallback (prefix-based): infer from file size. This supports existing tests and
    // non-gap resume. For true sparse/gap support, create/update the .resume bitmap.
    size_t haveSizeBytes = 0;
    if (getFileSizeBytes(path, &haveSizeBytes) != 0) return -1;
    if (haveSizeBytes > totalSizeBytes) haveSizeBytes = totalSizeBytes;

    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
    size_t haveChunks = 0;
    if (chunkSizeBytes) {
        if (haveSizeBytes == totalSizeBytes) {
            haveChunks = chunkCount;
        } else {
            haveChunks = haveSizeBytes / chunkSizeBytes; // floor: excludes partial chunk
        }
    }

    memset(outBitfield, 0, outBitfieldBytes);
    for (size_t i = 0; i < haveChunks; i++) protoBitmapSet(outBitfield, i);

    (void)protoResumeSave(resumePath, totalSizeBytes, chunkSizeBytes, chunkCount, outBitfield, outBitfieldBytes);
    return 0;
}

// buildBitfieldForLocalFile - look at file size and mark chunks we already have
static int buildBitfieldForLocalFile(const char *path, size_t totalSizeBytes, size_t chunkSizeBytes,
                                     uint8_t *outBitfield, size_t outBitfieldBytes) {
    return loadOrInitHaveBitfieldForPath(path, totalSizeBytes, chunkSizeBytes, outBitfield, outBitfieldBytes);
}

static int computeChunkHashHex(const char *path,
                               size_t totalSizeBytes,
                               size_t chunkSizeBytes,
                               size_t chunkIndex,
                               char outHex65[65]) {
    if (!path || !outHex65 || chunkSizeBytes == 0) return -1;

    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
    if (chunkIndex >= chunkCount) return -1;

    size_t expectedBytes = chunkSizeBytes;
    if (chunkIndex + 1 == chunkCount) {
        size_t remaining = totalSizeBytes - (chunkIndex * chunkSizeBytes);
        if (remaining < expectedBytes) expectedBytes = remaining;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)(chunkIndex * chunkSizeBytes), SEEK_SET) != 0) { fclose(f); return -1; }

    uint8_t *buf = (uint8_t*)malloc(expectedBytes ? expectedBytes : 1);
    if (!buf) { fclose(f); return -1; }

    size_t got = fread(buf, 1, expectedBytes, f);
    fclose(f);

    int rc = -1;
    if (got == expectedBytes) {
        rc = protoSha256Hex(buf, got, outHex65);
    }
    free(buf);
    return rc;
}

static int requestRemoteChunkHash(const char *ip,
                                  int port,
                                  const char *fileName,
                                  size_t chunkIndex,
                                  size_t chunkSizeBytes,
                                  char outHex65[65]) {
    if (!ip || !fileName || !outHex65) return -1;

    char portString[16];
    snprintf(portString, sizeof portString, "%d", port);

    int fd = protoConnectTcp(ip, portString);
    if (fd < 0) return -1;

    char reqLine[PROTO_MAX_LINE];
    int reqBytes = protoBuildHashReq(reqLine, sizeof reqLine, fileName, chunkIndex, chunkSizeBytes);
    if (reqBytes < 0 || protoWriteAll(fd, reqLine, (size_t)reqBytes) != 0) { close(fd); return -1; }

    char line[PROTO_MAX_LINE];
    if (protoReadLine(fd, line, sizeof line) <= 0) { close(fd); return -1; }
    close(fd);

    ProtoMsg resp;
    if (protoParseLine(line, &resp) != 0 || resp.type != PROTO_MSG_HASH) return -1;
    if (resp.as.hash.chunkIndex != chunkIndex) return -1;

    strncpy(outHex65, resp.as.hash.sha256Hex ? resp.as.hash.sha256Hex : "", 64);
    outHex65[64] = 0;
    return 0;
}

// announceToTracker - build bitfield then send ANNOUNCE to tracker and read OK or ERR
static int announceToTracker(const char *trackerHost, const char *trackerPort,
                            int peerPort, const char *fileName, const char *path,
                            size_t chunkSizeBytes, size_t totalSizeBytes) {
    size_t haveSizeBytes = 0;
    if (getFileSizeBytes(path, &haveSizeBytes) != 0) return -1;

    if (totalSizeBytes == 0) totalSizeBytes = haveSizeBytes;

    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
    size_t bitfieldBytes = protoBitmapByteCount(chunkCount);

    uint8_t *bitfield = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
    if (!bitfield) return -1;

    if (buildBitfieldForLocalFile(path, totalSizeBytes, chunkSizeBytes, bitfield, bitfieldBytes) != 0) {
        free(bitfield);
        return -1;
    }

    char hexBitfield[PROTO_MAX_LINE];
    char msg[PROTO_MAX_LINE];
    if (protoHexEncode(bitfield, bitfieldBytes, hexBitfield, sizeof hexBitfield) != 0) {
        free(bitfield);
        return -1;
    }
    free(bitfield);

    int fd = protoConnectTcp(trackerHost, trackerPort);
    if (fd < 0) { protoPerr("peer", "connect tracker (announce)"); return -1; }

    int msgBytes = protoBuildAnnounce(msg, sizeof msg, peerPort, fileName, totalSizeBytes, haveSizeBytes, chunkSizeBytes, hexBitfield);
    if (msgBytes < 0 || protoWriteAll(fd, msg, (size_t)msgBytes) != 0) { close(fd); return -1; }

    char line[PROTO_MAX_LINE];
    if (protoReadLine(fd, line, sizeof line) <= 0) { protoPerr("peer", "read tracker (announce)"); close(fd); return -1; }
    close(fd);

    ProtoMsg resp;
    if (protoParseLine(line, &resp) == 0 && resp.type == PROTO_MSG_OK) {
        protoLog("peer", "ANNOUNCE ok file=%s full_size=%zu have_size=%zu chunk=%zu",
              fileName, totalSizeBytes, haveSizeBytes, chunkSizeBytes);
        return 0;
    }

    protoLog("peer", "ANNOUNCE failed: %s", line);
    return -1;
}

static int queryTrackerMetaAndPeers(const char *trackerHost, const char *trackerPort, const char *fileName,
                                    size_t *outTotalSizeBytes, size_t *outChunkSizeBytes,
                                    SeedPeer *outPeers, int *outPeerCount) {
    if (!trackerHost || !trackerPort || !fileName || !outTotalSizeBytes || !outChunkSizeBytes || !outPeers || !outPeerCount) return -1;

    *outTotalSizeBytes = 0;
    *outChunkSizeBytes = 0;
    *outPeerCount = 0;

    int fd = protoConnectTcp(trackerHost, trackerPort);
    if (fd < 0) return -1;

    char queryLine[PROTO_MAX_LINE];
    int queryBytes = protoBuildQuery(queryLine, sizeof queryLine, fileName);
    if (queryBytes < 0 || protoWriteAll(fd, queryLine, (size_t)queryBytes) != 0) { close(fd); return -1; }

    char line[PROTO_MAX_LINE];
    if (protoReadLine(fd, line, sizeof line) <= 0) { close(fd); return -1; }

    ProtoMsg hdr;
    if (protoParseLine(line, &hdr) != 0) { close(fd); return -1; }
    if (hdr.type == PROTO_MSG_ERR) {
        close(fd);
        return -2; // not found / other error
    }
    if (hdr.type != PROTO_MSG_PEERS) { close(fd); return -1; }

    int advertisedPeerCount = hdr.as.peers.peerCount;
    *outTotalSizeBytes = hdr.as.peers.totalSizeBytes;
    *outChunkSizeBytes = hdr.as.peers.chunkSizeBytes;

    size_t chunkCount = (*outTotalSizeBytes + *outChunkSizeBytes - 1) / *outChunkSizeBytes;
    size_t bitfieldBytes = protoBitmapByteCount(chunkCount);

    int count = 0;
    for (int i = 0; i < advertisedPeerCount && i < MAX_PEERS; i++) {
        if (protoReadLine(fd, line, sizeof line) <= 0) break;
        ProtoMsg row;
        if (protoParseLine(line, &row) != 0 || row.type != PROTO_MSG_PEER) continue;

        SeedPeer *peer = &outPeers[count++];
        memset(peer, 0, sizeof *peer);
        strncpy(peer->ip, row.as.peer.ip, sizeof(peer->ip) - 1);
        peer->port = row.as.peer.port;

        peer->bitfieldBytes = bitfieldBytes;
        peer->bitfield = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
        if (!peer->bitfield) { close(fd); return -1; }

        if (protoHexDecode(row.as.peer.bitfieldHex, peer->bitfield, bitfieldBytes) != 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    *outPeerCount = count;
    return 0;
}

static void freeSeedPeers(SeedPeer *peers, int peerCount) {
    for (int i = 0; i < peerCount; i++) {
        free(peers[i].bitfield);
        peers[i].bitfield = NULL;
    }
}

static int verifySeedAgainstExistingPeers(const char *filePath,
                                         const char *fileName,
                                         size_t totalSizeBytes,
                                         size_t chunkSizeBytes,
                                         const uint8_t *haveBitfield,
                                         const SeedPeer *peers,
                                         int peerCount) {
    if (!filePath || !fileName || !haveBitfield || !peers) return 0;
    if (peerCount <= 0) return 0;

    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;

    int verified = 0;
    int attempts = 0;
    while (verified < 3 && attempts < 50) {
        attempts++;

        int p = rand() % peerCount;
        const SeedPeer *peer = &peers[p];
        if (!peer->bitfield) continue;

        size_t start = (size_t)(rand() % (chunkCount ? chunkCount : 1));
        size_t chosen = (size_t)-1;
        for (size_t i = 0; i < chunkCount; i++) {
            size_t idx = (start + i) % chunkCount;
            if (!protoBitmapGet(haveBitfield, idx)) continue;
            if (!protoBitmapGet(peer->bitfield, idx)) continue;
            chosen = idx;
            break;
        }
        if (chosen == (size_t)-1) continue;

        char localHex[65];
        char remoteHex[65];
        if (computeChunkHashHex(filePath, totalSizeBytes, chunkSizeBytes, chosen, localHex) != 0) continue;
        if (requestRemoteChunkHash(peer->ip, peer->port, fileName, chosen, chunkSizeBytes, remoteHex) != 0) continue;

        if (strcmp(localHex, remoteHex) != 0) {
            protoLog("peer", "SEED rejected: content mismatch file=%s chunk=%zu peer=%s:%d",
                     fileName, chosen, peer->ip, peer->port);
            return -1;
        }

        verified++;
    }

    if (verified > 0) {
        protoLog("peer", "SEED verified: matched %d chunk hashes", verified);
    }
    return 0;
}

// queryTracker - send QUERY to tracker then read peers list and save it into job
static int queryTracker(DownloadJob *job, const char *trackerHost, const char *trackerPort) {
    // step 1 connect and send QUERY
    int fd = protoConnectTcp(trackerHost, trackerPort);
    if (fd < 0) { protoPerr("peer", "connect tracker (query)"); return -1; }

    char queryLine[PROTO_MAX_LINE];
    int queryBytes = protoBuildQuery(queryLine, sizeof queryLine, job->fileName);
    if (queryBytes < 0 || protoWriteAll(fd, queryLine, (size_t)queryBytes) != 0) { close(fd); return -1; }

    char line[PROTO_MAX_LINE];
    if (protoReadLine(fd, line, sizeof line) <= 0) { protoPerr("peer", "read tracker (query hdr)"); close(fd); return -1; }
    ProtoMsg hdr;
    if (protoParseLine(line, &hdr) != 0 || hdr.type != PROTO_MSG_PEERS) {
        protoLog("peer", "QUERY unexpected: %s", line);
        close(fd);
        return -1;
    }

    // step 2 read header and allocate job bitfields
    int advertisedPeerCount = hdr.as.peers.peerCount;
    job->totalSizeBytes = hdr.as.peers.totalSizeBytes;
    job->chunkSizeBytes = hdr.as.peers.chunkSizeBytes;

    job->chunkCount = (job->totalSizeBytes + job->chunkSizeBytes - 1) / job->chunkSizeBytes;
    job->haveBitfield = protoBitmapAlloc(job->chunkCount);
    job->inFlightBitfield = protoBitmapAlloc(job->chunkCount);
    pthread_mutex_init(&job->mutex, NULL);

    size_t bitfieldBytes = protoBitmapByteCount(job->chunkCount);

    // step 3 read each peer row and decode bitmap
    job->peerCount = 0;
    for (int i = 0; i < advertisedPeerCount && i < MAX_PEERS; i++) {
        if (protoReadLine(fd, line, sizeof line) <= 0) break;
        ProtoMsg row;
        if (protoParseLine(line, &row) != 0 || row.type != PROTO_MSG_PEER) continue;

        SeedPeer *peer = &job->peers[job->peerCount++];
        memset(peer, 0, sizeof *peer);
        strncpy(peer->ip, row.as.peer.ip, sizeof(peer->ip) - 1);
        peer->port = row.as.peer.port;

        peer->bitfieldBytes = bitfieldBytes;
        peer->bitfield = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
        if (!peer->bitfield) { close(fd); return -1; }

        if (protoHexDecode(row.as.peer.bitfieldHex, peer->bitfield, bitfieldBytes) != 0) {
            protoLog("peer", "bad peer bitmap from tracker");
            close(fd);
            return -1;
        }

        protoLog("peer", "seed[%d]=%s:%d", job->peerCount - 1, peer->ip, peer->port);
    }

    close(fd);
    protoLog("peer", "QUERY file=%s -> size=%zu chunk=%zu nchunks=%zu npeers=%d",
          job->fileName, job->totalSizeBytes, job->chunkSizeBytes, job->chunkCount, advertisedPeerCount);

    return (job->peerCount > 0) ? 0 : -1;
}

// serveOne - read GET request then read chunk from disk and reply with DATA
static int serveOne(int clientFd, const char *dataDir) {
    char line[PROTO_MAX_LINE];
    if (protoReadLine(clientFd, line, sizeof line) <= 0) return -1;

    ProtoMsg req;
    if (protoParseLine(line, &req) != 0) {
        protoWriteAll(clientFd, "ERR bad_request\n", 16);
        return -1;
    }

    const char *fileName = NULL;
    size_t chunkIndex = 0;
    size_t chunkSizeBytes = 0;

    int wantData = 0;
    int wantHash = 0;
    if (req.type == PROTO_MSG_GET) {
        wantData = 1;
        fileName = req.as.get.fileName;
        chunkIndex = req.as.get.chunkIndex;
        chunkSizeBytes = req.as.get.chunkSizeBytes ? req.as.get.chunkSizeBytes : (size_t)PROTO_CHUNK_SIZE;
    } else if (req.type == PROTO_MSG_HASH_REQ) {
        wantHash = 1;
        fileName = req.as.hashReq.fileName;
        chunkIndex = req.as.hashReq.chunkIndex;
        chunkSizeBytes = req.as.hashReq.chunkSizeBytes;
    } else {
        protoWriteAll(clientFd, "ERR bad_request\n", 16);
        return -1;
    }

    if (chunkSizeBytes == 0) {
        protoWriteAll(clientFd, "ERR bad_get\n", 12);
        return -1;
    }

    protoLog("peer", "serve: %s file=%s chunk=%zu chunk_size=%zu", wantHash ? "HASH" : "GET", fileName, chunkIndex, chunkSizeBytes);

    char path[768];
    snprintf(path, sizeof path, "%s/%s", dataDir, fileName);

    FILE *f = fopen(path, "rb");
    if (!f) { protoWriteAll(clientFd, "ERR no_file\n", 12); return -1; }

    if (fseek(f, (long)((size_t)chunkIndex * chunkSizeBytes), SEEK_SET) != 0) {
        fclose(f);
        protoWriteAll(clientFd, "ERR seek\n", 9);
        return -1;
    }

    uint8_t *buf = (uint8_t*)malloc(chunkSizeBytes);
    if (!buf) { fclose(f); protoWriteAll(clientFd, "ERR oom\n", 8); return -1; }

    size_t bytesRead = fread(buf, 1, chunkSizeBytes, f);
    fclose(f);

    if (wantHash && bytesRead == 0) {
        free(buf);
        protoWriteAll(clientFd, "ERR seek\n", 9);
        return -1;
    }

    if (wantHash) {
        char hex[65];
        if (protoSha256Hex(buf, bytesRead, hex) != 0) { free(buf); protoWriteAll(clientFd, "ERR oom\n", 8); return -1; }
        char resp[128];
        int respBytes = protoBuildHashResp(resp, sizeof resp, chunkIndex, hex);
        if (respBytes < 0 || protoWriteAll(clientFd, resp, (size_t)respBytes) != 0) { free(buf); return -1; }
        free(buf);
        return 0;
    }

    protoLog("peer", "serve: DATA nbytes=%zu file=%s chunk=%zu", bytesRead, fileName, chunkIndex);

    char header[64];
    int headerBytes = protoBuildDataHeader(header, sizeof header, bytesRead);
    if (headerBytes < 0 || protoWriteAll(clientFd, header, (size_t)headerBytes) != 0) { free(buf); return -1; }
    if (bytesRead && protoWriteAll(clientFd, buf, bytesRead) != 0) { free(buf); return -1; }

    free(buf);
    return 0;
}

// serveThread - listen tcp and accept clients then call serveOne
static void *serveThread(void *arg) {
    const char **args = (const char**)arg;
    const char *listenPort = args[0];
    const char *dataDir = args[1];

    int listenFd = protoListenTcp("0.0.0.0", listenPort, BACKLOG);
    if (listenFd < 0) { perror("peer listen"); return NULL; }

    for (;;) {
        int clientFd = accept(listenFd, NULL, NULL);
        if (clientFd < 0) continue;
        serveOne(clientFd, dataDir);
        close(clientFd);
    }
    return NULL;
}

// fetchChunk - connect to peer then GET chunk then write it to output file and mark have bit
static int fetchChunk(DownloadJob *job, size_t chunkIndex, SeedPeer *peer, FILE *outFile) {
    int result = -1;
    int fd = -1;
    uint8_t *buffer = NULL;

    // step 1 connect to peer and send GET chunk
    char portString[16];
    snprintf(portString, sizeof portString, "%d", peer->port);

    protoLog("peer", "fetch: chunk=%zu from %s:%d", chunkIndex, peer->ip, peer->port);

    fd = protoConnectTcp(peer->ip, portString);
    if (fd < 0) { protoPerr("peer", "connect seed"); goto cleanup; }

    char request[PROTO_MAX_LINE];
    int requestBytes = protoBuildGet(request, sizeof request, job->fileName, chunkIndex, job->chunkSizeBytes);
    if (requestBytes < 0 || protoWriteAll(fd, request, (size_t)requestBytes) != 0) goto cleanup;

    // step 2 read DATA header and read bytes
    char line[PROTO_MAX_LINE];
    if (protoReadLine(fd, line, sizeof line) <= 0) goto cleanup;

    ProtoMsg dataHdr;
    if (protoParseLine(line, &dataHdr) != 0 || dataHdr.type != PROTO_MSG_DATA) goto cleanup;
    size_t receivedBytes = dataHdr.as.data.byteCount;

    size_t expectedBytes = job->chunkSizeBytes;
    if (chunkIndex + 1 == job->chunkCount) {
        size_t remaining = job->totalSizeBytes - (chunkIndex * job->chunkSizeBytes);
        if (remaining < expectedBytes) expectedBytes = remaining;
    }

    if (receivedBytes == 0 || receivedBytes > expectedBytes) {
        pthread_mutex_lock(&job->mutex);
        if (peer->bitfield) clearBit(peer->bitfield, chunkIndex);
        pthread_mutex_unlock(&job->mutex);
        goto cleanup;
    }

    buffer = (uint8_t*)malloc(receivedBytes);
    if (!buffer) goto cleanup;

    size_t offset = 0;
    while (offset < receivedBytes) {
        ssize_t r = read(fd, buffer + offset, receivedBytes - offset);
        if (r <= 0) goto cleanup;
        offset += (size_t)r;
    }

    // step 3 write to output file and mark chunk as done
    uint8_t *resumeSnapshot = NULL;
    size_t resumeSnapshotBytes = 0;
    size_t resumeTotal = 0;
    size_t resumeChunk = 0;
    size_t resumeCount = 0;
    char resumePath[768];
    resumePath[0] = 0;

    pthread_mutex_lock(&job->mutex);

    if (protoBitmapGet(job->haveBitfield, chunkIndex)) {
        pthread_mutex_unlock(&job->mutex);
        result = 0;
        goto cleanup;
    }

    long fileOffset = (long)(chunkIndex * job->chunkSizeBytes);
    fseek(outFile, fileOffset, SEEK_SET);
    fwrite(buffer, 1, receivedBytes, outFile);

    protoBitmapSet(job->haveBitfield, chunkIndex);

    peer->chunksSent += 1;
    peer->bytesSent += receivedBytes;

    // Snapshot resume bitmap at most once per second.
    time_t now = time(NULL);
    if (job->resumePath[0] && (job->lastResumeSave == 0 || now - job->lastResumeSave >= 1)) {
        job->lastResumeSave = now;
        resumeSnapshotBytes = protoBitmapByteCount(job->chunkCount);
        resumeSnapshot = (uint8_t*)malloc(resumeSnapshotBytes ? resumeSnapshotBytes : 1);
        if (resumeSnapshot) {
            memcpy(resumeSnapshot, job->haveBitfield, resumeSnapshotBytes);
            resumeTotal = job->totalSizeBytes;
            resumeChunk = job->chunkSizeBytes;
            resumeCount = job->chunkCount;
            strncpy(resumePath, job->resumePath, sizeof resumePath - 1);
            resumePath[sizeof resumePath - 1] = 0;
        }
    }

    pthread_mutex_unlock(&job->mutex);

    if (resumeSnapshot && resumePath[0]) {
        (void)protoResumeSave(resumePath, resumeTotal, resumeChunk, resumeCount, resumeSnapshot, resumeSnapshotBytes);
        free(resumeSnapshot);
    } else if (resumeSnapshot) {
        free(resumeSnapshot);
    }

    protoLog("peer", "fetch: chunk=%zu done nbytes=%zu", chunkIndex, receivedBytes);
    result = 0;

cleanup:
    // step 4 always clear inflight bit and close/free
    pthread_mutex_lock(&job->mutex);
    if (job->inFlightBitfield) clearBit(job->inFlightBitfield, chunkIndex);
    pthread_mutex_unlock(&job->mutex);

    if (buffer) free(buffer);
    if (fd >= 0) close(fd);
    return result;
}

// pickSeedForChunk - pick random peer then go forward until find one that have this chunk
static const SeedPeer *pickSeedForChunk(DownloadJob *job, size_t chunkIndex, unsigned workerId) {
    int startIndex = 0;
    if (job->peerCount > 0) startIndex = (int)((workerId + (unsigned)rand()) % (unsigned)job->peerCount);

    for (int i = 0; i < job->peerCount; i++) {
        const SeedPeer *peer = &job->peers[(startIndex + i) % job->peerCount];
        if (peer->bitfield && protoBitmapGet(peer->bitfield, chunkIndex)) return peer;
    }
    return NULL;
}

typedef struct { DownloadJob *job; FILE *outFile; unsigned workerId; } WorkerArgs;

static unsigned nextRand(unsigned *state) {
    // simple LCG
    *state = (*state * 1103515245u + 12345u);
    return *state;
}

// worker - pick next missing chunk then download it from some peer until all done or failed
static void *worker(void *arg) {
    WorkerArgs *args = (WorkerArgs*)arg;
    DownloadJob *job = args->job;

    unsigned rng = (unsigned)time(NULL) ^ (unsigned)getpid() ^ (args->workerId * 2654435761u);

    for (;;) {
        if (g_stopRequested) break;
        size_t chosenChunk = (size_t)-1;

        // step 1 pick a chunk we dont have and mark it inflight
        pthread_mutex_lock(&job->mutex);
        size_t start = (job->chunkCount > 0) ? (size_t)(nextRand(&rng) % (unsigned)job->chunkCount) : 0;
        for (size_t scan = 0; scan < job->chunkCount; scan++) {
            size_t i = (start + scan) % job->chunkCount;
            if (protoBitmapGet(job->haveBitfield, i)) continue;
            if (job->inFlightBitfield && protoBitmapGet(job->inFlightBitfield, i)) continue;

            int anyPeerHasChunk = 0;
            for (int p = 0; p < job->peerCount; p++) {
                if (job->peers[p].bitfield && protoBitmapGet(job->peers[p].bitfield, i)) { anyPeerHasChunk = 1; break; }
            }

            if (!anyPeerHasChunk) { chosenChunk = i; break; }
            if (job->inFlightBitfield) protoBitmapSet(job->inFlightBitfield, i);

            chosenChunk = i;
            break;
        }
        pthread_mutex_unlock(&job->mutex);

        if (chosenChunk == (size_t)-1) break;

        // step 2 check at least one peer have it
        pthread_mutex_lock(&job->mutex);
        int anyPeerHasChunk = 0;
        for (int p = 0; p < job->peerCount; p++) {
            if (job->peers[p].bitfield && protoBitmapGet(job->peers[p].bitfield, chosenChunk)) { anyPeerHasChunk = 1; break; }
        }
        pthread_mutex_unlock(&job->mutex);

        if (!anyPeerHasChunk) {
            protoLog("peer", "GET failed: missing chunk %zu (no peer has it)", chosenChunk);
            return NULL;
        }

        // step 3 pick a peer and download this chunk
        const SeedPeer *chosenPeer = pickSeedForChunk(job, chosenChunk, args->workerId);
        if (!chosenPeer) { usleep(50 * 1000); continue; }

        (void)fetchChunk(job, chosenChunk, (SeedPeer*)chosenPeer, args->outFile);
    }
    return NULL;
}

// cmdGet - query tracker then start workers and wait then check all chunks are downloaded
static int cmdGet(const char *trackerHost, const char *trackerPort, const char *fileName, const char *outPath) {
    // step 1 query tracker
    protoLog("peer", "GET start file=%s out=%s tracker=%s:%s", fileName, outPath, trackerHost, trackerPort);

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    DownloadJob job;
    memset(&job, 0, sizeof job);
    strncpy(job.fileName, fileName, sizeof(job.fileName) - 1);
    strncpy(job.outputPath, outPath, sizeof(job.outputPath) - 1);

    if (queryTracker(&job, trackerHost, trackerPort) != 0) { protoLog("peer", "GET: query failed"); return -1; }

    if (buildResumePath(job.resumePath, sizeof job.resumePath, outPath) != 0) {
        protoLog("peer", "GET: cannot build resume path");
        return -1;
    }
    job.lastResumeSave = 0;

    // step 2 open output file and mark already have chunks if file exists
    struct stat st;
    int fileExists = (stat(outPath, &st) == 0);

    FILE *outFile = fopen(outPath, fileExists ? "rb+" : "wb+");
    if (!outFile) return -1;

    if (ftruncate(fileno(outFile), (off_t)job.totalSizeBytes) != 0) { /* best-effort */ }

    // load resume bitmap if exists (supports gaps); fallback to old prefix-based inference.
    {
        size_t rTotal = 0, rChunk = 0, rCount = 0, rBytes = 0;
        uint8_t *rBits = NULL;
        if (protoResumeLoad(job.resumePath, &rTotal, &rChunk, &rCount, &rBits, &rBytes) == 0) {
            size_t expectedBytes = protoBitmapByteCount(job.chunkCount);
            if (rTotal == job.totalSizeBytes && rChunk == job.chunkSizeBytes && rCount == job.chunkCount && rBytes == expectedBytes) {
                pthread_mutex_lock(&job.mutex);
                memcpy(job.haveBitfield, rBits, expectedBytes);
                pthread_mutex_unlock(&job.mutex);
                protoLog("peer", "GET resume: loaded bitmap from %s", job.resumePath);
            }
            free(rBits);
        } else if (fileExists) {
            size_t haveSizeBytes = (size_t)st.st_size;
            if (haveSizeBytes > job.totalSizeBytes) haveSizeBytes = job.totalSizeBytes;
            size_t haveChunks = 0;
            if (job.chunkSizeBytes) {
                if (haveSizeBytes == job.totalSizeBytes) {
                    haveChunks = job.chunkCount;
                } else {
                    haveChunks = haveSizeBytes / job.chunkSizeBytes;
                }
            }
            pthread_mutex_lock(&job.mutex);
            for (size_t i = 0; i < haveChunks && i < job.chunkCount; i++) protoBitmapSet(job.haveBitfield, i);
            pthread_mutex_unlock(&job.mutex);
            (void)protoResumeSave(job.resumePath, job.totalSizeBytes, job.chunkSizeBytes, job.chunkCount,
                                  job.haveBitfield, protoBitmapByteCount(job.chunkCount));
            protoLog("peer", "GET resume: inferred prefix chunks=%zu and wrote %s", haveChunks, job.resumePath);
        }
    }

    // step 3 start worker threads and wait
    pthread_t threads[WORKERS];
    WorkerArgs workerArgs[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        workerArgs[i].job = &job;
        workerArgs[i].outFile = outFile;
        workerArgs[i].workerId = i;
        pthread_create(&threads[i], NULL, worker, &workerArgs[i]);
    }
    for (unsigned i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);

    // best-effort save resume on exit (including interrupt)
    (void)protoResumeSave(job.resumePath, job.totalSizeBytes, job.chunkSizeBytes, job.chunkCount,
                          job.haveBitfield, protoBitmapByteCount(job.chunkCount));

    if (g_stopRequested) {
        protoLog("peer", "GET stopped by signal; resume saved: %s", job.resumePath);
        fclose(outFile);
        return -1;
    }

    // step 4 check all chunks done and print summary
    int complete = 1;
    pthread_mutex_lock(&job.mutex);
    for (size_t i = 0; i < job.chunkCount; i++) {
        if (!protoBitmapGet(job.haveBitfield, i)) { complete = 0; break; }
    }
    pthread_mutex_unlock(&job.mutex);

    pthread_mutex_lock(&job.mutex);
    protoLog("peer", "DOWNLOAD SOURCES SUMMARY (chunks/bytes by peer):");
    for (int i = 0; i < job.peerCount; i++) {
        SeedPeer *peer = &job.peers[i];
        if (peer->chunksSent == 0 && peer->bytesSent == 0) continue;
        protoLog("peer", "  %s:%d -> chunks=%zu bytes=%zu", peer->ip, peer->port, peer->chunksSent, peer->bytesSent);
    }
    pthread_mutex_unlock(&job.mutex);

    if (!complete) {
        protoLog("peer", "GET incomplete file=%s out=%s", fileName, outPath);
        fclose(outFile);
        return -1;
    }

    protoLog("peer", "GET complete file=%s out=%s", fileName, outPath);
    fclose(outFile);
    return 0;
}

// cmdSeed - make file path then call announceToTracker
static int cmdSeed(const char *trackerHost, const char *trackerPort, int listenPort,
                   const char *dataDir, const char *fileName, size_t fullSizeBytes) {
    size_t chunkSizeBytes = (size_t)PROTO_CHUNK_SIZE;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", dataDir, fileName);

    size_t localSize = 0;
    if (getFileSizeBytes(path, &localSize) != 0) {
        protoLog("peer", "SEED: missing file: %s", path);
        return -1;
    }

    size_t totalSizeBytes = fullSizeBytes;

    SeedPeer peers[MAX_PEERS];
    memset(peers, 0, sizeof peers);
    int peerCount = 0;

    // If user did not provide full size, discover metadata via tracker if present.
    if (totalSizeBytes == 0) {
        size_t trackerTotal = 0;
        size_t trackerChunk = 0;
        int qrc = queryTrackerMetaAndPeers(trackerHost, trackerPort, fileName, &trackerTotal, &trackerChunk, peers, &peerCount);
        if (qrc == 0) {
            totalSizeBytes = trackerTotal;
            chunkSizeBytes = trackerChunk;
            protoLog("peer", "SEED: tracker meta file=%s full=%zu chunk=%zu peers=%d", fileName, totalSizeBytes, chunkSizeBytes, peerCount);
        } else {
            // new file: define meta from local file size
            totalSizeBytes = localSize;
            peerCount = 0;
        }
    }

    // Build our have bitmap (resume-aware) for verification.
    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
    size_t bitfieldBytes = protoBitmapByteCount(chunkCount);
    uint8_t *haveBits = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
    if (!haveBits) { freeSeedPeers(peers, peerCount); return -1; }
    if (buildBitfieldForLocalFile(path, totalSizeBytes, chunkSizeBytes, haveBits, bitfieldBytes) != 0) {
        free(haveBits);
        freeSeedPeers(peers, peerCount);
        return -1;
    }

    // If tracker already has this file and we discovered meta from it, verify content using a few hashes.
    if (fullSizeBytes == 0 && peerCount > 0) {
        if (verifySeedAgainstExistingPeers(path, fileName, totalSizeBytes, chunkSizeBytes, haveBits, peers, peerCount) != 0) {
            free(haveBits);
            freeSeedPeers(peers, peerCount);
            return -1;
        }
    }

    free(haveBits);
    freeSeedPeers(peers, peerCount);

    return announceToTracker(trackerHost, trackerPort, listenPort, fileName, path, chunkSizeBytes, totalSizeBytes);
}

// main - read env then run serve or seed or get or default mode
int main(int argc, char **argv) {
    const char *trackerHost = getenv("TRACKER_HOST");
    const char *trackerPort = getenv("TRACKER_PORT");
    const char *listenPort = getenv("LISTEN_PORT");
    const char *dataDir = getenv("DATA_DIR");
    if (!trackerHost) trackerHost = "t_tracker";
    if (!trackerPort) trackerPort = "9000";
    if (!listenPort) listenPort = "10001";
    if (!dataDir) dataDir = "/data";

    if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
        signal(SIGINT, handleStopSignal);
        signal(SIGTERM, handleStopSignal);
        const char *args[2] = { listenPort, dataDir };
        pthread_t t;
        pthread_create(&t, NULL, serveThread, (void*)args);
        pthread_join(t, NULL);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "seed") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: tpeer seed <file> [full_size_bytes]\n"); return 2; }
        signal(SIGINT, handleStopSignal);
        signal(SIGTERM, handleStopSignal);
        size_t fullSizeBytes = 0;
        if (argc >= 4) fullSizeBytes = (size_t)strtoull(argv[3], NULL, 10);
        int listenPortNumber = atoi(listenPort);
        return cmdSeed(trackerHost, trackerPort, listenPortNumber, dataDir, argv[2], fullSizeBytes) == 0 ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: tpeer get <file> <outpath>\n"); return 2; }
        signal(SIGINT, handleStopSignal);
        signal(SIGTERM, handleStopSignal);
        return cmdGet(trackerHost, trackerPort, argv[2], argv[3]) == 0 ? 0 : 1;
    }

    const char *seedFile = getenv("SEED_FILE");
    pthread_t serverThread;
    const char *args[2] = { listenPort, dataDir };
    pthread_create(&serverThread, NULL, serveThread, (void*)args);

    if (seedFile && seedFile[0]) {
        int listenPortNumber = atoi(listenPort);
        for (;;) {
            cmdSeed(trackerHost, trackerPort, listenPortNumber, dataDir, seedFile, 0);
            sleep(2);
        }
    }

    pthread_join(serverThread, NULL);
    return 0;
}
