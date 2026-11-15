#!/usr/bin/env python3
"""Check the tie-break rule for equal distances.

The official spec says that rows at the same distance are ordered by
ascending primary key, and the public case vector_ties_and_filter relies on
it.  Any change to how a distance is computed (for example a SIMD kernel with
a different summation order) risks breaking exactly that rule, so this test
pins the behaviour down and prints it in a quotable form.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/var/tmp/tieorder")
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)

    session = Session(CONFIG, str(work / "stderr.log"),
                       command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(2))")
    # Two different vectors at the same distance from [0,0], one exact tie and
    # one tie that only shows up after a filter.
    session.request("INSERT INTO docs VALUES (7,'keep','[1,0]'), (3,'keep','[-1,0]'), "
                    "(9,'skip','[1,0]'), (1,'skip','[-1,0]')")
    plain = session.request("SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs "
                            "ORDER BY distance LIMIT 4")
    filtered = session.request("SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs "
                               "WHERE category = 'keep' ORDER BY distance LIMIT 4")
    ann = session.request("SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs "
                          "ORDER BY distance LIMIT 4", mode="ann")
    session.shutdown()

    def ids(resp):
        return [int(r[0]) for r in resp.get("rows", [])]

    plain_ids = ids(plain)
    filtered_ids = ids(filtered)
    ann_ids = ids(ann)
    # All four rows sit at distance 1, so the order must be by id.
    ok = plain_ids == [1, 3, 7, 9] and filtered_ids == [3, 7] and ann_ids == [1, 3, 7, 9]
    print("plain ids=%s expected [1, 3, 7, 9]" % plain_ids)
    print("filtered ids=%s expected [3, 7]" % filtered_ids)
    print("ann ids=%s expected [1, 3, 7, 9]" % ann_ids)
    print("distances=%s" % [r[1] for r in plain.get("rows", [])])
    print("tie order by id: %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
