`docker compose up --build`
`docker exec -it z78_peer2 /app/tpeer get example.bin /data/example.bin`
`docker exec -it z78_peer2 ls -l /data`
`docker compose logs -f tracker peer1 peer2 peer3`
`docker exec -it z78_peer2 /app/tpeer get example.bin /data/example.bin`
`docker compose logs -f tracker`
`docker compose logs -f peer1`
`docker exec -it z78_peer1 /app/tpeer seed example.bin`
