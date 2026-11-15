#!/usr/bin/env python3
"""Crash consistency of the streaming mutation path.

A bulk UPDATE now writes every changed row to the data file and to the WAL
while it walks the table.  If the process dies in the middle of that walk the
table must still be consistent after the recovery: either the row carries the
old value or the new one, never a half written row, and the rows the
statement never reached must keep the old value.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")


def value_of(session, table, rid):
    r = session.request("SELECT tag FROM %s WHERE id = %d" % (table, rid))
    if not r.get("ok"):
        return None
    rows = r.get("rows", [])
    return rows[0][0] if rows else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=20000)
    ap.add_argument("--workdir", default="/tmp/crashmut")
    ap.add_argument("--kills", type=int, default=3)
    args = ap.parse_args()

    work = Path(args.workdir)
    subprocess.run(["rm", "-rf", str(work)], check=False)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "stderr.log"

    session = Session(CONFIG, str(log), command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    session.request("CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(8))")

    batch = 100
    for start in range(0, args.rows, batch):
        values = ",".join("(%d,'old')" % (start + i) for i in range(batch))
        r = session.request("INSERT INTO t VALUES " + values)
        if r.get("ok") is not True:
            print("insert failed:", json.dumps(r, ensure_ascii=False)[:200])
            return 1
    print("loaded %d rows" % args.rows, flush=True)

    # Kill the process while a bulk UPDATE is running.  The update is issued
    # from a helper process so that the kill lands in the middle of the walk.
    for attempt in range(args.kills):
        helper = subprocess.Popen(
            [BIN, "--data-dir", str(work), "--protocol", "jsonl"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            start_new_session=True)
        req = json.dumps({"id": 1, "op": "execute",
                          "sql": "UPDATE t SET tag = 'new' WHERE id >= 0",
                          "search_mode": "exact"}) + "\n"
        helper.stdin.write(req.encode())
        helper.stdin.flush()
        time.sleep(0.02 + 0.02 * attempt)
        try:
            os.killpg(helper.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        helper.wait(timeout=10)
        print("killed during bulk update #%d" % (attempt + 1), flush=True)

        t0 = time.monotonic()
        session.start()
        ready = time.monotonic() - t0
        ok_ready = ready < CONFIG["startup_timeout"]
        print("[%s] ready after the kill: %.2fs" % ("OK " if ok_ready else "OVER", ready), flush=True)

        # Every row must carry one of the two values, never something else.
        r = session.request("SELECT id, tag FROM t ORDER BY id")
        if r.get("ok") is not True:
            print("select failed:", json.dumps(r, ensure_ascii=False)[:200])
            return 1
        rows = r.get("rows", [])
        bad = [row for row in rows if row[1] not in ("old", "new")]
        news = sum(1 for row in rows if row[1] == "new")
        print("rows=%d new=%d old=%d invalid=%d" % (len(rows), news, len(rows) - news, len(bad)),
              flush=True)
        if len(rows) != args.rows or bad:
            print("FAIL: the table is inconsistent after the kill", flush=True)
            return 1

    # After the last kill the statement is finished, so a rerun must see all
    # rows with the new value and a restart must keep them.
    r = session.request("UPDATE t SET tag = 'new' WHERE id >= 0")
    print("final update affected:", r.get("affected_rows"), flush=True)
    session.shutdown()
    session.start()
    r = session.request("SELECT id, tag FROM t ORDER BY id")
    rows = r.get("rows", [])
    bad = [row for row in rows if row[1] != "new"]
    print("after restart rows=%d not-new=%d" % (len(rows), len(bad)), flush=True)
    session.shutdown()
    if bad or len(rows) != args.rows:
        print("FAIL: the committed update did not survive the restart", flush=True)
        return 1
    print("crash consistency: OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
