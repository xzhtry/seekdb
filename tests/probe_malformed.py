#!/usr/bin/env python3
"""A malformed request must still be answered, and with the request id."""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

LINES = [
    ('{"id": 77, "op": "execute", "sql": "SELECT id FROM t"', 77),
    ('{"id": 78, "op": "execute", "sql": "SELECT id FROM t", "x": [1,2,3]', 78),
    ('{"id": 79, "op": "execute", "sql": "SELECT id FROM t", "extra": {"a": {"b": 1}}}', 79),
    ('not json at all', 0),
    ('{"op": "execute", "sql": "SELECT id FROM t"}', 0),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/proto2")
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

    bad = 0
    for line, expected_id in LINES:
        session.proc.stdin.write((line + "\n").encode())
        session.proc.stdin.flush()
        r = session.read(CONFIG["request_timeout"])
        ok = r.get("ok") is False and r.get("id") == expected_id
        if not ok:
            bad += 1
        print(f"{line[:54]:56s} -> {json.dumps(r, ensure_ascii=False)[:110]}")

    r = session.request("SELECT id FROM t")
    alive = r.get("ok") is False or r.get("ok") is True   # answered either way
    if not alive:
        bad += 1
    print("still alive:", alive)
    session.shutdown()
    print("malformed requests:", "OK" if bad == 0 else f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
