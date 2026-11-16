#!/usr/bin/env python3
"""Run the write phase under callgrind and print the hot functions.

Callgrind counts instructions, so the result is deterministic and not affected
by the machine noise that makes wall clock A/B runs hard to read.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--workdir", default="/var/tmp/cg")
    ap.add_argument("--out", default="/var/tmp/cg.out")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    cmd = ["valgrind", "--tool=callgrind", "--callgrind-out-file=%s.%%p" % args.out,
           BIN, "--data-dir", str(work), "--protocol", "jsonl"]
    session = Session(CONFIG, str(work / "stderr.log"), command=cmd)
    session.start()
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)
    rng = random.Random(20261115)
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        parts = []
        for rid in range(start, min(start + args.batch, args.rows)):
            vec = ",".join("%.6f" % f32(rng.random()) for _ in range(args.dims))
            parts.append("(%d,'c%d','[%s]')" % (rid, rid % 4, vec))
        session.request("INSERT INTO docs VALUES " + ",".join(parts))
    elapsed = time.monotonic() - t0
    session.request("shutdown")
    print("inserted %d rows in %.3f s (under callgrind)" % (args.rows, elapsed))


if __name__ == "__main__":
    main()
