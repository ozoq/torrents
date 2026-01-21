#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TORRENTS_DIR="."
DATA_DIR="$TORRENTS_DIR/_data"

COMPOSE_FILE="$TORRENTS_DIR/docker-compose.yaml"

fail() { echo "TEST FAIL: $*" >&2; exit 1; }

cleanAll() {
  (cd "$TORRENTS_DIR" && docker compose -f "$COMPOSE_FILE" down -v --remove-orphans) >/dev/null 2>&1 || true
}

buildImages() {
  (cd "$TORRENTS_DIR" && docker compose -f "$COMPOSE_FILE" build) >/dev/null
}

prepareData() {
  local mode="$1"
  local split="$2"

  rm -rf "$DATA_DIR"
  mkdir -p "$DATA_DIR/peer1" "$DATA_DIR/peer2" "$DATA_DIR/peer3"

  python3 "$ROOT_DIR/tests/make-test-data.py" \
    --out "$DATA_DIR" \
    --file example.bin \
    --size-bytes 1048576 \
    --split "$split" \
    --mode "$mode"
}

upBase() {
  (cd "$TORRENTS_DIR" && docker compose -f "$COMPOSE_FILE" up -d tracker peer1 peer2 peer3) >/dev/null
  sleep 1
}

logsToFile() {
  local name="$1"
  local out="$2"
  docker logs "$name" > "$out" 2>&1 || true
}

dockerExec() {
  local container="$1"; shift
  docker exec "$container" "$@"
}

sha256OfFileInContainer() {
  local container="$1"
  local path="$2"
  dockerExec "$container" sh -lc "sha256sum '$path' | awk '{print \$1}'"
}

assertFileHashEquals() {
  local seedContainer="$1"
  local seedPath="$2"
  local outContainer="$3"
  local outPath="$4"

  local seedHash
  local outHash
  seedHash="$(sha256OfFileInContainer "$seedContainer" "$seedPath")"
  outHash="$(sha256OfFileInContainer "$outContainer" "$outPath")"
  [ "$seedHash" = "$outHash" ] || fail "hash mismatch"
}

ensureNetwork() {
  docker network inspect psinet >/dev/null 2>&1 || docker network create psinet >/dev/null
}

runGetCapture() {
  local outFile="$1"; shift
  local container="$1"; shift
  local fileName="$1"; shift
  local outPath="$1"; shift

  mkdir -p "$ROOT_DIR/tests/_logs"
  set +e
  dockerExec "$container" sh -lc "/app/tpeer get '$fileName' '$outPath'" >"$outFile" 2>&1
  local rc=$?
  set -e
  return $rc
}

testHappyPathSingleSeed() {
  echo "== testHappyPathSingleSeed =="
  cleanAll
  prepareData singleSeed 0
  buildImages
  upBase

  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  sleep 1

  dockerExec t_peer3 sh -lc "/app/tpeer get example.bin /data/out.bin" >/dev/null || fail "download failed"

  dockerExec t_peer3 sh -lc "test \$(stat -c%s /data/out.bin) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals t_peer1 /data/example.bin t_peer3 /data/out.bin

  echo "testHappyPathSingleSeed OK"
}

testHappyPathMultiSeed() {
  echo "== testHappyPathMultiSeed =="
  cleanAll
  ensureNetwork
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  dockerExec t_peer2 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  sleep 1

  dockerExec t_peer3 sh -lc "/app/tpeer get example.bin /data/out.bin" >/dev/null || fail "download failed"

  dockerExec t_peer3 sh -lc "test \$(stat -c%s /data/out.bin) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals t_peer1 /data/example.bin t_peer3 /data/out.bin

  echo "testHappyPathMultiSeed OK"
}

testMetaMismatch() {
  echo "== testMetaMismatch =="
  cleanAll
  ensureNetwork
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  dockerExec t_peer2 sh -lc "/app/tpeer seed example.bin 999999" >/dev/null 2>&1 || true

  mkdir -p "$ROOT_DIR/tests/_logs"
  logsToFile t_tracker "$ROOT_DIR/tests/_logs/tracker_metaMismatch.log"

  grep -q "meta mismatch" "$ROOT_DIR/tests/_logs/tracker_metaMismatch.log" || fail "expected meta mismatch log not found"
  echo "testMetaMismatch OK"
}

testBadBitmap() {
  echo "== testBadBitmap =="
  cleanAll
  ensureNetwork
  prepareData singleSeed 0
  buildImages
  upBase

  # use a small netcat container on same network and send a broken ANNOUNCE (bitmap length wrong)
  docker run --rm --network psinet alpine:3.19 sh -lc "
    apk add --no-cache netcat-openbsd >/dev/null
    printf 'ANNOUNCE 5555 example.bin 1048576 1048576 4096 deadbeef\n' | nc -w 2 t_tracker 9000
  " > "$ROOT_DIR/tests/_badBitmap.out" 2>&1 || true

  mkdir -p "$ROOT_DIR/tests/_logs"
  logsToFile t_tracker "$ROOT_DIR/tests/_logs/tracker_badBitmap.log"

  grep -q "ERR bad_bitmap" "$ROOT_DIR/tests/_badBitmap.out" || fail "expected ERR bad_bitmap reply not found"
  echo "testBadBitmap OK"
}

testMissingChunk() {
  echo "== testMissingChunk =="
  cleanAll
  ensureNetwork

  # only 1 byte existing, so only chunk 0 is "have"
  # many chunks will be missing for all peers
  prepareData missingChunk 1

  buildImages
  upBase

  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  dockerExec t_peer2 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  sleep 1

  local outLog="$ROOT_DIR/tests/_logs/peer3_missingChunk.log"
  if runGetCapture "$outLog" t_peer3 example.bin /data/out.bin; then
    fail "expected download to fail"
  fi

  grep -q "GET failed: missing chunk" "$outLog" || fail "expected missing chunk log not found"
  echo "testMissingChunk OK"
}

testResumeDownload() {
  echo "== testResumeDownload =="
  cleanAll
  ensureNetwork
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  dockerExec t_peer2 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  sleep 1

  mkdir -p "$ROOT_DIR/tests/_tmp"
  docker cp t_peer1:/data/example.bin "$ROOT_DIR/tests/_tmp/example.bin"
  docker cp "$ROOT_DIR/tests/_tmp/example.bin" t_peer3:/data/example.bin
  rm -f "$ROOT_DIR/tests/_tmp/example.bin"

  dockerExec t_peer3 sh -lc "rm -f /data/out.bin"
  dockerExec t_peer3 sh -lc "dd if=/data/example.bin of=/data/out.bin bs=1 count=200000 status=none" >/dev/null

  local outLog="$ROOT_DIR/tests/_logs/peer3_resume.log"
  runGetCapture "$outLog" t_peer3 example.bin /data/out.bin || fail "resume download failed"

  grep -q "GET resume" "$outLog" || fail "expected resume log not found"
  dockerExec t_peer3 sh -lc "test \$(stat -c%s /data/out.bin) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals t_peer1 /data/example.bin t_peer3 /data/out.bin
  echo "testResumeDownload OK"
}

testBadDataSizeFromSeed() {
  echo "== testBadDataSizeFromSeed =="
  cleanAll
  ensureNetwork
  prepareData singleSeed 0
  buildImages
  upBase

  # start fake seed server as separate container on psinet
  docker run -d --rm --name t_fake_seed --network psinet \
    -v "$ROOT_DIR/tests:/tests:ro" python:3.11-alpine \
    python /tests/fake-seed.py 10009 >/dev/null

  # announce fake seed with bitmap that claims it has everything (all 1 bits)
  # it is ok if it is not perfect, we just want client to try it and then reject bad DATA size
  docker run --rm --network psinet alpine:3.19 sh -lc "
    apk add --no-cache netcat-openbsd >/dev/null
    printf 'ANNOUNCE 10009 example.bin 1048576 1048576 4096 ' > /tmp/a
    # bitmap bytes for 1048576/4096=256 chunks => 32 bytes => 64 hex chars
    printf 'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n' >> /tmp/a
    cat /tmp/a | nc -w 2 t_tracker 9000
  " >/dev/null 2>&1 || true

  # also announce real seed so download can still finish
  dockerExec t_peer1 sh -lc "/app/tpeer seed example.bin 1048576" >/dev/null
  sleep 1

  dockerExec t_peer3 sh -lc "/app/tpeer get example.bin /data/out.bin" >/dev/null || fail "download failed"

  # stop fake seed
  docker rm -f t_fake_seed >/dev/null 2>&1 || true

  dockerExec t_peer3 sh -lc "test \$(stat -c%s /data/out.bin) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals t_peer1 /data/example.bin t_peer3 /data/out.bin

  echo "testBadDataSizeFromSeed OK"
}

testNotFoundQuery() {
  echo "== testNotFoundQuery =="
  cleanAll
  ensureNetwork
  prepareData multiSeed 400000
  buildImages
  upBase

  local outLog="$ROOT_DIR/tests/_logs/peer3_notFound.log"
  if runGetCapture "$outLog" t_peer3 missing.bin /data/out.bin; then
    fail "expected get to fail for missing file"
  fi

  grep -q "GET: query failed" "$outLog" || fail "expected query failed log not found"
  echo "testNotFoundQuery OK"
}

main() {
  testHappyPathSingleSeed
  testHappyPathMultiSeed
  testMetaMismatch
  testBadBitmap
  testMissingChunk
  testResumeDownload
  testBadDataSizeFromSeed
  testNotFoundQuery
  echo "ALL TESTS OK"
}

main "$@"
