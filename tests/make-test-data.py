import argparse
import os

def write_bytes(path, size_bytes):
    pattern = bytes([i % 256 for i in range(256)])
    with open(path, "wb") as f:
        remaining = size_bytes
        while remaining > 0:
            chunk = pattern[: min(len(pattern), remaining)]
            f.write(chunk)
            remaining -= len(chunk)

def copy_prefix(src, dst, size_bytes):
    with open(src, "rb") as fsrc, open(dst, "wb") as fdst:
        fdst.write(fsrc.read(size_bytes))

def truncate_file(path, new_size):
    with open(path, "ab") as f:
        f.truncate(new_size)

def ensure_dirs(base_out):
    for name in ("peer1", "peer2", "peer3"):
        os.makedirs(os.path.join(base_out, name), exist_ok=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--size-bytes", type=int, required=True)
    ap.add_argument("--split", type=int, default=0)
    ap.add_argument("--mode", choices=["singleSeed", "multiSeed", "missingChunk"], default="multiSeed")
    args = ap.parse_args()

    ensure_dirs(args.out)

    peer1Dir = os.path.join(args.out, "peer1")
    peer2Dir = os.path.join(args.out, "peer2")

    fullPath = os.path.join(peer1Dir, args.file)
    write_bytes(fullPath, args.size_bytes)

    if args.mode == "singleSeed":
        return

    if args.mode == "multiSeed":
        if args.split <= 0:
            raise SystemExit("--split must be > 0 for multiSeed")
        copy_prefix(fullPath, os.path.join(peer2Dir, args.file), args.split)
        return

    if args.mode == "missingChunk":
        if args.split <= 0:
            raise SystemExit("--split must be > 0 for missingChunk")
        copy_prefix(fullPath, os.path.join(peer2Dir, args.file), args.split)
        truncate_file(fullPath, args.split)
        return

if __name__ == "__main__":
    main()
