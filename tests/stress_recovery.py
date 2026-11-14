#!/usr/bin/env python3
"""Recovery latency after a forced kill, at scale.

The judge grants 120s between starting the container and the ready line.  After
a forced kill the store has to replay the WAL and compact the table file, and
that is the most expensive path in the program -- this measures it with a
dataset that is close to the memory budget.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")
STARTUP_BUDGET = CONFIG["startup_timeout"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--workdir", default="/tmp/recovery")
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"
    if log.exists():
        log.unlink()

    session = Session(CONFIG, str(log),
                      command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), embedding VECTOR({args.dims}))")
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for i in range(args.batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(args.dims))
            parts.append(f"({rid},'x','[{vec}]')")
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:200])
            return 1
    load_s = time.monotonic() - t0
    data_mb = sum(f.stat().st_size for f in work.glob("*")) / 1048576.0
    print(f"loaded {args.rows} rows in {load_s:.1f}s, data {data_mb:.1f} MiB", flush=True)

    # A transaction that is still open when the process dies must leave nothing
    # behind, and the recovery must still finish inside the startup budget.
    session.request("BEGIN;")
    session.request("UPDATE docs SET embedding = '[1,1]' WHERE id >= 0")
    session.crash()
    print("killed with an open transaction", flush=True)

    t0 = time.monotonic()
    session.start()
    ready = time.monotonic() - t0
    verdict = "OK " if ready < STARTUP_BUDGET else "OVER"
    print(f"[{verdict}] ready after crash: {ready:.2f}s (budget {STARTUP_BUDGET}s)", flush=True)

    r = session.request("SELECT id FROM docs ORDER BY id")
    n = len(r.get("rows", []))
    print(f"rows after recovery: {n}/{args.rows} (uncommitted update must be gone)", flush=True)

    # Kill again while the recovery is running.
    killer = subprocess.Popen([BIN, "--data-dir", str(work), "--protocol", "jsonl"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(0.02)
    try:
        os.killpg(killer.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    killer.wait(timeout=10)

    t0 = time.monotonic()
    session.start()
    ready2 = time.monotonic() - t0
    verdict = "OK " if ready2 < STARTUP_BUDGET else "OVER"
    print(f"[{verdict}] ready after second kill: {ready2:.2f}s", flush=True)
    r = session.request("SELECT id FROM docs ORDER BY id")
    n2 = len(r.get("rows", []))
    print(f"rows after repeated recovery: {n2}/{args.rows}", flush=True)
    session.shutdown()
    return 0 if (ready < STARTUP_BUDGET and ready2 < STARTUP_BUDGET and n == args.rows) else 1


if __name__ == "__main__":
    sys.exit(main())
