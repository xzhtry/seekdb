#!/usr/bin/env python3
"""Split the query phase into server work, client work and round trip cost.

The official benchmark measures the wall clock of the whole query phase, so it
contains the time the driver spends on the pipe and the time the server spends
on the scan.  This probe separates the three so the next optimisation goes to
the part that is actually large.
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
    """utime + stime of a process, in seconds."""
    with open("/proc/%d/stat" % pid, "r", encoding="utf-8") as fh:
        parts = fh.read().split()
    ticks = os.sysconf("SC_CLK_TCK")
    return (int(parts[13]) + int(parts[14])) / ticks


def build_requests(rows, dims, queries, k, rng):
    cats = ["alpha", "beta", "gamma", "delta"]
    out = []
    for i in range(queries):
        r = random.Random(500000 + i)
        q = ",".join("%.6f" % f32(r.random()) for _ in range(dims))
        if i % 2 == 1:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "WHERE category = '%s' ORDER BY distance LIMIT %d"
                   % (q, cats[i % len(cats)], k))
        else:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "ORDER BY distance LIMIT %d" % (q, k))
        out.append(sql)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--bin", default="")
    ap.add_argument("--workdir", default="/var/tmp/splitq")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()
    binary = args.bin or BIN

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
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
                vals.append("(%d,'%s','[%s]')" % (rid, ["alpha", "beta", "gamma", "delta"][rid % 4], vec))
            session.request("INSERT INTO docs VALUES " + ",".join(vals))

        srv_pid = session.proc.pid
        queries = build_requests(args.rows, args.dims, args.queries, args.k, rng)

        # --- 1. the round trip cost alone: a query that touches nothing ---
        trivial = ["SELECT id FROM docs WHERE id = 1"] * args.queries
        c0 = cpu_of(os.getpid())
        s0 = cpu_of(srv_pid)
        t0 = time.monotonic()
        for sql in trivial:
            session.request(sql)
        t_trivial = time.monotonic() - t0
        s1 = cpu_of(srv_pid)
        c1 = cpu_of(os.getpid())
        trivial_server_cpu = s1 - s0
        trivial_client_cpu = c1 - c0

        # --- 2. the real query phase ---
        c0 = cpu_of(os.getpid())
        s0 = cpu_of(srv_pid)
        t0 = time.monotonic()
        for sql in queries:
            session.request(sql)
        t_query = time.monotonic() - t0
        s1 = cpu_of(srv_pid)
        c1 = cpu_of(os.getpid())
        query_server_cpu = s1 - s0
        query_client_cpu = c1 - c0

        # --- 3. a filtered query that matches nothing: the scan without the
        #        distance computation ---
        empty = ["SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                 "WHERE category = 'nosuch' ORDER BY distance LIMIT %d"
                 % (",".join("%.6f" % 0.5 for _ in range(args.dims)), args.k)] * args.queries
        s0 = cpu_of(srv_pid)
        t0 = time.monotonic()
        for sql in empty:
            session.request(sql)
        t_empty = time.monotonic() - t0
        s1 = cpu_of(srv_pid)
        empty_server_cpu = s1 - s0

        out = {
            "trivial_roundtrip": {
                "wall_total_s": round(t_trivial, 4),
                "wall_per_request_us": round(t_trivial / args.queries * 1e6, 1),
                "server_cpu_per_request_us": round(trivial_server_cpu / args.queries * 1e6, 1),
                "client_cpu_per_request_us": round(trivial_client_cpu / args.queries * 1e6, 1),
            },
            "query_phase": {
                "wall_total_s": round(t_query, 4),
                "wall_per_request_us": round(t_query / args.queries * 1e6, 1),
                "server_cpu_per_request_us": round(query_server_cpu / args.queries * 1e6, 1),
                "client_cpu_per_request_us": round(query_client_cpu / args.queries * 1e6, 1),
            },
            "scan_without_distance": {
                "wall_per_request_us": round(t_empty / args.queries * 1e6, 1),
                "server_cpu_per_request_us": round(empty_server_cpu / args.queries * 1e6, 1),
            },
        }
        print(json.dumps(out, ensure_ascii=False, indent=2))
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(out, ensure_ascii=False, indent=2),
                                           encoding="utf-8")
    finally:
        session.shutdown()


if __name__ == "__main__":
    main()
