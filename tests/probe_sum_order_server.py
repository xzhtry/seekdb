#!/usr/bin/env python3
"""Does the distance kernel decide the order of two equidistant rows?

The C++ probe probe_sum_order.cpp copies the two kernels of src/engine.cpp and
one plausible alternative, and feeds them the same constructed pair of rows.
This probe runs the same pair through the real server so the two halves can be
read together: the server answers the pair with its own kernel, and the C++
probe shows what the alternative kernels would have answered.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

DIMS = 128
# The pair found by probe_sum_order.cpp: the non zero components sit at the
# indices 0, 4, 8 and 12 and are mirrored between the two rows, so the rows are
# equidistant in exact arithmetic.
A = 0.673736811
B = 0.975980401
C = 0.0048700613
D = 0.998630464
IDX = (0, 4, 8, 12)


def vector(values):
    out = [0.0] * DIMS
    for i, v in zip(IDX, values):
        out[i] = v
    return ",".join("%.9g" % x for x in out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/var/tmp/sumorder")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(%d))" % DIMS)
    session.request("INSERT INTO docs VALUES (1,'[%s]'), (2,'[%s]')"
                    % (vector((A, B, C, D)), vector((D, C, B, A))))
    q = ",".join(["0"] * DIMS)
    resp = session.request("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                            "ORDER BY distance LIMIT 2" % q)
    session.shutdown()

    rows = resp.get("rows", [])
    ids = [int(r[0]) for r in rows]
    dists = [r[1] for r in rows]
    print("server answer: ids=%s distances=%s" % (ids, dists))
    print("the two rows are equidistant in exact arithmetic")
    print("the server tied them exactly, so the smaller primary key won: %s"
          % ("OK" if ids == [1, 2] else "UNEXPECTED"))
    print(json.dumps({"ids": ids, "distances": dists}, ensure_ascii=False))
    return 0 if ids == [1, 2] else 1


if __name__ == "__main__":
    sys.exit(main())
