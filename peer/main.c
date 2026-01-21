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
    uint8_t *bm;
    size_t bm_nbytes;

    // NEW: simple stats to prove multi-peer download
    size_t chunks_sent;
    size_t bytes_sent;
} seed;

typedef struct {
    char file[MAX_FILE];
    char outpath[512];
    size_t size;
    size_t chunk_size;
    size_t nchunks;
    seed peers[MAX_PEERS];
    int npeers;

    uint8_t *have;
    uint8_t *inflight;          // NEW: one worker per chunk at a time
    pthread_mutex_t mu;
} job;

static int file_size(const char *path, size_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out = (size_t)st.st_size;
    return 0;
}

static int build_bitmap_for_file(const char *path, size_t size, size_t chunk_size, uint8_t *bm, size_t bm_nbytes) {
    (void)bm_nbytes;

    // NEW: interpret "bytes present on disk" as "have chunks up to that point"
    // This lets you create partial seed files (truncate/copy prefix) and show multi-peer downloading.
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    size_t have_size = (size_t)st.st_size;
    if (have_size > size) have_size = size;

    // NEW: include last partial chunk if any bytes exist for it
    size_t have_chunks = 0;
    if (chunk_size) {
        have_chunks = have_size / chunk_size;
        if ((have_size % chunk_size) != 0) have_chunks += 1;
    }

    for (size_t i = 0; i < have_chunks; i++) t_bitmap_set(bm, i);
    return 0;
}

static int announce(const char *tracker_host, const char *tracker_port,
                    int peer_port, const char *file, const char *path,
                    size_t chunk_size, size_t full_size) {
    size_t have_size = 0;
    if (file_size(path, &have_size) != 0) return -1;

    if (full_size == 0) full_size = have_size;

    size_t nchunks = (full_size + chunk_size - 1)/chunk_size;
    size_t bm_nbytes = t_bitmap_nbytes(nchunks);
    uint8_t *bm = (uint8_t*)calloc(bm_nbytes ? bm_nbytes : 1, 1);
    if (!bm) return -1;

    // bitmap represents what we have on disk, capped by full_size, but encoded for full_size
    if (build_bitmap_for_file(path, full_size, chunk_size, bm, bm_nbytes) != 0) { free(bm); return -1; }

    char hex[T_MAX_LINE];
    if (t_hex_encode(bm, bm_nbytes, hex, sizeof hex) != 0) { free(bm); return -1; }
    free(bm);

    int fd = t_connect_tcp(tracker_host, tracker_port);
    if (fd < 0) { t_perr("peer", "connect tracker (announce)"); return -1; }

    char msg[T_MAX_LINE];
    // NEW: include full_size + have_size
    int n = snprintf(msg, sizeof msg, "ANNOUNCE %d %s %zu %zu %zu %s\n",
                     peer_port, file, full_size, have_size, chunk_size, hex);
    if (t_write_all(fd, msg, (size_t)n) != 0) { close(fd); return -1; }

    char line[T_MAX_LINE];
    if (t_read_line(fd, line, sizeof line) <= 0) { t_perr("peer", "read tracker (announce)"); close(fd); return -1; }
    close(fd);

    if (strncmp(line, "OK\n", 3) == 0) {
        t_log("peer", "ANNOUNCE ok file=%s full_size=%zu have_size=%zu chunk=%zu", file, full_size, have_size, chunk_size);
        return 0;
    }
    t_log("peer", "ANNOUNCE failed: %s", line);
    return -1;
}

static int query(job *j, const char *tracker_host, const char *tracker_port) {
    int fd = t_connect_tcp(tracker_host, tracker_port);
    if (fd < 0) { t_perr("peer", "connect tracker (query)"); return -1; }

    char q[T_MAX_LINE];
    int qn = snprintf(q, sizeof q, "QUERY %s\n", j->file);
    if (t_write_all(fd, q, (size_t)qn) != 0) { close(fd); return -1; }

    char line[T_MAX_LINE];
    if (t_read_line(fd, line, sizeof line) <= 0) { t_perr("peer", "read tracker (query hdr)"); close(fd); return -1; }
    if (strncmp(line, "PEERS ", 6) != 0) { t_log("peer", "QUERY unexpected: %s", line); close(fd); return -1; }

    int npeers = 0;
    if (sscanf(line, "PEERS %*s %zu %zu %d", &j->size, &j->chunk_size, &npeers) != 3) { close(fd); return -1; }
    j->nchunks = (j->size + j->chunk_size - 1)/j->chunk_size;
    j->have = t_bitmap_alloc(j->nchunks);
    j->inflight = t_bitmap_alloc(j->nchunks); // NEW
    pthread_mutex_init(&j->mu, NULL);

    size_t bm_nbytes = t_bitmap_nbytes(j->nchunks);

    j->npeers = 0;
    for (int i=0; i<npeers && i<MAX_PEERS; i++) {
        if (t_read_line(fd, line, sizeof line) <= 0) break;
        if (strncmp(line, "P ", 2) != 0) continue;

        char ip[64], hex[T_MAX_LINE];
        int port = 0;
        if (sscanf(line, "P %63s %d %4095s", ip, &port, hex) != 3) continue;

        seed *s = &j->peers[j->npeers++];
        memset(s, 0, sizeof *s);
        strncpy(s->ip, ip, sizeof(s->ip)-1);
        s->port = port;
        s->bm_nbytes = bm_nbytes;
        s->bm = (uint8_t*)calloc(bm_nbytes ? bm_nbytes : 1, 1);
        if (!s->bm) { close(fd); return -1; }
        if (t_hex_decode(hex, s->bm, bm_nbytes) != 0) { t_log("peer", "bad peer bitmap from tracker"); close(fd); return -1; }

        t_log("peer", "seed[%d]=%s:%d", j->npeers - 1, s->ip, s->port);
    }
    close(fd);
    t_log("peer", "QUERY file=%s -> size=%zu chunk=%zu nchunks=%zu npeers=%d",
          j->file, j->size, j->chunk_size, j->nchunks, npeers);
    return (j->npeers > 0) ? 0 : -1;
}

static int serve_one(int cfd, const char *data_dir) {
    char line[T_MAX_LINE];
    if (t_read_line(cfd, line, sizeof line) <= 0) return -1;

    char file[MAX_FILE];
    long idx = -1;
    size_t chunk_size = 0;

    // NEW protocol: GET <file> <idx> <chunk_size>\n
    if (sscanf(line, "GET %255s %ld %zu", file, &idx, &chunk_size) == 3) {
        if (idx < 0 || chunk_size == 0) {
            t_write_all(cfd, "ERR bad_get\n", 12);
            return -1;
        }
    } else {
        // Back-compat: old protocol GET <file> <idx>\n (uses T_CHUNK_SIZE)
        if (sscanf(line, "GET %255s %ld", file, &idx) != 2 || idx < 0) {
            t_log("peer", "serve: bad request: %s", line);
            t_write_all(cfd, "ERR bad_get\n", 12);
            return -1;
        }
        chunk_size = (size_t)T_CHUNK_SIZE;
    }

    t_log("peer", "serve: GET file=%s chunk=%ld chunk_size=%zu", file, idx, chunk_size);

    char path[768];
    snprintf(path, sizeof path, "%s/%s", data_dir, file);

    FILE *f = fopen(path, "rb");
    if (!f) { t_write_all(cfd, "ERR no_file\n", 12); return -1; }

    // Use requested chunk_size so offsets match the client/tracker
    if (fseek(f, (long)((size_t)idx * chunk_size), SEEK_SET) != 0) {
        fclose(f);
        t_write_all(cfd, "ERR seek\n", 9);
        return -1;
    }

    uint8_t *buf = (uint8_t*)malloc(chunk_size);
    if (!buf) { fclose(f); t_write_all(cfd, "ERR oom\n", 8); return -1; }

    size_t r = fread(buf, 1, chunk_size, f);
    fclose(f);

    t_log("peer", "serve: DATA nbytes=%zu file=%s chunk=%ld", r, file, idx);

    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "DATA %zu\n", r);
    if (t_write_all(cfd, hdr, (size_t)hn) != 0) { free(buf); return -1; }
    if (r && t_write_all(cfd, buf, r) != 0) { free(buf); return -1; }

    free(buf);
    return 0;
}

static void *serve_thread(void *arg) {
    const char **a = (const char**)arg;
    const char *listen_port = a[0];
    const char *data_dir = a[1];

    int lfd = t_listen_tcp("0.0.0.0", listen_port, BACKLOG);
    if (lfd < 0) { perror("peer listen"); return NULL; }

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        serve_one(cfd, data_dir);
        close(cfd);
    }
    return NULL;
}

static int fetch_chunk(job *j, size_t chunk_idx, seed *s, FILE *out) {
    int rc = -1;
    int fd = -1;
    uint8_t *buf = NULL;

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", s->port);

    t_log("peer", "fetch: chunk=%zu from %s:%d", chunk_idx, s->ip, s->port);

    fd = t_connect_tcp(s->ip, portstr);
    if (fd < 0) { t_perr("peer", "connect seed"); goto cleanup; }

    char msg[T_MAX_LINE];
    int mn = snprintf(msg, sizeof msg, "GET %s %zu %zu\n", j->file, chunk_idx, j->chunk_size);
    if (t_write_all(fd, msg, (size_t)mn) != 0) goto cleanup;

    char line[T_MAX_LINE];
    if (t_read_line(fd, line, sizeof line) <= 0) goto cleanup;

    size_t nbytes = 0;
    if (sscanf(line, "DATA %zu", &nbytes) != 1) goto cleanup;

    // Determine expected size for this chunk (last chunk may be smaller)
    size_t expected = j->chunk_size;
    if (chunk_idx + 1 == j->nchunks) {
        size_t rem = j->size - (chunk_idx * j->chunk_size);
        if (rem < expected) expected = rem;
    }

    // If peer returns 0 or more than expected, treat as "peer can't serve this chunk"
    if (nbytes == 0 || nbytes > expected) {
        pthread_mutex_lock(&j->mu);
        if (s->bm) s->bm[chunk_idx/8] &= (uint8_t)~(1u << (chunk_idx % 8));
        pthread_mutex_unlock(&j->mu);
        goto cleanup;
    }

    buf = (uint8_t*)malloc(nbytes);
    if (!buf) goto cleanup;

    size_t off = 0;
    while (off < nbytes) {
        ssize_t r = read(fd, buf + off, nbytes - off);
        if (r <= 0) goto cleanup;
        off += (size_t)r;
    }

    pthread_mutex_lock(&j->mu);

    if (t_bitmap_get(j->have, chunk_idx)) {
        pthread_mutex_unlock(&j->mu);
        rc = 0;
        goto cleanup;
    }

    long pos = (long)(chunk_idx * j->chunk_size);
    fseek(out, pos, SEEK_SET);
    fwrite(buf, 1, nbytes, out);
    t_bitmap_set(j->have, chunk_idx);

    s->chunks_sent += 1;
    s->bytes_sent += nbytes;

    pthread_mutex_unlock(&j->mu);

    t_log("peer", "fetch: chunk=%zu done nbytes=%zu", chunk_idx, nbytes);
    rc = 0;

cleanup:
    // ALWAYS release the inflight claim for this chunk
    pthread_mutex_lock(&j->mu);
    if (j->inflight) j->inflight[chunk_idx/8] &= (uint8_t)~(1u << (chunk_idx % 8));
    pthread_mutex_unlock(&j->mu);

    if (buf) free(buf);
    if (fd >= 0) close(fd);
    return rc;
}

static const seed *pick_seed_for(job *j, size_t chunk_idx, unsigned w) {
    // NEW: small randomization to avoid always picking the first eligible seed
    int start = 0;
    if (j->npeers > 0) start = (int)((w + (unsigned)rand()) % (unsigned)j->npeers);

    for (int k=0; k<j->npeers; k++) {
        const seed *s = &j->peers[(start + k) % j->npeers];
        if (s->bm && t_bitmap_get(s->bm, chunk_idx)) return s;
    }
    return NULL;
}

typedef struct { job *j; FILE *out; unsigned wid; } worker_arg;

static void *worker(void *arg) {
    worker_arg *wa = (worker_arg*)arg;
    job *j = wa->j;

    for (;;) {
        size_t todo = (size_t)-1;

        pthread_mutex_lock(&j->mu);
        for (size_t i=0; i<j->nchunks; i++) {
            if (t_bitmap_get(j->have, i)) continue;
            if (j->inflight && t_bitmap_get(j->inflight, i)) continue;

            // fail-fast: if no peer advertises this chunk, don't spin forever
            int any = 0;
            for (int p=0; p<j->npeers; p++) {
                if (j->peers[p].bm && t_bitmap_get(j->peers[p].bm, i)) { any = 1; break; }
            }
            if (!any) { todo = i; break; } // picked as "impossible" marker
            if (j->inflight) t_bitmap_set(j->inflight, i);
            todo = i;
            break;
        }
        pthread_mutex_unlock(&j->mu);

        if (todo == (size_t)-1) break;

        // If we found an "impossible" chunk (no peer has it), stop the whole download.
        pthread_mutex_lock(&j->mu);
        int any = 0;
        for (int p=0; p<j->npeers; p++) {
            if (j->peers[p].bm && t_bitmap_get(j->peers[p].bm, todo)) { any = 1; break; }
        }
        pthread_mutex_unlock(&j->mu);
        if (!any) {
            t_log("peer", "GET failed: missing chunk %zu (no peer has it)", todo);
            return NULL;
        }

        const seed *cs = pick_seed_for(j, todo, wa->wid);
        if (!cs) { usleep(50*1000); continue; }

        (void)fetch_chunk(j, todo, (seed*)cs, wa->out);
    }
    return NULL;
}

static int cmd_get(const char *tracker_host, const char *tracker_port, const char *file, const char *outpath) {
    t_log("peer", "GET start file=%s out=%s tracker=%s:%s", file, outpath, tracker_host, tracker_port);

    // NEW: seed RNG for pick_seed_for()
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    job j;
    memset(&j, 0, sizeof j);
    strncpy(j.file, file, sizeof(j.file)-1);
    strncpy(j.outpath, outpath, sizeof(j.outpath)-1);

    if (query(&j, tracker_host, tracker_port) != 0) { t_log("peer", "GET: query failed"); return -1; }

    FILE *out = fopen(outpath, "wb+");
    if (!out) return -1;
    if (ftruncate(fileno(out), (off_t)j.size) != 0) { /* best-effort */ }

    pthread_t th[WORKERS];
    worker_arg wa[WORKERS];
    for (unsigned i=0; i<WORKERS; i++) {
        wa[i].j = &j; wa[i].out = out; wa[i].wid = i;
        pthread_create(&th[i], NULL, worker, &wa[i]);
    }
    for (unsigned i=0; i<WORKERS; i++) pthread_join(th[i], NULL);

    // NEW: verify completion (all chunks present)
    int ok = 1;
    pthread_mutex_lock(&j.mu);
    for (size_t i=0; i<j.nchunks; i++) {
        if (!t_bitmap_get(j.have, i)) { ok = 0; break; }
    }
    pthread_mutex_unlock(&j.mu);

    // NEW: print a distribution summary to prove multi-peer download
    pthread_mutex_lock(&j.mu);
    t_log("peer", "DOWNLOAD SOURCES SUMMARY (chunks/bytes by peer):");
    for (int i=0; i<j.npeers; i++) {
        seed *s = &j.peers[i];
        if (s->chunks_sent == 0 && s->bytes_sent == 0) continue;
        t_log("peer", "  %s:%d -> chunks=%zu bytes=%zu", s->ip, s->port, s->chunks_sent, s->bytes_sent);
    }
    pthread_mutex_unlock(&j.mu);

    if (!ok) {
        t_log("peer", "GET incomplete file=%s out=%s", file, outpath);
        fclose(out);
        return -1;
    }

    t_log("peer", "GET complete file=%s out=%s", file, outpath);

    fclose(out);
    return 0;
}

static int cmd_seed(const char *tracker_host, const char *tracker_port, int listen_port,
                    const char *data_dir, const char *file, size_t full_size) {
    const size_t chunk_size = (size_t)T_CHUNK_SIZE;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", data_dir, file);
    return announce(tracker_host, tracker_port, listen_port, file, path, chunk_size, full_size);
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
        return cmd_seed(tracker_host, tracker_port, lp, data_dir, argv[2], full_size) == 0 ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: tpeer get <file> <outpath>\n"); return 2; }
        return cmd_get(tracker_host, tracker_port, argv[2], argv[3]) == 0 ? 0 : 1;
    }

    // default: run server + periodic announce of all files in /data (simple: announce one optional SEED_FILE)
    const char *seed_file = getenv("SEED_FILE");
    pthread_t t;
    const char *args[2] = { listen_port, data_dir };
    pthread_create(&t, NULL, serve_thread, (void*)args);

    if (seed_file && seed_file[0]) {
        int lp = atoi(listen_port);
        for (;;) {
            cmd_seed(tracker_host, tracker_port, lp, data_dir, seed_file, 0);
            sleep(2);
        }
    }

    pthread_join(t, NULL);
    return 0;
}
