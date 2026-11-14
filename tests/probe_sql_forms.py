#!/usr/bin/env python3
"""Which ordinary SQL forms does the server accept?

The public sample pack only ever uses a small set of shapes.  The formal pack
is the same cases made harder, so it may well use the other obvious spellings.
Anything listed in the task statement has to work; everything else only has to
fail cleanly.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

work = Path("/tmp/forms")
work.mkdir(parents=True, exist_ok=True)
s = Session(CONFIG, str(work / "stderr.log"),
            command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
s.start()

# forms that the task statement requires to work
REQUIRED = [
    ("create table", "CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(8), v BIGINT)"),
    ("insert positional", "INSERT INTO t VALUES (1,'a',10)"),
    ("insert column list", "INSERT INTO t (id, tag, v) VALUES (2,'b',20)"),
    ("insert partial columns", "INSERT INTO t (id, v) VALUES (3,30)"),
    ("insert multi row", "INSERT INTO t VALUES (4,'c',40),(5,'d',50)"),
    ("select star", "SELECT * FROM t"),
    ("select qualified", "SELECT t.id FROM t"),
    ("select alias", "SELECT id AS k, v AS val FROM t"),
    ("select no alias", "SELECT id, v FROM t"),
    ("where eq", "SELECT id FROM t WHERE v = 10"),
    ("where and", "SELECT id FROM t WHERE v >= 10 AND v < 50"),
    ("where paren", "SELECT id FROM t WHERE (v = 10 OR v = 20) AND id > 0"),
    ("where not", "SELECT id FROM t WHERE NOT (v = 10)"),
    ("order asc", "SELECT id FROM t ORDER BY v"),
    ("order desc", "SELECT id FROM t ORDER BY v DESC"),
    ("order two keys", "SELECT id FROM t ORDER BY tag, v DESC"),
    ("order by alias", "SELECT id, v AS val FROM t ORDER BY val"),
    ("limit", "SELECT id FROM t ORDER BY id LIMIT 2"),
    ("limit offset", "SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 1"),
    ("limit zero", "SELECT id FROM t ORDER BY id LIMIT 0"),
    ("limit beyond", "SELECT id FROM t ORDER BY id LIMIT 1000"),
    ("update one col", "UPDATE t SET v = 11 WHERE id = 1"),
    ("update multi col", "UPDATE t SET v = 12, tag = 'z' WHERE id = 1"),
    ("update no match", "UPDATE t SET v = 13 WHERE id = 999"),
    ("delete one", "DELETE FROM t WHERE id = 5"),
    ("delete no match", "DELETE FROM t WHERE id = 999"),
    ("create index", "CREATE VECTOR INDEX tv ON t(v)"),
    ("drop index", "DROP INDEX tv ON t"),
    ("create if not exists", "CREATE TABLE IF NOT EXISTS t (id BIGINT PRIMARY KEY)"),
    ("drop table", "DROP TABLE t"),
    ("drop table if exists", "DROP TABLE IF EXISTS t"),
    ("begin", "BEGIN"),
    ("commit", "COMMIT"),
    ("rollback", "ROLLBACK"),
]

# forms the task explicitly does not require: they only have to fail cleanly
OPTIONAL = [
    ("between", "SELECT id FROM t WHERE v BETWEEN 10 AND 20"),
    ("in list", "SELECT id FROM t WHERE v IN (10, 20)"),
    ("like", "SELECT id FROM t WHERE tag LIKE 'a%'"),
    ("is null", "SELECT id FROM t WHERE tag IS NULL"),
    ("distinct", "SELECT DISTINCT tag FROM t"),
    ("having", "SELECT tag FROM t GROUP BY tag HAVING COUNT(*) > 1"),
    ("union", "SELECT id FROM t UNION SELECT id FROM t"),
    ("join", "SELECT t.id FROM t JOIN t u ON t.id = u.id"),
    ("subquery", "SELECT id FROM t WHERE id IN (SELECT id FROM t)"),
    ("count", "SELECT COUNT(*) FROM t"),
    ("alter", "ALTER TABLE t ADD x BIGINT"),
    ("null value", "INSERT INTO t VALUES (9,'n',NULL)"),
    ("update pk", "UPDATE t SET id = 77 WHERE id = 9"),
]

print("=== required forms ===")
bad = 0
for name, sql in REQUIRED:
    r = s.request(sql)
    ok = r.get("ok") is True
    if not ok:
        bad += 1
    print(f"{'OK  ' if ok else 'FAIL'} {name:24s} {json.dumps(r, ensure_ascii=False)[:120]}")
print(f"\n{bad} required forms failed")

print("\n=== optional forms (must fail cleanly, never hang) ===")
for name, sql in OPTIONAL:
    r = s.request(sql)
    ok = r.get("ok") is False and bool(r.get("error", {}).get("code"))
    print(f"{'ok  ' if ok else 'BAD '} {name:24s} {json.dumps(r, ensure_ascii=False)[:120]}")

s.shutdown()
