#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TORRENTS_DIR="."
DATA_DIR="$TORRENTS_DIR/_data"

TEST_SUFFIX=".test.tmp"
TEST_FILE="example.bin${TEST_SUFFIX}"
OUT_FILE="out.bin${TEST_SUFFIX}"
NOT_FOUND_FILE="missing.bin${TEST_SUFFIX}"

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

  mkdir -p "$DATA_DIR/peer1" "$DATA_DIR/peer2" "$DATA_DIR/peer3"

  for peer in peer1 peer2 peer3; do
    if [ -d "$DATA_DIR/$peer" ]; then
      # Remove test artifacts, including resume sidecars (e.g. `out.bin.test.tmp.resume`).
      find "$DATA_DIR/$peer" -mindepth 1 -maxdepth 1 -name "*${TEST_SUFFIX}*" -exec rm -rf -- {} +
    fi
  done

  python3 "$ROOT_DIR/tests/make-test-data.py" \
    --out "$DATA_DIR" \
    --file "$TEST_FILE" \
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

  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  sleep 1

  dockerExec z78_peer3 sh -lc "/app/tpeer get '$TEST_FILE' /data/$OUT_FILE" >/dev/null || fail "download failed"

  dockerExec z78_peer3 sh -lc "test \$(stat -c%s /data/$OUT_FILE) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals z78_peer1 "/data/$TEST_FILE" z78_peer3 "/data/$OUT_FILE"

  echo "testHappyPathSingleSeed OK"
}

testHappyPathMultiSeed() {
  echo "== testHappyPathMultiSeed =="
  cleanAll
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  dockerExec z78_peer2 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  sleep 1

  dockerExec z78_peer3 sh -lc "/app/tpeer get '$TEST_FILE' /data/$OUT_FILE" >/dev/null || fail "download failed"

  dockerExec z78_peer3 sh -lc "test \$(stat -c%s /data/$OUT_FILE) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals z78_peer1 "/data/$TEST_FILE" z78_peer3 "/data/$OUT_FILE"

  echo "testHappyPathMultiSeed OK"
}

testMetaMismatch() {
  echo "== testMetaMismatch =="
  cleanAll
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  dockerExec z78_peer2 sh -lc "/app/tpeer seed '$TEST_FILE' 999999" >/dev/null 2>&1 || true

  mkdir -p "$ROOT_DIR/tests/_logs"
  logsToFile z78_tracker "$ROOT_DIR/tests/_logs/tracker_metaMismatch.log"

  grep -q "meta mismatch" "$ROOT_DIR/tests/_logs/tracker_metaMismatch.log" || fail "expected meta mismatch log not found"
  echo "testMetaMismatch OK"
}

testBadBitmap() {
  echo "== testBadBitmap =="
  cleanAll
  prepareData singleSeed 0
  buildImages
  upBase

  # use a small netcat container on same network and send a broken ANNOUNCE (bitmap length wrong)
  docker run --rm --network z78_network alpine:3.19 sh -lc "
    apk add --no-cache netcat-openbsd >/dev/null
    printf 'ANNOUNCE 5555 ${TEST_FILE} 1048576 1048576 4096 deadbeef\n' | nc -w 2 z78_tracker 9000
  " > "$ROOT_DIR/tests/_badBitmap.out" 2>&1 || true

  mkdir -p "$ROOT_DIR/tests/_logs"
  logsToFile z78_tracker "$ROOT_DIR/tests/_logs/tracker_badBitmap.log"

  grep -q "ERR bad_bitmap" "$ROOT_DIR/tests/_badBitmap.out" || fail "expected ERR bad_bitmap reply not found"
  echo "testBadBitmap OK"
}

testMissingChunk() {
  echo "== testMissingChunk =="
  cleanAll

  # only 1 byte existing, so only chunk 0 is "have"
  # many chunks will be missing for all peers
  prepareData missingChunk 1

  buildImages
  upBase

  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  dockerExec z78_peer2 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  sleep 1

  local outLog="$ROOT_DIR/tests/_logs/peer3_missingChunk.log"
  if runGetCapture "$outLog" z78_peer3 "$TEST_FILE" "/data/$OUT_FILE"; then
    fail "expected download to fail"
  fi

  grep -q "GET failed: missing chunk" "$outLog" || fail "expected missing chunk log not found"
  echo "testMissingChunk OK"
}

testResumeDownload() {
  echo "== testResumeDownload =="
  cleanAll
  prepareData multiSeed 400000
  buildImages
  upBase

  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  dockerExec z78_peer2 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  sleep 1

  mkdir -p "$ROOT_DIR/tests/_tmp"
  docker cp "z78_peer1:/data/$TEST_FILE" "$ROOT_DIR/tests/_tmp/$TEST_FILE"
  docker cp "$ROOT_DIR/tests/_tmp/$TEST_FILE" "z78_peer3:/data/$TEST_FILE"
  rm -f "$ROOT_DIR/tests/_tmp/$TEST_FILE"

  dockerExec z78_peer3 sh -lc "rm -f /data/$OUT_FILE"
  dockerExec z78_peer3 sh -lc "dd if=/data/$TEST_FILE of=/data/$OUT_FILE bs=1 count=200000 status=none" >/dev/null

  local outLog="$ROOT_DIR/tests/_logs/peer3_resume.log"
  runGetCapture "$outLog" z78_peer3 "$TEST_FILE" "/data/$OUT_FILE" || fail "resume download failed"

  grep -q "GET resume" "$outLog" || fail "expected resume log not found"
  dockerExec z78_peer3 sh -lc "test \$(stat -c%s /data/$OUT_FILE) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals z78_peer1 "/data/$TEST_FILE" z78_peer3 "/data/$OUT_FILE"
  echo "testResumeDownload OK"
}

testBadDataSizeFromSeed() {
  echo "== testBadDataSizeFromSeed =="
  cleanAll
  prepareData singleSeed 0
  buildImages
  upBase

  # start fake seed server as separate container on z78_network
  docker run -d --rm --name z78_fake_seed --network z78_network \
    -v "$ROOT_DIR/tests:/tests:ro" python:3.11-alpine \
    python /tests/fake-seed.py 10009 >/dev/null

  # announce fake seed with bitmap that claims it has everything (all 1 bits)
  # it is ok if it is not perfect, we just want client to try it and then reject bad DATA size
  docker run --rm --network z78_network alpine:3.19 sh -lc "
    apk add --no-cache netcat-openbsd >/dev/null
    printf 'ANNOUNCE 10009 '"${TEST_FILE}"' 1048576 1048576 4096 ' > /tmp/a
    # bitmap bytes for 1048576/4096=256 chunks => 32 bytes => 64 hex chars
    printf 'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n' >> /tmp/a
    cat /tmp/a | nc -w 2 z78_tracker 9000
  " >/dev/null 2>&1 || true

  # also announce real seed so download can still finish
  dockerExec z78_peer1 sh -lc "/app/tpeer seed '$TEST_FILE' 1048576" >/dev/null
  sleep 1

  dockerExec z78_peer3 sh -lc "/app/tpeer get '$TEST_FILE' /data/$OUT_FILE" >/dev/null || fail "download failed"

  # stop fake seed
  docker rm -f z78_fake_seed >/dev/null 2>&1 || true

  dockerExec z78_peer3 sh -lc "test \$(stat -c%s /data/$OUT_FILE) -eq 1048576" || fail "out size mismatch"
  assertFileHashEquals z78_peer1 "/data/$TEST_FILE" z78_peer3 "/data/$OUT_FILE"

  echo "testBadDataSizeFromSeed OK"
}

testNotFoundQuery() {
  echo "== testNotFoundQuery =="
  cleanAll
  prepareData multiSeed 400000
  buildImages
  upBase

  local outLog="$ROOT_DIR/tests/_logs/peer3_notFound.log"
  if runGetCapture "$outLog" z78_peer3 "$NOT_FOUND_FILE" "/data/$OUT_FILE"; then
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
