#!/usr/bin/env python3
"""Write a JSONL request file with the load phase and the query phase."""
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
    ap.add_argument("--queries", type=int, default=100)
    ap.add_argument("--out", default="/var/tmp/reqq.jsonl")
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
    cats = ["alpha", "beta", "gamma", "delta"]
    for i in range(args.queries):
        q = [f32(rng.random()) for _ in range(args.dims)]
        text = ",".join("%.6f" % x for x in q)
        if i % 2 == 1:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "WHERE category = '%s' ORDER BY distance LIMIT 10" % (text, cats[i % 4]))
        else:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "ORDER BY distance LIMIT 10" % text)
        lines.append(json.dumps({"id": rid, "op": "execute", "sql": sql}))
        rid += 1
    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("%s: %d requests, %d bytes" % (args.out, len(lines), Path(args.out).stat().st_size))


if __name__ == "__main__":
    main()
