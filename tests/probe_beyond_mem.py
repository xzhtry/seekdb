#!/usr/bin/env python3
"""Scale test at the "more data than the memory limit" size.

The formal pack keeps one case whose data is larger than the judge's 2 GiB
memory budget.  This measures the three budgets that matter for it:
  * one request must stay below the 60s request timeout,
  * the peak RSS must stay below the 2 GiB cgroup limit,
  * the restart handshake must stay below the 120s startup timeout.
The data directory lives on a real ext4 filesystem, not on tmpfs, so the
numbers include the page cache pressure the judge also has.  This is the
script that reproduced the judge's "JSON response timeout" locally: a bulk
UPDATE over five million rows used to need more memory than the table itself.
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
JUDGE_LIMIT_MIB = 2048


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


def timed(label, fn, budget):
    t0 = time.monotonic()
    try:
        r = fn()
    except Exception as exc:  # noqa: BLE001
        dt = time.monotonic() - t0
        print("[OVER] %-34s %7.2fs (budget %ss) -> %s: %s"
              % (label, dt, budget, type(exc).__name__, exc), flush=True)
        return dt, False
    dt = time.monotonic() - t0
    print("[%s] %-34s %7.2fs (budget %ss) ok=%s rows=%d"
          % ("OK " if dt < budget * 0.5 else ("WARN" if dt < budget else "OVER"),
             label, dt, budget, r.get("ok"), len(r.get("rows", []))), flush=True)
    return dt, r.get("ok") is True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=5000000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=1000)
    ap.add_argument("--workdir", default="/var/tmp/bigscale")
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
    worst = 0.0
    for start in range(0, args.rows, args.batch):
        parts = []
        for i in range(args.batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(args.dims))
            parts.append("(%d,'x','[%s]')" % (rid, vec))
        t1 = time.monotonic()
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        dt = time.monotonic() - t1
        worst = max(worst, dt)
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:200], flush=True)
            break
    load_s = time.monotonic() - t0
    data_mb = sum(f.stat().st_size for f in work.glob("*") if f.is_file()) / 1048576.0
    print("loaded %d rows x %d dims in %.1fs (worst request %.2fs), data %.0f MiB"
          % (args.rows, args.dims, load_s, worst, data_mb), flush=True)

    q = "[" + ",".join("0" for _ in range(args.dims)) + "]"
    timed("exact TopK", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs ORDER BY d LIMIT 10" % q),
        CONFIG["request_timeout"])
    timed("exact TopK ann mode", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs ORDER BY d LIMIT 10" % q, "ann"),
        CONFIG["request_timeout"])
    timed("filtered scan LIMIT 1000", lambda: session.request(
        "SELECT id FROM docs WHERE tag = 'x' LIMIT 1000"), CONFIG["request_timeout"])
    timed("full scan LIMIT 1", lambda: session.request(
        "SELECT id FROM docs ORDER BY id LIMIT 1"), CONFIG["request_timeout"])
    timed("bulk UPDATE all rows", lambda: session.request(
        "UPDATE docs SET tag = 'y' WHERE id >= 0"), CONFIG["request_timeout"])
    timed("bulk DELETE all rows", lambda: session.request(
        "DELETE FROM docs WHERE id >= 0"), CONFIG["request_timeout"])

    peak = stats.get("peak_kb", 0) / 1024.0
    print("peak RSS during the run: %.0f MiB (judge limit %d MiB)" % (peak, JUDGE_LIMIT_MIB), flush=True)

    t0 = time.monotonic()
    session.shutdown()
    print("shutdown took %.2fs" % (time.monotonic() - t0), flush=True)

    t0 = time.monotonic()
    session.start()
    ready = time.monotonic() - t0
    print("[%s] restart handshake %.2fs (budget %ss)"
          % ("OK " if ready < CONFIG["startup_timeout"] else "OVER", ready,
             CONFIG["startup_timeout"]), flush=True)
    timed("TopK after restart", lambda: session.request(
        "SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM docs ORDER BY d LIMIT 10" % q),
        CONFIG["request_timeout"])
    stop.set()
    watcher.join()
    session.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
