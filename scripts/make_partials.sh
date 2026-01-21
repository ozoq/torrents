#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MASTER="${ROOT_DIR}/master.bin"

OUT1_DIR="${ROOT_DIR}/_data/peer1"
OUT2_DIR="${ROOT_DIR}/_data/peer2"
OUT1="${OUT1_DIR}/example.bin"
OUT2="${OUT2_DIR}/example.bin"

PEER1_BYTES=$((16 * 1024)) # 16 KiB

if [[ ! -f "$MASTER" ]]; then
  echo "missing master file: $MASTER" >&2
  exit 1
fi

mkdir -p "$OUT1_DIR" "$OUT2_DIR"

head -c "$PEER1_BYTES" "$MASTER" > "$OUT1"
cp -f "$MASTER" "$OUT2"

echo "ok: generated partials"
ls -l "$OUT1" "$OUT2"
