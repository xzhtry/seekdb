#!/usr/bin/env python3
"""Three measurements on the official benchmark shape.

  * how much a filter of a given selectivity actually saves,
  * how the TopK size changes the cost,
  * whether the ann mode returns exactly what the exact mode returns.

They all use the official shape: 10,000 rows, 128 dimensions, K = 10, one
query every row scanned, half of the queries carrying an equality filter.
"""
import argparse
import json
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def build(session, args):
    rng = random.Random(args.seed)
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)
    for start in range(0, args.rows, args.batch):
        parts = []
        for rid in range(start, min(start + args.batch, args.rows)):
            vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
            parts.append("(%d,'c%d','[%s]')" % (rid, rid % 16, vec))
        resp = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if resp.get("ok") is not True:
            raise SystemExit("insert failed: " + json.dumps(resp, ensure_ascii=False)[:200])


def query_text(seed, dims):
    r = random.Random(seed)
    return ",".join("%.6f" % f32(r.random()) for _ in range(dims))


def measure(session, sql_for, queries, dims, k, mode="exact"):
    samples = []
    for q in queries:
        sql = sql_for(q)
        t0 = time.monotonic()
        resp = session.request(sql, mode=mode)
        samples.append((time.monotonic() - t0) * 1000.0)
        if resp.get("ok") is not True:
            raise SystemExit("query failed: " + json.dumps(resp, ensure_ascii=False)[:200])
    samples.sort()
    return samples[len(samples) // 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--queries", type=int, default=100)
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20261115)
    ap.add_argument("--workdir", default="/var/tmp/selk")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    build(session, args)

    queries = [query_text(900000 + i, args.dims) for i in range(args.queries)]

    # ---- 0. warm up: the first shape measured would otherwise pay for the
    # cold page cache and look slower than the shapes measured after it ----
    warm_sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                "ORDER BY distance LIMIT %d")
    for i in range(args.warmup):
        session.request(warm_sql % (query_text(800000 + i, args.dims), args.k))

    # ---- 1. selectivity curve, K = 10 ----
    selectivity = {}
    shapes = {
        "no_filter": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                      "ORDER BY distance LIMIT %d", 1.0),
        "filter_100pct": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                          "WHERE id >= 0 ORDER BY distance LIMIT %d", 1.0),
        "filter_50pct": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                         "WHERE id < %d ORDER BY distance LIMIT %d", 0.5),
        "filter_25pct": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                         "WHERE id < %d ORDER BY distance LIMIT %d", 0.25),
        "filter_6pct": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                        "WHERE id < %d ORDER BY distance LIMIT %d", 0.0625),
        "filter_0pct": ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                        "WHERE category = 'zz' ORDER BY distance LIMIT %d", 0.0),
    }
    for name, (template, frac) in shapes.items():
        limit = int(args.rows * frac) if frac < 1 else 0
        if name == "filter_0pct":
            sql_for = lambda q, t=template, l=limit: t % (q, l)
        elif limit:
            sql_for = lambda q, t=template, l=limit: t % (q, l, args.k)
        else:
            sql_for = lambda q, t=template: t % (q, args.k)
        selectivity[name] = round(measure(session, sql_for, queries, args.dims, args.k), 4)

    # ---- 2. K sweep ----
    ksweep = {}
    for k in (10, 100, 1000):
        sql_for = lambda q, kk=k: ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                                   "ORDER BY distance LIMIT %d" % (q, kk))
        ksweep["K=%d" % k] = round(measure(session, sql_for, queries, args.dims, k), 4)

    # ---- 3. ann vs exact, byte for byte ----
    sql_for = lambda q: ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                         "ORDER BY distance LIMIT %d" % (q, args.k))
    exact_rows = []
    ann_rows = []
    for q in queries:
        exact_rows.append(session.request(sql_for(q))["rows"])
        ann_rows.append(session.request(sql_for(q), mode="ann")["rows"])
    mismatches = sum(1 for a, b in zip(exact_rows, ann_rows) if a != b)

    session.shutdown()
    result = {
        "rows": args.rows,
        "dims": args.dims,
        "queries": args.queries,
        "selectivity_ms": selectivity,
        "k_sweep_ms": ksweep,
        "ann_vs_exact_mismatches": mismatches,
        "ann_vs_exact_queries": len(queries),
    }
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
