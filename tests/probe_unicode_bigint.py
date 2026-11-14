#!/usr/bin/env python3
"""Probe the two suspected defects found by the hardened suite."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

work = Path("/tmp/probe")
work.mkdir(parents=True, exist_ok=True)
log = work / "stderr.log"
if log.exists():
    log.unlink()
s = Session(CONFIG, str(log), command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
s.start()


def show(label, sql):
    r = s.request(sql)
    print(f"{label}\n  sql: {sql}\n  -> {json.dumps(r, ensure_ascii=False)[:300]}", flush=True)
    return r


print("=== VARCHAR length: code points or bytes? ===")
show("varchar(4) + 2 emoji (2 code points, 8 bytes)", "CREATE TABLE u (id BIGINT PRIMARY KEY, c VARCHAR(4))")
show("insert 2 emoji", "INSERT INTO u VALUES (1,'😀😀')")
show("insert 4 emoji (4 code points, 16 bytes)", "INSERT INTO u VALUES (2,'😀😀😀😀')")
show("insert 5 emoji (5 code points)", "INSERT INTO u VALUES (3,'😀😀😀😀😀')")
show("insert 4 chinese (4 code points, 12 bytes)", "INSERT INTO u VALUES (4,'中文测试')")
show("rows", "SELECT id, c FROM u ORDER BY id")

print()
print("=== BIGINT literals ===")
show("create", "CREATE TABLE b (id BIGINT PRIMARY KEY, v BIGINT)")
show("insert exact ints", "INSERT INTO b VALUES (0,9007199254740993),(1,-9007199254740993),"
                           "(2,9223372036854775807),(3,-9223372036854775808),(4,0),(5,1),(6,-1),(7,4294967297)")
show("rows", "SELECT id, v FROM b ORDER BY id")
show("compare > 2^53", "SELECT id FROM b WHERE v > 9007199254740992")
show("compare = 2^53+1", "SELECT id FROM b WHERE v = 9007199254740993")
show("compare < -2^53", "SELECT id FROM b WHERE v < -9007199254740992")
show("min/max", "SELECT id FROM b WHERE v = -9223372036854775808")

s.shutdown()
