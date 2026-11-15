#!/usr/bin/env python3
"""How much does a commit cost because the WAL carries a copy of every row?

Our WAL record contains the full row payload, so a commit has to flush that
copy before it can answer "committed".  This probe measures the fdatasync
latency as a function of the number of bytes that have to be flushed, which
gives the cost curve that decides whether dropping the payload from the WAL
would pay off.
"""
import argparse
import json
import os
import statistics
import time
from pathlib import Path


def fsync_curve(path, sizes, rounds):
    fd = os.open(str(path), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    out = []
    try:
        for size in sizes:
            payload = b"x" * size
            os.write(fd, payload)
            os.fdatasync(fd)
            samples = []
            for _ in range(rounds):
                t0 = time.monotonic()
                os.write(fd, payload)
                os.fdatasync(fd)
                samples.append((time.monotonic() - t0) * 1000.0)
            samples.sort()
            out.append({
                "bytes": size,
                "median_ms": round(samples[len(samples) // 2], 4),
                "p10_ms": round(samples[len(samples) // 10], 4),
                "p90_ms": round(samples[(len(samples) * 9) // 10], 4),
            })
    finally:
        os.close(fd)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/var/tmp/fsynccurve")
    ap.add_argument("--sizes", default="4096,32768,71680,524288")
    ap.add_argument("--rounds", type=int, default=120)
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    sizes = [int(s) for s in args.sizes.split(",")]
    curve = fsync_curve(work / "probe", sizes, args.rounds)

    # What our commit actually flushes: the WAL carries one copy of every row
    # (128 float32 = 512 B plus the record header), and the batch is 100 rows.
    row_bytes = 512 + 40
    wal_batch = row_bytes * 100
    by_size = {c["bytes"]: c["median_ms"] for c in curve}
    near = min(by_size, key=lambda b: abs(b - wal_batch))
    marker = by_size[sizes[0]]

    result = {
        "curve": curve,
        "wal_payload_bytes_per_batch": wal_batch,
        "fsync_ms_for_wal_batch": by_size[near],
        "fsync_ms_for_marker_only": marker,
        "fsync_ms_saved_if_payload_dropped": round(by_size[near] - marker, 4),
        "fsync_rounds_added_if_data_file_fsynced": 1,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
