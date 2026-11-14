#!/usr/bin/env python3
"""Targeted stress on the two directions that could still time out.

1. Vector literals and expressions: random fuzzing rarely produces these, but a
   formal case may well contain a wide vector or an extreme literal.
2. A data set that is larger than the judge's memory budget (2 GiB), with the
   peak RSS sampled so it can be compared against that budget.
"""
import argparse
import json
import math
import os
import random
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")
BUDGET = CONFIG["request_timeout"]


def sample_peak_rss(pid, stop, out):
    """Watch /proc/<pid>/status for the high water mark of the RSS."""
    peak = 0
    path = f"/proc/{pid}/status"
    while not stop.is_set():
        try:
            with open(path, "r") as fh:
                for line in fh:
                    if line.startswith("VmHWM:"):
                        peak = max(peak, int(line.split()[1]))
                        break
        except OSError:
            pass
        time.sleep(0.2)
    out["peak_kb"] = peak


def run_vector_literals(session):
    """Every one of these must come back as a response, quickly."""
    rng = random.Random(4242)
    db = "CREATE TABLE v (id BIGINT PRIMARY KEY, e VECTOR(4))"
    session.request(db)
    cases = []
    # wide vectors of many lengths
    for n in (1, 2, 3, 4, 5, 8, 64, 256, 1024, 4096):
        cases.append((f"literal width {n}", "INSERT INTO v VALUES (1,'[" +
                       ",".join("1" for _ in range(n)) + "]')"))
    # extreme numeric literals
    for text in ("1e308", "-1e308", "1e-308", "3.4e38", "-3.4e38", "1e39",
                 "0.0000001", "1e-45", "1e-46", "16777217", "16777216",
                 "-16777217", "9007199254740993", "1e400", "-1e400"):
        cases.append((f"literal value {text}", f"INSERT INTO v VALUES (1,'[{text},0,0,0]')"))
    # wide queries
    wide = ",".join("0" for _ in range(4096))
    cases.append(("wide query", f"SELECT id, L2_DISTANCE(e,'[{wide}]') AS d FROM v"))
    cases.append(("wide query ann", f"SELECT id, L2_DISTANCE(e,'[{wide}]') AS d FROM v LIMIT 5"))
    # deep expressions
    deep = "1" + "+1" * 2000
    cases.append(("deep expression", f"SELECT {deep} FROM v"))
    nested = "(" * 200 + "1" + ")" * 200
    cases.append(("deep nesting", f"SELECT {nested} FROM v"))
    cases.append(("many predicates", "SELECT id FROM v WHERE " +
                   " AND ".join(f"id > {i}" for i in range(2000))))
    cases.append(("long varchar", "INSERT INTO v VALUES (1,'[1,2,3,4]')"))

    worst = 0.0
    bad = 0
    for name, sql in cases:
        t0 = time.monotonic()
        try:
            r = session.request(sql)
        except Exception as exc:  # noqa: BLE001
            bad += 1
            print(f"  [HANG/ERR] {name}: {type(exc).__name__}: {exc}", flush=True)
            continue
        dt = time.monotonic() - t0
        worst = max(worst, dt)
        if dt > BUDGET / 3:
            print(f"  [SLOW] {name}: {dt:.2f}s", flush=True)
    print(f"vector/expression literals: {len(cases)} statements, worst {worst:.3f}s, "
          f"{bad} failures", flush=True)
    return bad == 0 and worst < BUDGET


def run_scale(session, rows, dims, batch, proc):
    """Load more data than the judge's memory budget and watch the peak RSS."""
    session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), "
                    f"embedding VECTOR({dims}))")
    stop = threading.Event()
    stats = {}
    watcher = threading.Thread(target=sample_peak_rss, args=(proc.pid, stop, stats))
    watcher.start()
    t0 = time.monotonic()
    for start in range(0, rows, batch):
        parts = []
        for i in range(batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(dims))
            parts.append(f"({rid},'x','[{vec}]')")
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            print("  insert failed:", json.dumps(r, ensure_ascii=False)[:160], flush=True)
            break
    load_s = time.monotonic() - t0
    query = "[" + ",".join("0" for _ in range(dims)) + "]"
    t0 = time.monotonic()
    r = session.request(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                        "WHERE tag = 'x' ORDER BY d LIMIT 10")
    query_s = time.monotonic() - t0
    stop.set()
    watcher.join()
    print(f"loaded {rows} rows x {dims} dims in {load_s:.1f}s, TopK {query_s:.2f}s, "
          f"peak RSS {stats.get('peak_kb', 0) / 1024.0:.0f} MiB "
          f"(judge limit 2048 MiB)", flush=True)
    return stats.get("peak_kb", 0) < 2 * 1024 * 1024 and query_s < BUDGET


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/scale")
    ap.add_argument("--rows", type=int, default=2000000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"
    if log.exists():
        log.unlink()

    session = Session(CONFIG, str(log),
                      command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    proc = session.proc

    ok = True
    try:
        ok &= run_vector_literals(session)
    except Exception as exc:  # noqa: BLE001
        print("vector literals failed:", exc, flush=True)
        ok = False
    try:
        ok &= run_scale(session, args.rows, args.dims, args.batch, proc)
    except Exception as exc:  # noqa: BLE001
        print("scale failed:", exc, flush=True)
        ok = False
    try:
        session.shutdown()
    except Exception:  # noqa: BLE001
        pass
    print("scale-extremes:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
