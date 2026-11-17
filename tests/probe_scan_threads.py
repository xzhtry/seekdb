#!/usr/bin/env python3
"""Measure the throughput and the cpu cost of the vector scan.

The benchmark shape is reproduced: ten thousand rows of a hundred and
twenty eight dimensions, a thousand TopK queries, half of them with an
equality filter on an ordinary column.  Beside the wall clock of the query
phase the cpu time of the server process is read from /proc, so the cpu to
wall ratio says how much of the work was done in parallel.
"""
import argparse
import json
import os
import random
import statistics
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


def cpu_of(pid):
    with open("/proc/%d/stat" % pid, "r", encoding="utf-8") as fh:
        parts = fh.read().split()
    ticks = os.sysconf("SC_CLK_TCK")
    return (int(parts[13]) + int(parts[14])) / ticks


def peak_rss_of(pid):
    try:
        with open("/proc/%d/status" % pid, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--threads", type=int, default=0, help="MVDB_SCAN_THREADS")
    ap.add_argument("--bin", default="")
    ap.add_argument("--workdir", default="/var/tmp/threadbench")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()
    binary = args.bin or BIN

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.threads >= 0:
        env["MVDB_SCAN_THREADS"] = str(args.threads)
        # The transport hands its own environment to the server, so the setting
        # has to be in the environment of this process as well.
        os.environ["MVDB_SCAN_THREADS"] = str(args.threads)
    session = Session(CONFIG, str(work / "stderr.log"),
                      command=[binary, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    try:
        session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), "
                        "embedding VECTOR(%d))" % args.dims)
        rng = random.Random(7)
        for start in range(0, args.rows, args.batch):
            vals = []
            for rid in range(start, min(start + args.batch, args.rows)):
                vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
                vals.append("(%d,'%s','[%s]')"
                            % (rid, ["alpha", "beta", "gamma", "delta"][rid % 4], vec))
            session.request("INSERT INTO docs VALUES " + ",".join(vals))

        cats = ["alpha", "beta", "gamma", "delta"]
        queries = []
        for i in range(args.queries):
            r = random.Random(500000 + i)
            q = ",".join("%.6f" % f32(r.random()) for _ in range(args.dims))
            if i % 2 == 1:
                sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                       "WHERE category = '%s' ORDER BY distance LIMIT %d"
                       % (q, cats[i % len(cats)], args.k))
            else:
                sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                       "ORDER BY distance LIMIT %d" % (q, args.k))
            queries.append(sql)

        # warm up: the first query builds the snapshot and the fingerprints
        for sql in queries[:20]:
            session.request(sql)

        pid = session.proc.pid
        c0 = cpu_of(pid)
        t0 = time.monotonic()
        for sql in queries:
            resp = session.request(sql)
            if resp.get("ok") is not True:
                raise SystemExit("query failed")
        wall = time.monotonic() - t0
        cpu = cpu_of(pid) - c0
        rss = peak_rss_of(pid)

        out = {
            "threads": args.threads,
            "queries": args.queries,
            "wall_total_s": round(wall, 4),
            "wall_per_query_us": round(wall / args.queries * 1e6, 1),
            "queries_per_s": round(args.queries / wall, 1),
            "server_cpu_total_s": round(cpu, 4),
            "server_cpu_per_query_us": round(cpu / args.queries * 1e6, 1),
            "cpu_over_wall": round(cpu / wall, 3),
            "peak_rss_bytes": rss,
        }
        print(json.dumps(out, ensure_ascii=False))
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(out, ensure_ascii=False, indent=2),
                                           encoding="utf-8")
    finally:
        session.shutdown()


if __name__ == "__main__":
    main()
