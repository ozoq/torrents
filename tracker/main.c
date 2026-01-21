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
    size_t bm_nbytes;
    uint8_t *bm;
} peer_info;

typedef struct {
    char name[MAX_NAME];
    size_t size;
    size_t chunk_size;
    size_t nchunks;
    size_t bm_nbytes;
    peer_info peers[MAX_PEERS];
    int npeers;
} file_entry;

static file_entry g_files[MAX_FILES];
static int g_nfiles = 0;

static file_entry *get_file(const char *name) {
    for (int i=0; i<g_nfiles; i++) if (strcmp(g_files[i].name, name)==0) return &g_files[i];
    if (g_nfiles >= MAX_FILES) return NULL;
    file_entry *f = &g_files[g_nfiles++];
    memset(f, 0, sizeof *f);
    strncpy(f->name, name, sizeof(f->name)-1);
    return f;
}

static peer_info *get_peer(file_entry *f, const char *ip, int port) {
    for (int i=0; i<f->npeers; i++)
        if (strcmp(f->peers[i].ip, ip)==0 && f->peers[i].port==port) return &f->peers[i];
    if (f->npeers >= MAX_PEERS) return NULL;
    peer_info *p = &f->peers[f->npeers++];
    memset(p, 0, sizeof *p);
    strncpy(p->ip, ip, sizeof(p->ip)-1);
    p->port = port;
    return p;
}

static void handle_client(int cfd, const char *rip) {
    char line[T_MAX_LINE];
    int n = t_read_line(cfd, line, sizeof line);
    if (n <= 0) return;

    // ANNOUNCE <peer_port> <file> <full_size> <have_size> <chunk_size> <hex_bitmap>\n
    if (strncmp(line, "ANNOUNCE ", 9) == 0) {
        int peer_port = 0;
        char file[MAX_NAME];
        unsigned long long full_size = 0, have_size = 0, chunk = 0;
        char hex[T_MAX_LINE];

        if (sscanf(line, "ANNOUNCE %d %255s %llu %llu %llu %4095s",
                   &peer_port, file, &full_size, &have_size, &chunk, hex) != 6) {
            t_log("tracker", "ANNOUNCE from %s: bad announce line", rip);
            t_write_all(cfd, "ERR bad_announce\n", 17);
            return;
        }

        file_entry *f = get_file(file);
        if (!f) { t_write_all(cfd, "ERR file_table_full\n", 20); return; }

        // Canonical torrent params are (full_size, chunk_size)
        if (f->size == 0) {
            f->size = (size_t)full_size;
            f->chunk_size = (size_t)chunk;
            f->nchunks = (f->chunk_size ? (f->size + f->chunk_size - 1)/f->chunk_size : 0);
            f->bm_nbytes = t_bitmap_nbytes(f->nchunks);
        } else {
            if (f->size != (size_t)full_size || f->chunk_size != (size_t)chunk) {
                t_log("tracker", "ANNOUNCE from %s:%d file=%s: meta mismatch", rip, peer_port, file);
                t_write_all(cfd, "ERR meta_mismatch\n", 18);
                return;
            }
        }

        peer_info *p = get_peer(f, rip, peer_port);
        if (!p) { t_write_all(cfd, "ERR peer_table_full\n", 20); return; }

        if (p->bm) free(p->bm);
        p->bm_nbytes = f->bm_nbytes;
        p->bm = (uint8_t*)calloc(p->bm_nbytes ? p->bm_nbytes : 1, 1);
        if (!p->bm) { t_write_all(cfd, "ERR oom\n", 8); return; }

        if (t_hex_decode(hex, p->bm, p->bm_nbytes) != 0) {
            t_log("tracker", "ANNOUNCE from %s:%d file=%s: bad bitmap", rip, peer_port, file);
            t_write_all(cfd, "ERR bad_bitmap\n", 15);
            return;
        }

        t_log("tracker", "ANNOUNCE from %s:%d file=%s full=%zu have=%zu chunk=%zu peers_now=%d",
              rip, peer_port, f->name, f->size, (size_t)have_size, f->chunk_size, f->npeers);

        t_write_all(cfd, "OK\n", 3);
        return;
    }

    // QUERY <file>\n
    if (strncmp(line, "QUERY ", 6) == 0) {
        char file[MAX_NAME];
        if (sscanf(line, "QUERY %255s", file) != 1) {
            t_log("tracker", "QUERY from %s: bad query line", rip);
            t_write_all(cfd, "ERR bad_query\n", 14); return;
        }

        file_entry *f = NULL;
        for (int i=0; i<g_nfiles; i++) if (strcmp(g_files[i].name, file)==0) { f = &g_files[i]; break; }
        if (!f) {
            t_log("tracker", "QUERY from %s file=%s: not found", rip, file);
            t_write_all(cfd, "ERR not_found\n", 14); return;
        }

        t_log("tracker", "QUERY from %s file=%s: returning %d peers", rip, f->name, f->npeers);

        char hdr[256];
        int hdrn = snprintf(hdr, sizeof hdr, "PEERS %s %zu %zu %d\n", f->name, f->size, f->chunk_size, f->npeers);
        t_write_all(cfd, hdr, (size_t)hdrn);

        for (int i=0; i<f->npeers; i++) {
            peer_info *p = &f->peers[i];
            if (!p->bm) continue;
            char hex[T_MAX_LINE];
            if (t_hex_encode(p->bm, p->bm_nbytes, hex, sizeof hex) != 0) continue;
            char row[512 + T_MAX_LINE];
            int rn = snprintf(row, sizeof row, "P %s %d %s\n", p->ip, p->port, hex);
            t_write_all(cfd, row, (size_t)rn);
        }
        return;
    }

    t_write_all(cfd, "ERR unknown_cmd\n", 16);
}

int main(int argc, char **argv) {
    const char *port = (argc >= 2) ? argv[1] : "9000";
    int lfd = t_listen_tcp("0.0.0.0", port, BACKLOG);
    if (lfd < 0) { perror("tracker listen"); return 1; }
    fprintf(stderr, "tracker listening on :%s\n", port);
    fflush(stderr);

    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int cfd = accept(lfd, (struct sockaddr*)&sa, &sl);
        if (cfd < 0) { perror("accept"); continue; }
        char rip[64];
        inet_ntop(AF_INET, &sa.sin_addr, rip, sizeof rip);

        t_log("tracker", "client %s connected", rip);
        handle_client(cfd, rip);
        close(cfd);
    }
}
