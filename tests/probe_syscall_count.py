#!/usr/bin/env python3
"""Where does the per-row write cost go?  Count the syscalls.

The student read the write phase as "the fsync round trip is the floor, the
part we can still move is parsing and moving the data".  Before touching that
path it is worth knowing how many syscalls a row costs: /proc/<pid>/io
reports syscw, the number of write syscalls, next to the byte counters.
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


def f32(v):
    return struct.unpack("f", struct.pack("f", v))[0]


def io_counters(pid):
    out = {}
    try:
        with open("/proc/%d/io" % pid, "r", encoding="utf-8") as fh:
            for line in fh:
                key, _, val = line.partition(":")
                out[key] = int(val)
    except OSError:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--seed", type=int, default=20261115)
    ap.add_argument("--workdir", default="/var/tmp/syscount")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    pid = session.proc.pid
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)

    rng = random.Random(args.seed)
    before = io_counters(pid)
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for rid in range(start, min(start + args.batch, args.rows)):
            vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
            parts.append("(%d,'c%d','[%s]')" % (rid, rid % 4, vec))
        session.request("INSERT INTO docs VALUES " + ",".join(parts))
    elapsed = time.monotonic() - t0
    after = io_counters(pid)
    session.shutdown()

    rows = args.rows
    result = {
        "rows": rows,
        "batch": args.batch,
        "commits": (rows + args.batch - 1) // args.batch,
        "dims": args.dims,
        "t_write": round(elapsed, 4),
        "us_per_row": round(elapsed * 1e6 / rows, 3),
        "write_syscalls": after.get("syscw", 0) - before.get("syscw", 0),
        "read_syscalls": after.get("syscr", 0) - before.get("syscr", 0),
        "bytes_written": after.get("write_bytes", 0) - before.get("write_bytes", 0),
    }
    result["write_syscalls_per_row"] = round(result["write_syscalls"] / rows, 4)
    result["write_syscalls_per_commit"] = round(result["write_syscalls"] / result["commits"], 4)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
