# torrents (PSI) - minimal torrent-like system

## Components

- `tracker` (coordinator): keeps `(file -> peers + chunk bitmap)` in memory
- `peer`: serves chunks over TCP and downloads chunks from other peers

## Protocol (newline-delimited text)

Tracker:

- `ANNOUNCE <peer_port> <file> <size> <chunk_size> <hex_bitmap>\n`
- `QUERY <file>\n`
  Replies:
- `OK\n` or `ERR <msg>\n`
- `PEERS <file> <size> <chunk_size> <n>\n`
  then `P <ip> <port> <hex_bitmap>\n` repeated `n` times

Peer:

- `GET <file> <chunk_index>\n`
  Reply:
- `DATA <nbytes>\n` followed by raw bytes

## Demo (docker)

1. Create/ensure external network:

   - `docker network create psinet` (skip if exists)

2. Put a seed file into peer1 folder:

   - place `example.bin` into `torrents/_data/peer1/`

3. Start:

   - `docker compose up --build`

4. In another terminal, download from peer2:

   - `docker exec -it t_peer2 /app/tpeer get example.bin /data/example.bin`

5. Verify on peer2:
   - `docker exec -it t_peer2 ls -l /data`

## Watching logs (tracker + peers)

In one terminal:

- `docker compose up --build`

In another terminal (live logs from all components):

- `docker compose logs -f tracker peer1 peer2 peer3`

Then run the download (in a third terminal) and watch logs:

- `docker exec -it t_peer2 /app/tpeer get example.bin /data/example.bin`

Tip: single service logs:

- `docker compose logs -f tracker`
- `docker compose logs -f peer1`

## Notes / common pitfalls

- The tracker is in-memory: it knows a file only after a peer sends `ANNOUNCE`.
- To seed in docker, set `SEED_FILE=example.bin` for a peer (e.g. peer1) or run:
  - `docker exec -it t_peer1 /app/tpeer seed example.bin`
