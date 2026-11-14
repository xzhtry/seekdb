#!/usr/bin/env python3
"""Robustness sweep: every statement must produce exactly one response.

The formal pack exercises statements the task explicitly does not require
(NULL, JOIN, aggregates, subqueries, ALTER TABLE, foreign keys, nested
transactions, primary key updates).  Those have to fail with a proper error
response -- never hang, never crash, never emit a second response.
"""
import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

SETUP = [
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2))",
    "INSERT INTO docs VALUES (1,'a','hello','[1,0]'),(2,'b','world','[0,1]'),(3,'a','x','[2,2]')",
    "CREATE TABLE other (id BIGINT PRIMARY KEY, tag VARCHAR(8))",
    "INSERT INTO other VALUES (1,'t1'),(2,'t2')",
    "CREATE TABLE kids (id BIGINT PRIMARY KEY, pid BIGINT, FOREIGN KEY (pid) REFERENCES docs(id))",
]

CASES = [
    # --- supported shapes -------------------------------------------------
    ("select_star", "SELECT * FROM docs"),
    ("select_alias", "SELECT id AS k, L2_DISTANCE(embedding,'[0,0]') AS d FROM docs ORDER BY d LIMIT 2"),
    ("where_and_or", "SELECT id FROM docs WHERE (category = 'a' OR category = 'b') AND id > 0"),
    ("where_not", "SELECT id FROM docs WHERE NOT (id = 1)"),
    ("order_desc_limit", "SELECT id FROM docs ORDER BY id DESC LIMIT 2"),
    ("limit_offset", "SELECT id FROM docs ORDER BY id LIMIT 1 OFFSET 1"),
    ("update_multi_col", "UPDATE docs SET content = 'y', category = 'z' WHERE id = 1"),
    ("delete_range", "DELETE FROM docs WHERE id >= 2"),
    ("multi_row_insert", "INSERT INTO docs VALUES (10,'m','m','[1,1]'),(11,'m','n','[2,2]')"),
    ("int64_max", "INSERT INTO docs VALUES (9223372036854775807,'m','m','[1,1]')"),
    ("int64_min", "INSERT INTO docs VALUES (-9223372036854775808,'m','m','[1,1]')"),
    ("comment_only", "SELECT id FROM docs -- trailing comment"),
    ("block_comment", "/* c */ SELECT id FROM docs"),
    ("multi_line", "SELECT id\nFROM docs\nWHERE id = 1"),
    ("unicode_value", "INSERT INTO docs VALUES (20,'u','中文测试','[1,1]')"),
    ("empty_string", "INSERT INTO docs VALUES (21,'e','','[1,1]')"),
    ("backtick_ident", "SELECT `id` FROM `docs`"),
    ("semicolon_ws", "SELECT id FROM docs ;   "),
    ("create_drop", "CREATE TABLE tmp1 (id BIGINT PRIMARY KEY); DROP TABLE tmp1"),
    ("begin_commit", "BEGIN; UPDATE docs SET content='c' WHERE id=3; COMMIT;"),
    ("begin_rollback", "BEGIN; UPDATE docs SET content='r' WHERE id=3; ROLLBACK;"),
    ("vector_index", "CREATE VECTOR INDEX dv ON docs(embedding)"),
    ("drop_index", "DROP INDEX dv"),
    ("show_tables", "SHOW TABLES"),
    ("describe", "DESCRIBE docs"),
    ("use_db", "USE test"),
    # --- explicitly unsupported: must error, not hang ---------------------
    ("unsupported_join", "SELECT d.id FROM docs d JOIN other o ON d.id = o.id"),
    ("unsupported_aggregate", "SELECT COUNT(*) FROM docs"),
    ("unsupported_sum", "SELECT SUM(id) FROM docs"),
    ("unsupported_group", "SELECT category, COUNT(*) FROM docs GROUP BY category"),
    ("unsupported_having", "SELECT id FROM docs GROUP BY id HAVING id > 1"),
    ("unsupported_subquery", "SELECT id FROM docs WHERE id IN (SELECT id FROM other)"),
    ("unsupported_subquery_expr", "SELECT (SELECT id FROM other) AS x FROM docs"),
    ("unsupported_alter_add", "ALTER TABLE docs ADD extra BIGINT"),
    ("unsupported_alter_drop", "ALTER TABLE docs DROP COLUMN content"),
    ("unsupported_alter_modify", "ALTER TABLE docs MODIFY content VARCHAR(32)"),
    ("unsupported_alter_rename", "ALTER TABLE docs RENAME TO docs2"),
    ("unsupported_alter_index", "ALTER TABLE docs ADD INDEX ix (id)"),
    ("unsupported_null_insert", "INSERT INTO docs VALUES (30,'n',NULL,'[1,1]')"),
    ("unsupported_null_where", "SELECT id FROM docs WHERE content IS NULL"),
    ("unsupported_nested_txn", "BEGIN; BEGIN; COMMIT;"),
    ("unsupported_pk_update", "UPDATE docs SET id = 999 WHERE id = 1"),
    ("unsupported_fk_insert", "INSERT INTO other VALUES (3,'t3')"),
    ("unsupported_like", "SELECT id FROM docs WHERE content LIKE 'h%'"),
    ("unsupported_in_list", "SELECT id FROM docs WHERE id IN (1,2)"),
    ("unsupported_between", "SELECT id FROM docs WHERE id BETWEEN 1 AND 2"),
    ("unsupported_distinct", "SELECT DISTINCT category FROM docs"),
    ("unsupported_union", "SELECT id FROM docs UNION SELECT id FROM other"),
    ("unsupported_join_inner", "SELECT * FROM docs INNER JOIN other ON docs.id = other.id"),
    ("unsupported_left_join", "SELECT * FROM docs LEFT JOIN other ON docs.id = other.id"),
    # --- malformed / hostile ----------------------------------------------
    ("bad_unclosed_paren", "SELECT id FROM docs WHERE (id = 1"),
    ("bad_unclosed_quote", "SELECT id FROM docs WHERE content = 'abc"),
    ("bad_unclosed_bracket", "SELECT L2_DISTANCE(embedding,'[1,2') FROM docs"),
    ("bad_unclosed_comment", "SELECT id FROM docs /* unterminated"),
    ("bad_garbage", "!!! ??? ###"),
    ("bad_only_semicolons", ";;;"),
    ("bad_keywords", "SELECT FROM WHERE"),
    ("bad_truncated_insert", "INSERT INTO docs VALUES (1,'a'"),
    ("bad_missing_table", "SELECT id FROM nosuchtable"),
    ("bad_missing_column", "SELECT nosuchcolumn FROM docs"),
    ("bad_wrong_value_count", "INSERT INTO docs VALUES (1)"),
    ("bad_too_many_values", "INSERT INTO docs VALUES (1,'a','b','[1,1]','extra')"),
    ("bad_reserved", "SELECT id FROM docs WHERE id = @@@"),
    ("bad_only_comment", "-- nothing"),
    ("bad_empty", ""),
    ("bad_spaces", "     "),
    ("bad_nul_like", "SELECT id FROM docs WHERE id = 1 /*!40001 */"),
    ("bad_deep_nest", "SELECT id FROM docs WHERE " + "(" * 60 + "id = 1" + ")" * 60),
    ("bad_long_ident", "SELECT " + "a" * 5000 + " FROM docs"),
    ("bad_long_string", "INSERT INTO docs VALUES (99,'L','" + "x" * 5000 + "','[1,1]')"),
    ("bad_many_values", "INSERT INTO docs VALUES " + ",".join(
        f"({i},'m','m','[1,1]')" for i in range(500))),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/robustness")
    ap.add_argument("--image", default=None, help="run against a built image (judge limits)")
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    log = Path("/tmp/robustness/stderr.log")
    if log.exists():
        log.unlink()

    if args.image:
        session = Session(CONFIG, str(log), image=args.image, data_root=str(work))
    else:
        session = Session(CONFIG, str(log),
                          command=[BIN, "--data-dir", str(work), "--protocol", "jsonl"])
    session.start()
    for sql in SETUP:
        session.request(sql)

    problems = []
    for name, sql in CASES:
        t0 = time.monotonic()
        try:
            r = session.request(sql)
            dt = time.monotonic() - t0
            if r.get("ok") is not True and r.get("ok") is not False:
                problems.append((name, f"bad ok value: {r!r}"))
            if "id" not in r:
                problems.append((name, f"missing id: {r!r}"))
            if dt > args.timeout:
                problems.append((name, f"slow: {dt:.2f}s"))
            print(f"{name:24s} ok={r.get('ok')} {dt * 1000:7.1f}ms "
                  f"{json.dumps(r, ensure_ascii=False)[:110]}", flush=True)
        except Exception as exc:  # noqa: BLE001
            problems.append((name, f"{type(exc).__name__}: {exc}"))
            print(f"{name:24s} EXCEPTION {type(exc).__name__}: {exc}", flush=True)
            break

    # The process must still be alive and must still answer.
    try:
        r = session.request("SELECT id FROM docs ORDER BY id")
        alive = r.get("ok") is True
    except Exception as exc:  # noqa: BLE001
        alive = False
        problems.append(("final_ping", f"{type(exc).__name__}: {exc}"))
    session.shutdown()

    print("\n=== problems ===")
    if not problems and alive:
        print("none")
    for name, why in problems:
        print(f"{name}: {why}")
    print(f"survived: {alive}")
    return 0 if (not problems and alive) else 1


if __name__ == "__main__":
    sys.exit(main())
