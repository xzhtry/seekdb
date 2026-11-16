#!/usr/bin/env python3
"""Write a JSONL request file for the write phase (used with callgrind)."""
import argparse
import json
import random
import struct
from pathlib import Path


def f32(v):
    return struct.unpack("f", struct.pack("f", v))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--out", default="/var/tmp/requests.jsonl")
    args = ap.parse_args()

    rng = random.Random(20261115)
    lines = []
    rid = 1
    lines.append(json.dumps({"id": rid, "op": "execute",
                              "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), "
                                     "embedding VECTOR(%d))" % args.dims}))
    rid += 1
    for start in range(0, args.rows, args.batch):
        parts = []
        for r in range(start, min(start + args.batch, args.rows)):
            vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
            parts.append("(%d,'c%d','[%s]')" % (r, r % 4, vec))
        lines.append(json.dumps({"id": rid, "op": "execute",
                                  "sql": "INSERT INTO docs VALUES " + ",".join(parts)}))
        rid += 1
    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("%s: %d requests, %d bytes" % (args.out, len(lines), Path(args.out).stat().st_size))


if __name__ == "__main__":
    main()
