#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MASTER="${ROOT_DIR}/master.bin"

OUT1_DIR="${ROOT_DIR}/_data/peer1"
OUT2_DIR="${ROOT_DIR}/_data/peer2"
OUT1="${OUT1_DIR}/example.bin"
OUT2="${OUT2_DIR}/example.bin"

# T_CHUNK_SIZE in common/proto.h
CHUNK=4096

PEER1_BYTES=$((16 * 1024))
PEER2_BYTES=$((128 * 1024))

if [[ ! -f "$MASTER" ]]; then
  echo "missing master file: $MASTER" >&2
  exit 1
fi

mkdir -p "$OUT1_DIR" "$OUT2_DIR"

align_down() { local n="$1"; echo $(( (n / CHUNK) * CHUNK )); }

A1="$(align_down "$PEER1_BYTES")"
A2="$(align_down "$PEER2_BYTES")"

if (( A1 <= 0 || A2 <= 0 )); then
  echo "aligned sizes must be > 0 (A1=$A1 A2=$A2)" >&2
  exit 1
fi

if (( A2 <= A1 )); then
  echo "PEER2 must be larger than PEER1 after alignment (A1=$A1 A2=$A2)" >&2
  exit 1
fi

head -c "$A1" "$MASTER" > "$OUT1"
head -c "$A2" "$MASTER" > "$OUT2"

echo "ok: generated partials (chunk=$CHUNK bytes)"
ls -l "$OUT1" "$OUT2"
echo "peer1_bytes=$A1 peer2_bytes=$A2"
