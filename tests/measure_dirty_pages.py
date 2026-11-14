#!/usr/bin/env python3
"""Measure the dirty page cache a bulk load leaves behind.

The judge runs the container in a 2 GiB memory cgroup.  Dirty page cache
counts against that limit and is not reclaimable, so a bulk load that never
pushes its data file out can fill the cgroup: the kernel then throttles every
following write until the writeback catches up, which shows up as a single
request taking longer than the 60s budget.

This script loads the same data twice - with the periodic fsync enabled and
disabled - and reports the peak dirty page cache of the file system for both.
Run it on a real Linux file system; on tmpfs the pages belong to the process
and on a v9fs mount they belong to the host, so neither shows the effect.
"""
import argparse
import json
import os
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


def meminfo_kb(key):
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith(key):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def peak_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as fh:
            for line in fh:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def run(workdir, rows, dims, batch, sync_mb):
    work = Path(workdir)
    work.mkdir(parents=True, exist_ok=True)
    # The session's child process inherits the environment of this process.
    os.environ["MVDB_SYNC_MB"] = str(sync_mb)

    session = Session(CONFIG, str(work / "stderr.log"),
                      command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()

    stop = threading.Event()
    stats = {"dirty": 0, "writeback": 0, "rss": 0}

    def sampler():
        while not stop.is_set():
            stats["dirty"] = max(stats["dirty"], meminfo_kb("Dirty:"))
            stats["writeback"] = max(stats["writeback"], meminfo_kb("Writeback:"))
            stats["rss"] = max(stats["rss"], peak_rss_kb(session.proc.pid))
            time.sleep(0.1)

    t = threading.Thread(target=sampler)
    t.start()
    session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), "
                    f"embedding VECTOR({dims}))")
    t0 = time.monotonic()
    for start in range(0, rows, batch):
        parts = []
        for i in range(batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(dims))
            parts.append(f"({rid},'x','[{vec}]')")
        r = session.request("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:160], flush=True)
            break
    load_s = time.monotonic() - t0
    stop.set()
    t.join()
    data_mb = sum(f.stat().st_size for f in work.glob("tbl_*.dat")) / 1048576.0
    try:
        session.shutdown()
    except Exception:  # noqa: BLE001
        pass
    try:
        session.close()
    except Exception:  # noqa: BLE001
        pass
    return {
        "sync_mb": sync_mb,
        "rows": rows,
        "load_s": round(load_s, 1),
        "data_mb": round(data_mb, 1),
        "peak_dirty_mb": round(stats["dirty"] / 1024.0, 1),
        "peak_writeback_mb": round(stats["writeback"] / 1024.0, 1),
        "peak_rss_mb": round(stats["rss"] / 1024.0, 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/root/mvdb-dirty")
    ap.add_argument("--rows", type=int, default=600000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    args = ap.parse_args()

    results = []
    for sync_mb in (0, 32):
        work = Path(args.workdir) / f"sync{sync_mb}"
        res = run(str(work), args.rows, args.dims, args.batch, sync_mb)
        results.append(res)
        print(json.dumps(res), flush=True)
        # give the kernel time to write back what is left
        time.sleep(5)
    off = results[0]
    on = results[1]
    print(f"\nwithout periodic fsync: peak dirty {off['peak_dirty_mb']} MiB")
    print(f"with    periodic fsync: peak dirty {on['peak_dirty_mb']} MiB")
    print(f"reduction: {off['peak_dirty_mb'] - on['peak_dirty_mb']:.1f} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
