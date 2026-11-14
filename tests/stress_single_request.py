#!/usr/bin/env python3
"""Measure the latency of individual requests against the judge's budgets.

The judge allows 120s for the handshake and 60s for a single request.  A
formal case fails with "JSON response timeout" when one statement misses the
60s budget, so the interesting number is the latency of the *worst single
statement*, not the average one.
"""
import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")
REQUEST_BUDGET = CONFIG["request_timeout"]
STARTUP_BUDGET = CONFIG["startup_timeout"]


def timed(label, fn, budget=REQUEST_BUDGET):
    t0 = time.monotonic()
    r = fn()
    dt = time.monotonic() - t0
    ok = r.get("ok") is True
    verdict = "OK " if dt < budget * 0.5 else ("WARN" if dt < budget else "OVER")
    print(f"[{verdict}] {label}: {dt:.2f}s (budget {budget}s) ok={ok} "
          f"rows={len(r.get('rows', []))}", flush=True)
    return dt, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=500000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--one-shot-rows", type=int, default=100000)
    ap.add_argument("--workdir", default="/tmp/stress")
    ap.add_argument("--image", default=None)
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = Path("/tmp/stress/stderr.log")
    if log.exists():
        log.unlink()

    if args.image:
        session = Session(CONFIG, str(log), image=args.image, data_root=str(work))
    else:
        session = Session(CONFIG, str(log),
                          command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])

    t0 = time.monotonic()
    session.start()
    print(f"handshake: {time.monotonic() - t0:.2f}s (budget {STARTUP_BUDGET}s)", flush=True)

    dims = args.dims
    session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), embedding VECTOR({dims}))")

    # 1) One statement carrying a very large multi-row INSERT.
    parts = []
    for i in range(args.one_shot_rows):
        vec = ",".join(str((i + j) % 97) for j in range(dims))
        parts.append(f"({i},'x','[{vec}]')")
    stmt = "INSERT INTO docs VALUES " + ",".join(parts)
    print(f"one-shot INSERT statement size: {len(stmt) / 1048576.0:.1f} MiB", flush=True)
    timed("one-shot INSERT", lambda: session.request(stmt))

    # 2) Fill the table in batches until the requested size.
    batch = 100
    t0 = time.monotonic()
    for start in range(args.one_shot_rows, args.rows, batch):
        parts = []
        for i in range(batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(dims))
            parts.append(f"({rid},'x','[{vec}]')")
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:200])
            break
    print(f"batch load of {args.rows - args.one_shot_rows} rows took "
          f"{time.monotonic() - t0:.1f}s", flush=True)

    session.request("CREATE VECTOR INDEX docs_vec ON docs(embedding)")
    query = "[" + ",".join("0" for _ in range(dims)) + "]"

    # 3) The two query shapes the judge measures.
    timed("exact TopK (no filter)",
          lambda: session.request(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                                  "ORDER BY d LIMIT 10"))
    timed("exact TopK (filtered)",
          lambda: session.request(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                                  "WHERE tag = 'x' ORDER BY d LIMIT 10"))
    timed("ann TopK (filtered)",
          lambda: session.request(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                                  "WHERE tag = 'x' ORDER BY d LIMIT 10", "ann"))
    timed("plain filtered scan",
          lambda: session.request("SELECT id FROM docs WHERE tag = 'x' LIMIT 1000"))
    timed("count via full scan",
          lambda: session.request("SELECT id FROM docs ORDER BY id LIMIT 1"))

    # 4) Restart: recovery has to fit into the startup budget.
    session.shutdown()
    t0 = time.monotonic()
    session.start()
    ready = time.monotonic() - t0
    print(f"[{'OK ' if ready < STARTUP_BUDGET else 'OVER'}] restart handshake: {ready:.2f}s "
          f"(budget {STARTUP_BUDGET}s)", flush=True)
    timed("TopK after restart",
          lambda: session.request(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                                  "WHERE tag = 'x' ORDER BY d LIMIT 10"))
    session.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
