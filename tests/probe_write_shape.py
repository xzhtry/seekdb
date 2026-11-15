#!/usr/bin/env python3
"""Split the per-row write cost into a fixed part and a per-dimension part.

Every implementation pays for parsing the 128 float literals of a row, so that
part is a public cost: it does not separate us from the reference
implementation.  What separates us is the fixed per-commit cost.  This probe
inserts the same number of rows into tables with a different vector width and
reports the slope, and it measures the raw fsync latency of the filesystem so
the per-commit cost can be compared against the floor.
"""
import argparse
import json
import os
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


def fsync_latency(path, rounds=200, size=4096):
    """Raw fdatasync latency of the filesystem the data directory lives on."""
    payload = b"x" * size
    fd = os.open(str(path), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    try:
        os.write(fd, payload)
        os.fdatasync(fd)
        samples = []
        for _ in range(rounds):
            t0 = time.monotonic()
            os.write(fd, payload)
            os.fdatasync(fd)
            samples.append((time.monotonic() - t0) * 1000.0)
        samples.sort()
        return samples[len(samples) // 2]
    finally:
        os.close(fd)


def insert_shape(session, args, dims):
    """Insert rows.rows rows in rows.batch commits, with the given vector width."""
    rng = random.Random(args.seed)
    name = "d%d" % dims
    if dims > 0:
        session.request("CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                        % (name, dims))
    else:
        session.request("CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(8))" % name)
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for rid in range(start, min(start + args.batch, args.rows)):
            if dims > 0:
                vec = ",".join("%.6f" % f32(rng.random()) for _ in range(dims))
                parts.append("(%d,'c%d','[%s]')" % (rid, rid % 4, vec))
            else:
                parts.append("(%d,'c%d')" % (rid, rid % 4))
        resp = session.request("INSERT INTO %s VALUES %s" % (name, ",".join(parts)))
        if resp.get("ok") is not True:
            raise SystemExit("insert failed: " + json.dumps(resp, ensure_ascii=False)[:200])
    elapsed = time.monotonic() - t0
    return {"dims": dims, "rows": args.rows, "commits": (args.rows + args.batch - 1) // args.batch,
            "t_write": elapsed, "us_per_row": elapsed * 1e6 / args.rows,
            "ms_per_commit": elapsed * 1000.0 / ((args.rows + args.batch - 1) // args.batch)}


def insert_shape_isolated(args, dims):
    """Same as insert_shape() but in its own database and data directory.

    The WAL is per database, so sharing one database between the shapes makes
    the later shapes flush a WAL that already carries the earlier rows.
    """
    work = Path(args.workdir) / ("d%d" % dims)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    try:
        return insert_shape(session, args, dims)
    finally:
        session.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--dims", default="0,8,128")
    ap.add_argument("--seed", type=int, default=20261115)
    ap.add_argument("--workdir", default="/var/tmp/writeshape")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    fsync_ms = fsync_latency(work / "fsync_probe")

    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.shutdown()

    shapes = []
    for dims in [int(d) for d in args.dims.split(",")]:
        shapes.append(insert_shape_isolated(args, dims))
        print("  dims=%-4d %8.2f us/row  %6.3f ms/commit" %
              (shapes[-1]["dims"], shapes[-1]["us_per_row"], shapes[-1]["ms_per_commit"]), flush=True)

    # The slope between two widths is the cost of one vector dimension per row.
    base = next((s for s in shapes if s["dims"] == 0), None)
    wide = max(shapes, key=lambda s: s["dims"])
    per_dim_us = None
    if base and wide["dims"] > 0:
        per_dim_us = (wide["us_per_row"] - base["us_per_row"]) / wide["dims"]

    result = {
        "rows": args.rows,
        "batch": args.batch,
        "shapes": shapes,
        "fsync_latency_ms": round(fsync_ms, 4),
        "per_dimension_us_per_row": round(per_dim_us, 4) if per_dim_us is not None else None,
        "fixed_us_per_row": round(base["us_per_row"], 4) if base else None,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
