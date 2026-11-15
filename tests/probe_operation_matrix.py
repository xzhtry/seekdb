#!/usr/bin/env python3
"""Operation matrix on a table that does not fit into the memory budget.

One formal case fails with "JSON response timeout", which means one
statement misses the 60s request budget of the judge.  This walks every
statement shape that a formal case could plausibly use over a table bigger
than the 2 GiB budget and reports the latency and the peak memory of each
one, so that no other shape is left unmeasured.
"""
import argparse
import json
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
BUDGET = CONFIG["request_timeout"]
LIMIT_MIB = 2048


def sample_peak_rss(pid, stop, out):
    peak = 0
    while not stop.is_set():
        try:
            with open("/proc/%d/status" % pid, "r", encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("VmRSS:"):
                        peak = max(peak, int(line.split()[1]))
                        break
        except OSError:
            pass
        time.sleep(0.2)
    out["peak_kb"] = peak


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=2000000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=1000)
    ap.add_argument("--workdir", default="/var/tmp/matrix")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"

    session = Session(CONFIG, str(log), command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    stop = threading.Event()
    stats = {}
    watcher = threading.Thread(target=sample_peak_rss, args=(session.proc.pid, stop, stats))
    watcher.start()

    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for i in range(args.batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(args.dims))
            parts.append("(%d,'x','[%s]')" % (rid, vec))
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:200], flush=True)
            return 1
    data_mb = sum(f.stat().st_size for f in work.glob("*") if f.is_file()) / 1048576.0
    print("loaded %d rows x %d dims in %.1fs, data %.0f MiB"
          % (args.rows, args.dims, time.monotonic() - t0, data_mb), flush=True)

    q = "[" + ",".join("0" for _ in range(args.dims)) + "]"

    def run(label, fn, budget=BUDGET):
        t0 = time.monotonic()
        try:
            r = fn()
            dt = time.monotonic() - t0
            ok = r.get("ok") is True
            extra = "affected=%s rows=%d" % (r.get("affected_rows"), len(r.get("rows", [])))
        except Exception as exc:  # noqa: BLE001
            dt = time.monotonic() - t0
            ok = False
            extra = "%s: %s" % (type(exc).__name__, exc)
        verdict = "OK " if dt < budget * 0.5 else ("WARN" if dt < budget else "OVER")
        print("[%s] %-38s %8.2fs (budget %ss) ok=%s %s"
              % (verdict, label, dt, budget, ok, extra), flush=True)
        return dt, ok

    run("TopK exact", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs ORDER BY d LIMIT 10" % q))
    run("TopK ann", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs ORDER BY d LIMIT 10" % q, "ann"))
    run("TopK filtered", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs WHERE tag='x' "
        "ORDER BY d LIMIT 10" % q))
    run("distance in WHERE", lambda: session.request(
        "SELECT id FROM docs WHERE L2_DISTANCE(embedding,'%s') < 100 LIMIT 10" % q))
    run("scan with limit", lambda: session.request("SELECT id FROM docs LIMIT 10"))
    run("update non vector column", lambda: session.request(
        "UPDATE docs SET tag='y' WHERE id >= 0"))
    run("update vector column", lambda: session.request(
        "UPDATE docs SET embedding='[1,2]' WHERE id >= 0"))
    run("transaction begin", lambda: session.request("BEGIN;"))
    session.request("ROLLBACK;")
    run("delete all rows", lambda: session.request("DELETE FROM docs WHERE id >= 0"))
    # Last, because the answer does not fit into the 16 MiB the driver allows
    # for one response and the transport cannot continue afterwards.
    run("scan ordered no limit", lambda: session.request("SELECT id FROM docs ORDER BY id"))

    stop.set()
    watcher.join()
    peak = stats.get("peak_kb", 0) / 1024.0
    print("peak RSS: %.0f MiB (judge limit %d MiB)" % (peak, LIMIT_MIB), flush=True)
    session.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
