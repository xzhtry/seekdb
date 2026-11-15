#!/usr/bin/env python3
"""Break the official-shape benchmark down into its cost components.

Two questions the raw benchmark numbers cannot answer:

  1. How is the disk I/O split between the data file and the WAL?  The WAL
     carries a copy of every row payload, so the write phase touches the
     data twice.
  2. How much of a TopK query is protocol and scan overhead, and how much is
     actually spent computing distances?  That is what decides whether an
     approximate index would buy anything.
"""
import argparse
import json
import random
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")


def f32(value):
    import struct
    return struct.unpack("f", struct.pack("f", value))[0]


def wal_watcher(work, stop, out):
    """Track the WAL size while the run is in progress."""
    peak = 0
    while not stop.is_set():
        try:
            size = (work / "wal.log").stat().st_size
            peak = max(peak, size)
        except OSError:
            pass
        time.sleep(0.02)
    out["wal_peak"] = peak


def io_reader(pid, out):
    read_bytes = 0
    write_bytes = 0
    try:
        with open("/proc/%d/io" % pid, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("read_bytes:"):
                    read_bytes = int(line.split()[1])
                elif line.startswith("write_bytes:"):
                    write_bytes = int(line.split()[1])
    except OSError:
        pass
    out["read_bytes"] = read_bytes
    out["write_bytes"] = write_bytes


def timed(session, sql, repeats):
    t0 = time.monotonic()
    for _ in range(repeats):
        resp = session.request(sql)
        if resp.get("ok") is not True:
            raise SystemExit("query failed: " + json.dumps(resp, ensure_ascii=False)[:300])
    return (time.monotonic() - t0) / repeats * 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--queries", type=int, default=300)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--workdir", default="/var/tmp/perfbreak")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    stop = threading.Event()
    wal = {}
    watcher = threading.Thread(target=wal_watcher, args=(work, stop, wal))
    watcher.start()

    rng = random.Random(20261115)
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)

    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for rid in range(start, min(start + args.batch, args.rows)):
            vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
            parts.append("(%d,'c%d','[%s]')" % (rid, rid % 4, vec))
        session.request("INSERT INTO docs VALUES " + ",".join(parts))
    t_write = time.monotonic() - t0

    io = {}
    io_reader(session.proc.pid, io)
    data_file = work / "tbl_1.dat"
    data_bytes = data_file.stat().st_size if data_file.exists() else 0

    def query_text(seed):
        r = random.Random(seed)
        return ",".join("%.6f" % f32(r.random()) for _ in range(args.dims))

    shapes = {
        "no_filter_all_rows": "SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                               "ORDER BY distance LIMIT %d",
        "filter_all_rows": "SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                           "WHERE id >= 0 ORDER BY distance LIMIT %d",
        "filter_quarter_rows": "SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                               "WHERE category = 'c0' ORDER BY distance LIMIT %d",
        "filter_no_rows": "SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                          "WHERE category = 'zz' ORDER BY distance LIMIT %d",
    }
    lat = {}
    for name, template in shapes.items():
        # warm up, then measure the median of a few batches
        samples = []
        for b in range(5):
            q = query_text(700000 + b)
            sql = template % (q, args.k)
            session.request(sql)
        for b in range(5):
            q = query_text(800000 + b)
            sql = template % (q, args.k)
            samples.append(timed(session, sql, args.queries // 5))
        lat[name] = round(statistics.median(samples), 4)

    stop.set()
    watcher.join()
    io_after = {}
    io_reader(session.proc.pid, io_after)
    session.shutdown()

    result = {
        "rows": args.rows,
        "dims": args.dims,
        "batches": args.rows // args.batch,
        "t_write": round(t_write, 3),
        "data_file_bytes": data_bytes,
        "wal_peak_bytes": wal.get("wal_peak", 0),
        "write_bytes_total": io_after["write_bytes"],
        "read_bytes_total": io_after["read_bytes"],
        "latency_ms": lat,
    }
    result["wal_to_data_ratio"] = round(wal.get("wal_peak", 0) / max(1, data_bytes), 4)
    result["write_to_data_ratio"] = round(io_after["write_bytes"] / max(1, data_bytes), 4)
    overhead = lat["filter_no_rows"]
    full = lat["no_filter_all_rows"]
    result["distance_cost_ms"] = round(full - overhead, 4)
    result["distance_share"] = round((full - overhead) / max(1e-9, full), 4)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
