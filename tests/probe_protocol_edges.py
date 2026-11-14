#!/usr/bin/env python3
"""Protocol edge cases: the request fields the judge may vary.

Every request has to be answered exactly once, the id has to come back with
its original JSON type, and ok has to be a boolean.  A malformed request must
still be answered with the request id, otherwise the judge sees a protocol
error instead of a failed statement.
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

VARIANTS = [
    ("no search_mode", {"id": 9001, "op": "execute", "sql": "SELECT id FROM t"}),
    ("search_mode null", {"id": 9002, "op": "execute", "sql": "SELECT id FROM t",
                          "search_mode": None}),
    ("search_mode ANN upper", {"id": 9003, "op": "execute", "sql": "SELECT id FROM t",
                               "search_mode": "ANN"}),
    ("search_mode empty", {"id": 9004, "op": "execute", "sql": "SELECT id FROM t",
                           "search_mode": ""}),
    ("search_mode unknown", {"id": 9005, "op": "execute", "sql": "SELECT id FROM t",
                             "search_mode": "brute"}),
    ("extra object field", {"id": 9006, "op": "execute", "sql": "SELECT id FROM t",
                             "search_mode": "exact", "extra": {"a": 1}}),
    ("extra array field", {"id": 9007, "op": "execute", "sql": "SELECT id FROM t",
                           "params": [1, {"b": [2, 3]}]}),
    ("string id", {"id": "abc", "op": "execute", "sql": "SELECT id FROM t"}),
    ("unknown op", {"id": 9008, "op": "ping", "sql": "SELECT id FROM t"}),
    ("no sql", {"id": 9009, "op": "execute"}),
    ("sql empty", {"id": 9010, "op": "execute", "sql": ""}),
    ("sql only semicolons", {"id": 9011, "op": "execute", "sql": ";;;"}),
]

MALFORMED = [
    '{"id": 77, "op": "execute", "sql": "SELECT id FROM t"',
    '{"id": 78, "op": "execute", "sql": "SELECT id FROM t", "x": [1,2,3]',
    "not json at all",
    '{"op": "execute", "sql": "SELECT id FROM t"}',
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/proto")
    ap.add_argument("--image", default=None)
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"
    if log.exists():
        log.unlink()
    if args.image:
        session = Session(CONFIG, str(log), image=args.image)
    else:
        session = Session(CONFIG, str(log),
                          command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))")
    session.request("INSERT INTO t VALUES (1,'[1,0]'),(2,'[0,1]')")

    bad = 0
    print("--- request field variants ---")
    for label, request in VARIANTS:
        session.send(request)
        r = session.read(CONFIG["request_timeout"])
        ok_id = r.get("id") == request["id"] and type(r.get("id")) is type(request["id"])
        ok_ok = type(r.get("ok")) is bool
        if not (ok_id and ok_ok):
            bad += 1
        print(f"{label:22s} id_echo={'ok' if ok_id else 'BAD(' + repr(r.get('id')) + ')'} "
              f"ok_bool={'ok' if ok_ok else 'BAD'} -> {json.dumps(r, ensure_ascii=False)[:100]}")

    print("\n--- malformed requests (must still carry the id) ---")
    for line in MALFORMED:
        session.proc.stdin.write((line + "\n").encode())
        session.proc.stdin.flush()
        r = session.read(CONFIG["request_timeout"])
        expected = None
        if '"id"' in line:
            try:
                expected = int(line.split('"id"')[1].split(":")[1].split(",")[0].strip())
            except (IndexError, ValueError):
                expected = None
        else:
            expected = 0
        # a request without an id is still valid JSON: it runs with id 0
        ok = r.get("id") == expected and type(r.get("ok")) is bool
        if not ok:
            bad += 1
        print(f"{line[:52]:54s} -> {json.dumps(r, ensure_ascii=False)[:100]}")

    r = session.request("SELECT id FROM t")
    alive = r.get("ok") is True
    if not alive:
        bad += 1
    print(f"\nstill alive: {alive}")
    session.shutdown()
    print("protocol edges:", "OK" if bad == 0 else f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
