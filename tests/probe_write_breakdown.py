#!/usr/bin/env python3
"""Split the write phase into a per-commit cost and a per-row cost.

The official benchmark commits every batch, so the write phase is
  t = commits * C + rows * R
with C the fixed cost of one commit (the WAL fsync and the bookkeeping around
it) and R the cost of one row (JSON parsing, encoding, data file append and
WAL record).  Two batch sizes over the same number of rows give two
equations, which is enough to separate the two.
"""
import argparse
import json
import random
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
    import struct
    return struct.unpack("f", struct.pack("f", value))[0]


def write_phase(args, batch):
    work = Path(args.workdir) / ("b%d" % batch)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    rng = random.Random(args.seed)
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)
    rows = []
    for rid in range(args.rows):
        vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
        rows.append("(%d,'c%d','[%s]')" % (rid, rid % 4, vec))
    t0 = time.monotonic()
    for start in range(0, args.rows, batch):
        resp = session.request("INSERT INTO docs VALUES " + ",".join(rows[start:start + batch]))
        if resp.get("ok") is not True:
            raise SystemExit("insert failed: " + json.dumps(resp, ensure_ascii=False)[:300])
    elapsed = time.monotonic() - t0
    wal = 0
    try:
        wal = (work / "wal.log").stat().st_size
    except OSError:
        pass
    data = 0
    try:
        data = (work / "tbl_1.dat").stat().st_size
    except OSError:
        pass
    session.shutdown()
    return {"batch": batch, "commits": (args.rows + batch - 1) // batch, "t_write": elapsed,
            "wal_bytes": wal, "data_bytes": data}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch-sizes", default="100,10")
    ap.add_argument("--seed", type=int, default=20261115)
    ap.add_argument("--workdir", default="/var/tmp/writebreak")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    runs = []
    for batch in [int(b) for b in args.batch_sizes.split(",")]:
        r = write_phase(args, batch)
        runs.append(r)
        print(json.dumps(r, ensure_ascii=False), flush=True)

    # t = commits * C + rows * R
    a, b = runs[0], runs[1]
    d_commits = a["commits"] - b["commits"]
    c = (a["t_write"] - b["t_write"]) / d_commits if d_commits else 0.0
    r = (a["t_write"] - a["commits"] * c) / args.rows if args.rows else 0.0
    result = {
        "runs": runs,
        "per_commit_ms": round(c * 1000, 4),
        "per_row_us": round(r * 1e6, 4),
        "commit_share_of_write_phase": round(a["commits"] * c / a["t_write"], 4),
        "row_share_of_write_phase": round(args.rows * r / a["t_write"], 4),
    }
    print("RESULT " + json.dumps(result, ensure_ascii=False), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
