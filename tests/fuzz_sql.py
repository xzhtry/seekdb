#!/usr/bin/env python3
"""Randomised SQL fuzzing: hunt for statements that never produce a response.

Every request must be answered.  A statement that makes the server hang is
indistinguishable from a timeout in the judge, so this fuzzes the grammar with
mutations of the public corpus and random token soup.
"""
import argparse
import json
import random
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

KEYWORDS = [
    "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "ORDER", "BY", "LIMIT",
    "OFFSET", "GROUP", "HAVING", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
    "DELETE", "CREATE", "TABLE", "INDEX", "VECTOR", "PRIMARY", "KEY", "NULL",
    "BEGIN", "COMMIT", "ROLLBACK", "ALTER", "DROP", "JOIN", "ON", "AS",
    "DISTINCT", "COUNT", "SUM", "IN", "LIKE", "BETWEEN", "IS", "UNION",
    "L2_DISTANCE", "USE", "SHOW", "DESCRIBE", "ASC", "DESC", "EXISTS",
]
LITERALS = [
    "1", "0", "-1", "9223372036854775807", "3.5", "1e10", "'x'", "'中文'",
    "''", "NULL", "TRUE", "FALSE", "'[1,2]'", "'[NaN,0]'", "'[]'", "''''",
    "@x", "?", "0x1f", "1.2.3", "--c\n", "/*c*/", "`id`", "\\", '"q"',
]
PUNCT = ["(", ")", ",", ";", ".", "*", "=", "<", ">", "+", "-", "/", "%", "[", "]"]


def random_sql(rng):
    parts = []
    n = rng.randint(1, 24)
    for _ in range(n):
        r = rng.random()
        if r < 0.45:
            parts.append(rng.choice(KEYWORDS))
        elif r < 0.8:
            parts.append(rng.choice(LITERALS))
        else:
            parts.append(rng.choice(PUNCT))
    sep = rng.choice([" ", " ", " ", "\n", "\t", ""])
    text = sep.join(parts)
    if rng.random() < 0.5:
        text += ";"
    return text


def mutate(rng, sql):
    ops = rng.randint(1, 3)
    for _ in range(ops):
        if not sql:
            break
        k = rng.random()
        i = rng.randrange(len(sql))
        if k < 0.3:
            sql = sql[:i] + sql[i + 1:]
        elif k < 0.6:
            sql = sql[:i] + rng.choice(KEYWORDS + LITERALS + PUNCT) + sql[i:]
        elif k < 0.8:
            sql = sql[:i] + rng.choice(["(", ")", "'", '"', "[", "]", "/*", "--", "\\"]) + sql[i:]
        else:
            j = rng.randrange(len(sql))
            sql = sql[:i] + sql[j:j + 5] + sql[i:]
    return sql


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=20261114)
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--workdir", default="/tmp/fuzz")
    ap.add_argument("--image", default=None, help="run against a built image (judge limits)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"
    if log.exists():
        log.unlink()

    if args.image:
        # Volume mode, like protocol_check/run.py: a bind mount from a 0700
        # host directory is not writable inside the container because the judge
        # drops every capability.
        session = Session(CONFIG, str(log), image=args.image)
    else:
        session = Session(CONFIG, str(log),
                          command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2))")
    session.request("INSERT INTO docs VALUES (1,'a','x','[1,0]')")

    corpus = []
    for path in sorted((ROOT / "mysqltest" / "cases").glob("*.test")):
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("--"):
                corpus.append(line)

    hangs = []
    slow = []
    t_start = time.monotonic()
    for i in range(args.iterations):
        if rng.random() < 0.35 and corpus:
            sql = mutate(rng, rng.choice(corpus))
        else:
            sql = random_sql(rng)
        t0 = time.monotonic()
        try:
            session.request(sql)
        except Exception as exc:  # noqa: BLE001
            hangs.append((sql, f"{type(exc).__name__}: {exc}"))
            print(f"HANG/ERR after {i} iters: {sql!r} -> {exc}", flush=True)
            break
        dt = time.monotonic() - t0
        if dt > 1.0:
            slow.append((sql, dt))
            print(f"SLOW {dt:.2f}s: {sql!r}", flush=True)
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{args.iterations} ok ({time.monotonic() - t_start:.1f}s)", flush=True)

    alive = True
    try:
        session.request("SELECT id FROM docs")
    except Exception:  # noqa: BLE001
        alive = False
    try:
        session.shutdown()
    except Exception:  # noqa: BLE001
        pass

    print(f"\niterations={args.iterations} elapsed={time.monotonic() - t_start:.1f}s "
          f"alive={alive} hangs={len(hangs)} slow={len(slow)}")
    for sql, why in hangs:
        print(f"HANG: {sql!r} -> {why}")
    return 1 if hangs or not alive else 0


if __name__ == "__main__":
    sys.exit(main())
