#!/usr/bin/env python3
"""Local replica of the official MiniVectorDB performance benchmark.

The scoring benchmark is described in the official task doc (赛题一 3.3):

  * small scale, single client: 10,000 records, 128 dimensions,
  * 1,000 TopK queries with K = 10, half of them carrying an equality filter
    on an ordinary column,
  * the write phase goes in batches of 100 rows and every batch is committed,
  * queries run in exact mode, Recall@10 >= 0.95 for the filtered and the
    unfiltered group,
  * T is the wall clock time of the batch phase and of the query phase,
    B is the disk read + write byte count of the whole run,
    M is the peak memory of the container,
  * three rounds, the median round is used for ranking.

Everything here is a *proxy*: T is measured around the official transport,
B comes from /proc/<pid>/io and M from VmHWM, so the page cache is not
counted the way the cgroup v2 memory.peak would count it.  The numbers are
only meaningful as a relative baseline between our own revisions.
"""
import argparse
import json
import math
import random
import statistics
import struct
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


def sample_resources(pid, stop, out):
    """Track the disk I/O counters and the peak RSS of the server process."""
    peak_rss = 0
    read_bytes = 0
    write_bytes = 0
    while not stop.is_set():
        try:
            with open("/proc/%d/status" % pid, "r", encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("VmHWM:"):
                        peak_rss = max(peak_rss, int(line.split()[1]) * 1024)
                        break
        except OSError:
            pass
        try:
            with open("/proc/%d/io" % pid, "r", encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("read_bytes:"):
                        read_bytes = int(line.split()[1])
                    elif line.startswith("write_bytes:"):
                        write_bytes = int(line.split()[1])
        except OSError:
            pass
        time.sleep(0.05)
    out["peak_rss"] = peak_rss
    out["read_bytes"] = read_bytes
    out["write_bytes"] = write_bytes


def f32(value):
    """Round a double to the float32 value that the server actually stores."""
    return struct.unpack("f", struct.pack("f", value))[0]


def build_dataset(rows, dims, rng, categories):
    """Deterministic data set: float32 values in [0, 1) plus a category tag."""
    matrix = []
    cats = []
    vectors = []
    for rid in range(rows):
        vec = [f32(rng.random()) for _ in range(dims)]
        matrix.append(vec)
        cats.append(categories[rid % len(categories)])
        vectors.append(",".join("%.6f" % x for x in vec))
    return matrix, cats, vectors


def truth_topk(matrix, cats, query, k, category=None):
    """Brute force TopK over the float32 values that are actually stored."""
    scored = []
    for rid, row in enumerate(matrix):
        if category is not None and cats[rid] != category:
            continue
        total = 0.0
        for a, b in zip(row, query):
            d = a - b
            total += d * d
        scored.append((math.sqrt(total), rid))
    scored.sort(key=lambda item: (item[0], item[1]))
    return [rid for _, rid in scored[:k]]


def run_round(args, rng):
    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"

    session = Session(CONFIG, str(log), command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    t_start = time.monotonic()
    session.start()
    t_ready = time.monotonic() - t_start

    stop = threading.Event()
    stats = {}
    watcher = threading.Thread(target=sample_resources, args=(session.proc.pid, stop, stats))
    watcher.start()

    cats_list = args.categories.split(",")
    matrix, cats, vectors = build_dataset(args.rows, args.dims, rng, cats_list)
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))"
                    % args.dims)

    # ---- write phase: batches of 100 rows, every batch committed ----
    t0 = time.monotonic()
    for start in range(0, args.rows, args.batch):
        chunk = list(zip(range(start, min(start + args.batch, args.rows)), cats[start:start + args.batch],
                         vectors[start:start + args.batch]))
        values = ",".join("(%d,'%s','[%s]')" % (rid, cat, vec) for rid, cat, vec in chunk)
        resp = session.request("INSERT INTO docs VALUES " + values)
        if resp.get("ok") is not True:
            raise SystemExit("insert failed: " + json.dumps(resp, ensure_ascii=False)[:300])
    t_write = time.monotonic() - t0

    # ---- index preparation and warmup stay outside the measured query window ----
    for i in range(args.warmup):
        w = random.Random(1000 + i)
        q = ",".join("%.6f" % w.random() for _ in range(args.dims))
        session.request("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                        "ORDER BY distance LIMIT %d" % (q, args.k))
    session.request("SELECT COUNT(*) AS n FROM docs")

    # ---- query phase: half unfiltered, half filtered ----
    queries = []
    for i in range(args.queries):
        r = random.Random(500000 + i)
        q = [f32(r.random()) for _ in range(args.dims)]
        filtered = (i % 2 == 1)
        cat = cats_list[i % len(cats_list)]
        queries.append((q, filtered, cat, ",".join("%.6f" % float(x) for x in q)))

    t0 = time.monotonic()
    results = []
    for q, filtered, cat, qtext in queries:
        if filtered:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "WHERE category = '%s' ORDER BY distance LIMIT %d" % (qtext, cat, args.k))
        else:
            sql = ("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs "
                   "ORDER BY distance LIMIT %d" % (qtext, args.k))
        resp = session.request(sql)
        if resp.get("ok") is not True:
            raise SystemExit("query failed: " + json.dumps(resp, ensure_ascii=False)[:300])
        results.append([int(row[0]) for row in resp.get("rows", [])])
    t_query = time.monotonic() - t0

    # ---- recall check on a sample, the judge checks every query ----
    # Sample both groups explicitly: the filtered and the unfiltered queries
    # alternate, so a plain stride would only ever pick one parity.
    half = max(1, args.recall_sample // 2)
    sample_idx = []
    for want_filtered in (False, True):
        picked = 0
        for idx, (_q, filtered, _cat, _t) in enumerate(queries):
            if filtered != want_filtered:
                continue
            picked += 1
            if picked > half:
                break
            sample_idx.append(idx)
    hits = {"plain": 0, "filtered": 0}
    seen = {"plain": 0, "filtered": 0}
    for idx in sample_idx:
        q, filtered, cat, _ = queries[idx]
        expect = truth_topk(matrix, cats, q, args.k, cat if filtered else None)
        key = "filtered" if filtered else "plain"
        hits[key] += len(set(expect) & set(results[idx]))
        seen[key] += len(expect)
    recall_plain = hits["plain"] / seen["plain"] if seen["plain"] else 0.0
    recall_filtered = hits["filtered"] / seen["filtered"] if seen["filtered"] else 0.0

    stop.set()
    watcher.join()
    session.shutdown()

    data_bytes = sum(f.stat().st_size for f in work.glob("*") if f.is_file())
    return {
        "t_ready": round(t_ready, 3),
        "t_write": round(t_write, 3),
        "t_query": round(t_query, 3),
        "t_query_per_query_ms": round(t_query / args.queries * 1000, 3),
        "read_bytes": stats["read_bytes"],
        "write_bytes": stats["write_bytes"],
        "b_total_bytes": stats["read_bytes"] + stats["write_bytes"],
        "peak_rss_bytes": stats["peak_rss"],
        "data_dir_bytes": data_bytes,
        "recall_plain": round(recall_plain, 4),
        "recall_filtered": round(recall_filtered, 4),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--categories", default="alpha,beta,gamma,delta")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=20)
    ap.add_argument("--recall-sample", type=int, default=20)
    ap.add_argument("--seed", type=int, default=20261115)
    ap.add_argument("--workdir", default="/var/tmp/benchofficial")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    rounds = []
    for r in range(args.rounds):
        rng = random.Random(args.seed + r)
        result = run_round(args, rng)
        result["round"] = r + 1
        rounds.append(result)
        print(json.dumps(result, ensure_ascii=False), flush=True)

    med = {
        "t_write": round(statistics.median(x["t_write"] for x in rounds), 3),
        "t_query": round(statistics.median(x["t_query"] for x in rounds), 3),
        "t_query_per_query_ms": round(statistics.median(x["t_query_per_query_ms"] for x in rounds), 3),
        "b_total_bytes": int(statistics.median(x["b_total_bytes"] for x in rounds)),
        "peak_rss_bytes": int(statistics.median(x["peak_rss_bytes"] for x in rounds)),
        "t_ready": round(statistics.median(x["t_ready"] for x in rounds), 3),
        "recall_plain": rounds[0]["recall_plain"],
        "recall_filtered": rounds[0]["recall_filtered"],
    }
    print("MEDIAN " + json.dumps(med, ensure_ascii=False), flush=True)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps({"rounds": rounds, "median": med},
                                                  ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
