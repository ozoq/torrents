#include "../common/proto.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>   // for off_t
#include <sys/stat.h>    // for ftruncate (some libcs)
#include <fcntl.h>       // may be needed on some setups
#include <time.h>        // add for simple randomization

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
} DownloadJob;

static void clearBit(uint8_t *bitfield, size_t bitIndex) {
    bitfield[bitIndex / 8] &= (uint8_t)~(1u << (bitIndex % 8));
}

static int getFileSizeBytes(const char *path, size_t *outSizeBytes) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *outSizeBytes = (size_t)st.st_size;
    return 0;
}

static int buildBitfieldForLocalFile(const char *path, size_t totalSizeBytes, size_t chunkSizeBytes,
                                     uint8_t *outBitfield, size_t outBitfieldBytes) {
    (void)outBitfieldBytes;

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    size_t haveSizeBytes = (size_t)st.st_size;
    if (haveSizeBytes > totalSizeBytes) haveSizeBytes = totalSizeBytes;

    size_t haveChunks = 0;
    if (chunkSizeBytes) {
        haveChunks = haveSizeBytes / chunkSizeBytes;
        if ((haveSizeBytes % chunkSizeBytes) != 0) haveChunks += 1;
    }

    for (size_t i = 0; i < haveChunks; i++) tBitmapSet(outBitfield, i);
    return 0;
}

static int announceToTracker(const char *trackerHost, const char *trackerPort,
                            int peerPort, const char *fileName, const char *path,
                            size_t chunkSizeBytes, size_t totalSizeBytes) {
    size_t haveSizeBytes = 0;
    if (getFileSizeBytes(path, &haveSizeBytes) != 0) return -1;

    if (totalSizeBytes == 0) totalSizeBytes = haveSizeBytes;

    size_t chunkCount = (totalSizeBytes + chunkSizeBytes - 1) / chunkSizeBytes;
    size_t bitfieldBytes = tBitmapByteCount(chunkCount);

    uint8_t *bitfield = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
    if (!bitfield) return -1;

    if (buildBitfieldForLocalFile(path, totalSizeBytes, chunkSizeBytes, bitfield, bitfieldBytes) != 0) {
        free(bitfield);
        return -1;
    }

    char hexBitfield[T_MAX_LINE];
    if (tHexEncode(bitfield, bitfieldBytes, hexBitfield, sizeof hexBitfield) != 0) {
        free(bitfield);
        return -1;
    }
    free(bitfield);

    int fd = tConnectTcp(trackerHost, trackerPort);
    if (fd < 0) { tPerr("peer", "connect tracker (announce)"); return -1; }

    char msg[T_MAX_LINE];
    int msgBytes = snprintf(msg, sizeof msg, "ANNOUNCE %d %s %zu %zu %zu %s\n",
                            peerPort, fileName, totalSizeBytes, haveSizeBytes, chunkSizeBytes, hexBitfield);
    if (tWriteAll(fd, msg, (size_t)msgBytes) != 0) { close(fd); return -1; }

    char line[T_MAX_LINE];
    if (tReadLine(fd, line, sizeof line) <= 0) { tPerr("peer", "read tracker (announce)"); close(fd); return -1; }
    close(fd);

    if (strncmp(line, "OK\n", 3) == 0) {
        tLog("peer", "ANNOUNCE ok file=%s full_size=%zu have_size=%zu chunk=%zu",
              fileName, totalSizeBytes, haveSizeBytes, chunkSizeBytes);
        return 0;
    }

    tLog("peer", "ANNOUNCE failed: %s", line);
    return -1;
}

static int queryTracker(DownloadJob *job, const char *trackerHost, const char *trackerPort) {
    int fd = tConnectTcp(trackerHost, trackerPort);
    if (fd < 0) { tPerr("peer", "connect tracker (query)"); return -1; }

    char queryLine[T_MAX_LINE];
    int queryBytes = snprintf(queryLine, sizeof queryLine, "QUERY %s\n", job->fileName);
    if (tWriteAll(fd, queryLine, (size_t)queryBytes) != 0) { close(fd); return -1; }

    char line[T_MAX_LINE];
    if (tReadLine(fd, line, sizeof line) <= 0) { tPerr("peer", "read tracker (query hdr)"); close(fd); return -1; }
    if (strncmp(line, "PEERS ", 6) != 0) { tLog("peer", "QUERY unexpected: %s", line); close(fd); return -1; }

    int advertisedPeerCount = 0;
    if (sscanf(line, "PEERS %*s %zu %zu %d", &job->totalSizeBytes, &job->chunkSizeBytes, &advertisedPeerCount) != 3) {
        close(fd);
        return -1;
    }

    job->chunkCount = (job->totalSizeBytes + job->chunkSizeBytes - 1) / job->chunkSizeBytes;
    job->haveBitfield = tBitmapAlloc(job->chunkCount);
    job->inFlightBitfield = tBitmapAlloc(job->chunkCount);
    pthread_mutex_init(&job->mutex, NULL);

    size_t bitfieldBytes = tBitmapByteCount(job->chunkCount);

    job->peerCount = 0;
    for (int i = 0; i < advertisedPeerCount && i < MAX_PEERS; i++) {
        if (tReadLine(fd, line, sizeof line) <= 0) break;
        if (strncmp(line, "P ", 2) != 0) continue;

        char ip[64];
        char hexBitfield[T_MAX_LINE];
        int port = 0;
        if (sscanf(line, "P %63s %d %4095s", ip, &port, hexBitfield) != 3) continue;

        SeedPeer *peer = &job->peers[job->peerCount++];
        memset(peer, 0, sizeof *peer);
        strncpy(peer->ip, ip, sizeof(peer->ip) - 1);
        peer->port = port;

        peer->bitfieldBytes = bitfieldBytes;
        peer->bitfield = (uint8_t*)calloc(bitfieldBytes ? bitfieldBytes : 1, 1);
        if (!peer->bitfield) { close(fd); return -1; }

        if (tHexDecode(hexBitfield, peer->bitfield, bitfieldBytes) != 0) {
            tLog("peer", "bad peer bitmap from tracker");
            close(fd);
            return -1;
        }

        tLog("peer", "seed[%d]=%s:%d", job->peerCount - 1, peer->ip, peer->port);
    }

    close(fd);
    tLog("peer", "QUERY file=%s -> size=%zu chunk=%zu nchunks=%zu npeers=%d",
          job->fileName, job->totalSizeBytes, job->chunkSizeBytes, job->chunkCount, advertisedPeerCount);

    return (job->peerCount > 0) ? 0 : -1;
}

static int serve_one(int cfd, const char *data_dir) {
    char line[T_MAX_LINE];
    if (tReadLine(cfd, line, sizeof line) <= 0) return -1;

    char file[MAX_FILE];
    long idx = -1;
    size_t chunk_size = 0;

    // NEW protocol: GET <file> <idx> <chunk_size>\n
    if (sscanf(line, "GET %255s %ld %zu", file, &idx, &chunk_size) == 3) {
        if (idx < 0 || chunk_size == 0) {
            tWriteAll(cfd, "ERR bad_get\n", 12);
            return -1;
        }
    } else {
        // Back-compat: old protocol GET <file> <idx>\n (uses T_CHUNK_SIZE)
        if (sscanf(line, "GET %255s %ld", file, &idx) != 2 || idx < 0) {
            tLog("peer", "serve: bad request: %s", line);
            tWriteAll(cfd, "ERR bad_get\n", 12);
            return -1;
        }
        chunk_size = (size_t)T_CHUNK_SIZE;
    }

    tLog("peer", "serve: GET file=%s chunk=%ld chunk_size=%zu", file, idx, chunk_size);

    char path[768];
    snprintf(path, sizeof path, "%s/%s", data_dir, file);

    FILE *f = fopen(path, "rb");
    if (!f) { tWriteAll(cfd, "ERR no_file\n", 12); return -1; }

    // Use requested chunk_size so offsets match the client/tracker
    if (fseek(f, (long)((size_t)idx * chunk_size), SEEK_SET) != 0) {
        fclose(f);
        tWriteAll(cfd, "ERR seek\n", 9);
        return -1;
    }

    uint8_t *buf = (uint8_t*)malloc(chunk_size);
    if (!buf) { fclose(f); tWriteAll(cfd, "ERR oom\n", 8); return -1; }

    size_t r = fread(buf, 1, chunk_size, f);
    fclose(f);

    tLog("peer", "serve: DATA nbytes=%zu file=%s chunk=%ld", r, file, idx);

    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "DATA %zu\n", r);
    if (tWriteAll(cfd, hdr, (size_t)hn) != 0) { free(buf); return -1; }
    if (r && tWriteAll(cfd, buf, r) != 0) { free(buf); return -1; }

    free(buf);
    return 0;
}

static void *serve_thread(void *arg) {
    const char **a = (const char**)arg;
    const char *listen_port = a[0];
    const char *data_dir = a[1];

    int lfd = tListenTcp("0.0.0.0", listen_port, BACKLOG);
    if (lfd < 0) { perror("peer listen"); return NULL; }

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        serve_one(cfd, data_dir);
        close(cfd);
    }
    return NULL;
}

static int fetchChunk(DownloadJob *job, size_t chunkIndex, SeedPeer *peer, FILE *outFile) {
    int result = -1;
    int fd = -1;
    uint8_t *buffer = NULL;

    char portString[16];
    snprintf(portString, sizeof portString, "%d", peer->port);

    tLog("peer", "fetch: chunk=%zu from %s:%d", chunkIndex, peer->ip, peer->port);

    fd = tConnectTcp(peer->ip, portString);
    if (fd < 0) { tPerr("peer", "connect seed"); goto cleanup; }

    char request[T_MAX_LINE];
    int requestBytes = snprintf(request, sizeof request, "GET %s %zu %zu\n", job->fileName, chunkIndex, job->chunkSizeBytes);
    if (tWriteAll(fd, request, (size_t)requestBytes) != 0) goto cleanup;

    char line[T_MAX_LINE];
    if (tReadLine(fd, line, sizeof line) <= 0) goto cleanup;

    size_t receivedBytes = 0;
    if (sscanf(line, "DATA %zu", &receivedBytes) != 1) goto cleanup;

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

    pthread_mutex_lock(&job->mutex);

    if (tBitmapGet(job->haveBitfield, chunkIndex)) {
        pthread_mutex_unlock(&job->mutex);
        result = 0;
        goto cleanup;
    }

    long fileOffset = (long)(chunkIndex * job->chunkSizeBytes);
    fseek(outFile, fileOffset, SEEK_SET);
    fwrite(buffer, 1, receivedBytes, outFile);

    tBitmapSet(job->haveBitfield, chunkIndex);

    peer->chunksSent += 1;
    peer->bytesSent += receivedBytes;

    pthread_mutex_unlock(&job->mutex);

    tLog("peer", "fetch: chunk=%zu done nbytes=%zu", chunkIndex, receivedBytes);
    result = 0;

cleanup:
    pthread_mutex_lock(&job->mutex);
    if (job->inFlightBitfield) clearBit(job->inFlightBitfield, chunkIndex);
    pthread_mutex_unlock(&job->mutex);

    if (buffer) free(buffer);
    if (fd >= 0) close(fd);
    return result;
}

static const SeedPeer *pickSeedForChunk(DownloadJob *job, size_t chunkIndex, unsigned workerId) {
    int startIndex = 0;
    if (job->peerCount > 0) startIndex = (int)((workerId + (unsigned)rand()) % (unsigned)job->peerCount);

    for (int i = 0; i < job->peerCount; i++) {
        const SeedPeer *peer = &job->peers[(startIndex + i) % job->peerCount];
        if (peer->bitfield && tBitmapGet(peer->bitfield, chunkIndex)) return peer;
    }
    return NULL;
}

typedef struct { DownloadJob *job; FILE *outFile; unsigned workerId; } WorkerArgs;

static void *worker(void *arg) {
    WorkerArgs *args = (WorkerArgs*)arg;
    DownloadJob *job = args->job;

    for (;;) {
        size_t chosenChunk = (size_t)-1;

        pthread_mutex_lock(&job->mutex);
        for (size_t i = 0; i < job->chunkCount; i++) {
            if (tBitmapGet(job->haveBitfield, i)) continue;
            if (job->inFlightBitfield && tBitmapGet(job->inFlightBitfield, i)) continue;

            int anyPeerHasChunk = 0;
            for (int p = 0; p < job->peerCount; p++) {
                if (job->peers[p].bitfield && tBitmapGet(job->peers[p].bitfield, i)) { anyPeerHasChunk = 1; break; }
            }

            if (!anyPeerHasChunk) { chosenChunk = i; break; }
            if (job->inFlightBitfield) tBitmapSet(job->inFlightBitfield, i);

            chosenChunk = i;
            break;
        }
        pthread_mutex_unlock(&job->mutex);

        if (chosenChunk == (size_t)-1) break;

        pthread_mutex_lock(&job->mutex);
        int anyPeerHasChunk = 0;
        for (int p = 0; p < job->peerCount; p++) {
            if (job->peers[p].bitfield && tBitmapGet(job->peers[p].bitfield, chosenChunk)) { anyPeerHasChunk = 1; break; }
        }
        pthread_mutex_unlock(&job->mutex);

        if (!anyPeerHasChunk) {
            tLog("peer", "GET failed: missing chunk %zu (no peer has it)", chosenChunk);
            return NULL;
        }

        const SeedPeer *chosenPeer = pickSeedForChunk(job, chosenChunk, args->workerId);
        if (!chosenPeer) { usleep(50 * 1000); continue; }

        (void)fetchChunk(job, chosenChunk, (SeedPeer*)chosenPeer, args->outFile);
    }
    return NULL;
}

static int cmdGet(const char *trackerHost, const char *trackerPort, const char *fileName, const char *outPath) {
    tLog("peer", "GET start file=%s out=%s tracker=%s:%s", fileName, outPath, trackerHost, trackerPort);

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    DownloadJob job;
    memset(&job, 0, sizeof job);
    strncpy(job.fileName, fileName, sizeof(job.fileName) - 1);
    strncpy(job.outputPath, outPath, sizeof(job.outputPath) - 1);

    if (queryTracker(&job, trackerHost, trackerPort) != 0) { tLog("peer", "GET: query failed"); return -1; }

    struct stat st;
    int fileExists = (stat(outPath, &st) == 0);

    FILE *outFile = fopen(outPath, fileExists ? "rb+" : "wb+");
    if (!outFile) return -1;

    if (ftruncate(fileno(outFile), (off_t)job.totalSizeBytes) != 0) { /* best-effort */ }

    if (fileExists) {
        size_t haveSizeBytes = (size_t)st.st_size;
        if (haveSizeBytes > job.totalSizeBytes) haveSizeBytes = job.totalSizeBytes;

        size_t haveChunks = 0;
        if (job.chunkSizeBytes) {
            haveChunks = haveSizeBytes / job.chunkSizeBytes;
            if ((haveSizeBytes % job.chunkSizeBytes) != 0) haveChunks += 1;
        }

        pthread_mutex_lock(&job.mutex);
        for (size_t i = 0; i < haveChunks && i < job.chunkCount; i++) tBitmapSet(job.haveBitfield, i);
        pthread_mutex_unlock(&job.mutex);

        tLog("peer", "GET resume: already_have_bytes=%zu already_have_chunks=%zu", haveSizeBytes, haveChunks);
    }

    pthread_t threads[WORKERS];
    WorkerArgs workerArgs[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        workerArgs[i].job = &job;
        workerArgs[i].outFile = outFile;
        workerArgs[i].workerId = i;
        pthread_create(&threads[i], NULL, worker, &workerArgs[i]);
    }
    for (unsigned i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);

    int complete = 1;
    pthread_mutex_lock(&job.mutex);
    for (size_t i = 0; i < job.chunkCount; i++) {
        if (!tBitmapGet(job.haveBitfield, i)) { complete = 0; break; }
    }
    pthread_mutex_unlock(&job.mutex);

    pthread_mutex_lock(&job.mutex);
    tLog("peer", "DOWNLOAD SOURCES SUMMARY (chunks/bytes by peer):");
    for (int i = 0; i < job.peerCount; i++) {
        SeedPeer *peer = &job.peers[i];
        if (peer->chunksSent == 0 && peer->bytesSent == 0) continue;
        tLog("peer", "  %s:%d -> chunks=%zu bytes=%zu", peer->ip, peer->port, peer->chunksSent, peer->bytesSent);
    }
    pthread_mutex_unlock(&job.mutex);

    if (!complete) {
        tLog("peer", "GET incomplete file=%s out=%s", fileName, outPath);
        fclose(outFile);
        return -1;
    }

    tLog("peer", "GET complete file=%s out=%s", fileName, outPath);
    fclose(outFile);
    return 0;
}

static int cmdSeed(const char *trackerHost, const char *trackerPort, int listenPort,
                   const char *dataDir, const char *fileName, size_t fullSizeBytes) {
    const size_t chunkSizeBytes = (size_t)T_CHUNK_SIZE;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", dataDir, fileName);
    return announceToTracker(trackerHost, trackerPort, listenPort, fileName, path, chunkSizeBytes, fullSizeBytes);
}

int main(int argc, char **argv) {
    const char *tracker_host = getenv("TRACKER_HOST");
    const char *tracker_port = getenv("TRACKER_PORT");
    const char *listen_port = getenv("LISTEN_PORT");
    const char *data_dir = getenv("DATA_DIR");
    if (!tracker_host) tracker_host = "t_tracker";
    if (!tracker_port) tracker_port = "9000";
    if (!listen_port) listen_port = "10001";
    if (!data_dir) data_dir = "/data";

    if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
        const char *args[2] = { listen_port, data_dir };
        pthread_t t;
        pthread_create(&t, NULL, serve_thread, (void*)args);
        pthread_join(t, NULL);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "seed") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: tpeer seed <file> [full_size_bytes]\n"); return 2; }
        size_t full_size = 0;
        if (argc >= 4) full_size = (size_t)strtoull(argv[3], NULL, 10);
        int lp = atoi(listen_port);
        return cmdSeed(tracker_host, tracker_port, lp, data_dir, argv[2], full_size) == 0 ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: tpeer get <file> <outpath>\n"); return 2; }
        return cmdGet(tracker_host, tracker_port, argv[2], argv[3]) == 0 ? 0 : 1;
    }

    // default: run server + periodic announce of all files in /data (simple: announce one optional SEED_FILE)
    const char *seed_file = getenv("SEED_FILE");
    pthread_t t;
    const char *args[2] = { listen_port, data_dir };
    pthread_create(&t, NULL, serve_thread, (void*)args);

    if (seed_file && seed_file[0]) {
        int lp = atoi(listen_port);
        for (;;) {
            cmdSeed(tracker_host, tracker_port, lp, data_dir, seed_file, 0);
            sleep(2);
        }
    }

    pthread_join(t, NULL);
    return 0;
}
