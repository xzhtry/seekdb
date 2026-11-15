#!/usr/bin/env python3
"""How much does the row cache budget buy, and what does it cost in memory?

The judge measures the peak memory of the container as one third of the
performance metric (M is 20% of E next to the 60% time and the 20% I/O), and
the container runs in a 2 GiB cgroup.  The row cache is the only part of the
process that grows with the table, so the budget is a lever that trades peak
memory against the number of row decodes.

The server takes the budget from MVDB_CACHE_MB.  Every budget below runs the
official workload shape (10,000 rows, 128 dimensions, 1,000 TopK queries, half
of them filtered) in its own database directory, so no run can reuse a warm
cache from another one.
"""
import argparse
import json
import os
import random
import shutil
import statistics
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "protocol_check"))

import bench_official_shape as bench  # noqa: E402


class Args:
    def __init__(self):
        self.rows = 10000
        self.dims = 128
        self.batch = 100
        self.queries = 1000
        self.k = 10
        self.categories = "alpha,beta,gamma,delta"
        self.warmup = 20
        self.recall_sample = 4
        self.seed = 20261115
        self.workdir = ""
        self.rounds = 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budgets", default="1,2,4,8,16,32")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--workdir", default="/var/tmp/cachebudget")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    budgets = [int(x) for x in args.budgets.split(",")]
    base = Path(args.workdir)
    shutil.rmtree(base, ignore_errors=True)
    base.mkdir(parents=True, exist_ok=True)

    results = []
    for mb in budgets:
        os.environ["MVDB_CACHE_MB"] = str(mb)
        rounds = []
        for r in range(args.rounds):
            a = Args()
            a.workdir = str(base / ("mb%d_r%d" % (mb, r)))
            a.rounds = 1
            rng = random.Random(a.seed + r)
            out = bench.run_round(a, rng)
            out["round"] = r + 1
            rounds.append(out)
        row = {
            "cache_mb": mb,
            "t_write": round(statistics.median(x["t_write"] for x in rounds), 3),
            "t_query": round(statistics.median(x["t_query"] for x in rounds), 3),
            "t_query_per_query_ms": round(statistics.median(x["t_query_per_query_ms"] for x in rounds), 3),
            "peak_rss_bytes": int(statistics.median(x["peak_rss_bytes"] for x in rounds)),
            "b_total_bytes": int(statistics.median(x["b_total_bytes"] for x in rounds)),
            "data_dir_bytes": int(statistics.median(x["data_dir_bytes"] for x in rounds)),
            "recall_plain": rounds[0]["recall_plain"],
            "recall_filtered": rounds[0]["recall_filtered"],
            "rounds": rounds,
        }
        results.append(row)
        print(json.dumps({k: v for k, v in row.items() if k != "rounds"},
                         ensure_ascii=False), flush=True)

    del os.environ["MVDB_CACHE_MB"]
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(results, ensure_ascii=False, indent=2),
                                        encoding="utf-8")


if __name__ == "__main__":
    main()
