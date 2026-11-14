#!/usr/bin/env python3
"""Hardened variants of the 20 public cases.

The task package says the public samples are simplified: "基础样例已简化，不能用
评测仓库中的同名正式用例替代它们".  The formal pack has 29 cases - the same
names, but heavier - plus 9 sealed ones.  So the useful exercise is to make
every public case harder and check that the behaviour still holds.

Every expectation is computed independently in Python (brute force truth), not
by replaying what the server returned, so this is a real cross check:

  * L2 distances are recomputed from the float32 values that were stored
  * ties are broken by ascending primary key
  * filtered TopK is computed inside the filtered set
  * VARCHAR limits count Unicode code points
  * BIGINT values are compared as exact integers
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")
ABS_TOL = 1e-5
REL_TOL = 1e-5

RESULTS = []


def f32(x):
    """Round a Python float to float32, the way the server stores vectors."""
    return struct.unpack("f", struct.pack("f", float(x)))[0]


def l2(a, b):
    """Euclidean distance over the float32 values that were stored."""
    total = 0.0
    for x, y in zip(a, b):
        d = f32(x) - f32(y)
        total += d * d
    return math.sqrt(total)


def record(name, ok, detail):
    RESULTS.append({"case": name, "ok": bool(ok), "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name} ({detail})", flush=True)
    return ok


def close(a, b):
    if isinstance(b, float):
        return isinstance(a, (int, float)) and math.isfinite(a) and \
            math.isclose(a, b, abs_tol=ABS_TOL, rel_tol=REL_TOL)
    return type(a) is type(b) and a == b


def same(actual, expected):
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and \
            all(same(x, y) for x, y in zip(actual, expected))
    return close(actual, expected)


class Db:
    def __init__(self, session):
        self.s = session

    def q(self, sql, mode="exact"):
        r = self.s.request(sql, mode)
        if r.get("ok") is not True:
            raise AssertionError(f"statement failed: {sql!r} -> {json.dumps(r, ensure_ascii=False)}")
        return r

    def err(self, sql):
        r = self.s.request(sql)
        code = r.get("error", {}).get("code") if r.get("ok") is False else None
        if not isinstance(code, str) or not code:
            raise AssertionError(f"statement should have failed: {sql!r} -> "
                                 f"{json.dumps(r, ensure_ascii=False)}")
        return code

    def rows(self, sql, mode="exact"):
        r = self.q(sql, mode)
        return r.get("rows", [])


def vec(values):
    return "[" + ",".join(str(v) for v in values) + "]"


# ---------------------------------------------------------------------------
# 1. sql_crud / 13. storage_mutations: many rows, then update/delete/restart
# ---------------------------------------------------------------------------
def case_crud(db):
    n = 300
    rows = []
    truth = {}
    for i in range(1, n + 1):
        content = f"内容{i}" if i % 7 == 0 else f"row-{i}"
        truth[i] = content
        rows.append(f"({i},'{content}')")
    db.q("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16))")
    r = db.q("INSERT INTO docs VALUES " + ",".join(rows))
    ok = r.get("affected_rows") == n
    got = db.rows("SELECT id, content FROM docs ORDER BY id")
    ok &= same([list(x) for x in got], [[i, truth[i]] for i in range(1, n + 1)])

    # update a slice
    for i in range(1, 51):
        truth[i] = "已改"
    r = db.q("UPDATE docs SET content = '已改' WHERE id >= 1 AND id <= 50")
    ok &= r.get("affected_rows") == 50
    # delete a slice
    for i in range(51, 71):
        truth.pop(i, None)
    r = db.q("DELETE FROM docs WHERE id >= 51 AND id <= 70")
    ok &= r.get("affected_rows") == 20
    got = db.rows("SELECT id, content FROM docs ORDER BY id")
    want = [[i, truth[i]] for i in sorted(truth)]
    ok &= same([list(x) for x in got], want)
    return record("crud/hard", ok, f"rows={len(got)} want={len(want)} affected={r.get('affected_rows')}")


def case_storage_mutations(db, session, workdir):
    db.q("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), "
         "embedding VECTOR(3))")
    rows, truth = [], {}
    for i in range(1, 121):
        cat = "甲" if i % 3 == 0 else "乙"
        vecv = [i % 7, (i * 3) % 11, (i * 5) % 13]
        truth[i] = (cat, f"c{i}", vecv)
        rows.append(f"({i},'{cat}','c{i}','{vec(vecv)}')")
    db.q("INSERT INTO docs VALUES " + ",".join(rows))
    # vector updates
    for i in range(1, 21):
        truth[i] = (truth[i][0], truth[i][1], [9, 9, 9])
    db.q("UPDATE docs SET embedding = '[9,9,9]' WHERE id >= 1 AND id <= 20")
    # content updates
    for i in range(21, 41):
        truth[i] = (truth[i][0], "改", truth[i][2])
    db.q("UPDATE docs SET content = '改' WHERE id >= 21 AND id <= 40")
    # deletes
    for i in range(41, 51):
        truth.pop(i, None)
    db.q("DELETE FROM docs WHERE id >= 41 AND id <= 50")
    session.shutdown()
    session.start()
    got = db.rows("SELECT id, category, content, embedding FROM docs ORDER BY id")
    want = [[i, truth[i][0], truth[i][1], [f32(x) for x in truth[i][2]]] for i in sorted(truth)]
    ok = same([list(x) for x in got], want)
    return record("storage_mutations/hard", ok, f"rows={len(got)} want={len(want)}")


# ---------------------------------------------------------------------------
# 2. sql_comparisons / 3. sql_ordering
# ---------------------------------------------------------------------------
def case_comparisons(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT)")
    vals = {i: (i - 40) * 3 for i in range(1, 61)}   # -117 .. 117
    db.q("INSERT INTO t VALUES " + ",".join(f"({i},{vals[i]})" for i in range(1, 61)))
    checks = [
        ("value >= 20 AND value < 30", lambda v: v >= 20 and v < 30),
        ("value = 0", lambda v: v == 0),
        ("value < -100", lambda v: v < -100),
        ("value > 100", lambda v: v > 100),
        ("value <= -117", lambda v: v <= -117),
        ("value >= 117", lambda v: v >= 117),
    ]
    ok = True
    for where, pred in checks:
        got = [x[0] for x in db.rows(f"SELECT id FROM t WHERE {where} ORDER BY id")]
        want = [i for i in range(1, 61) if pred(vals[i])]
        ok &= same(got, want)
        if not same(got, want):
            print(f"    mismatch on {where}: {got[:8]} vs {want[:8]}", flush=True)
    return record("comparisons/hard", ok, "6 predicates over 60 rows")


def case_ordering(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT)")
    # many ties on purpose
    vals = {i: (i % 5) for i in range(1, 41)}
    db.q("INSERT INTO t VALUES " + ",".join(f"({i},{vals[i]})" for i in range(1, 41)))
    ok = True
    for limit in (None, 3, 10, 40, 100):
        sql = "SELECT id FROM t ORDER BY value"
        if limit is not None:
            sql += f" LIMIT {limit}"
        got = [x[0] for x in db.rows(sql)]
        want = sorted(range(1, 41), key=lambda i: (vals[i], i))
        if limit is not None:
            want = want[:limit]
        ok &= same(got, want)
        if not same(got, want):
            print(f"    mismatch LIMIT {limit}: {got[:10]} vs {want[:10]}", flush=True)
    # DESC with ties -> descending value, ascending id
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY value DESC")]
    want = sorted(range(1, 41), key=lambda i: (-vals[i], i))
    ok &= same(got, want)
    # LIMIT with OFFSET
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id LIMIT 5 OFFSET 7")]
    ok &= same(got, list(range(8, 13)))
    return record("ordering/hard", ok, "ties resolved by ascending primary key")


# ---------------------------------------------------------------------------
# 4. sql_empty / 5. sql_primary_key
# ---------------------------------------------------------------------------
def case_empty(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, content VARCHAR(16))")
    ok = db.rows("SELECT id FROM t") == []
    ok &= db.rows("SELECT id, content FROM t ORDER BY id") == []
    r = db.q("DELETE FROM t WHERE id = 1")
    ok &= r.get("affected_rows") == 0
    # a rejected insert must leave the table empty
    db.q("CREATE TABLE v (id BIGINT PRIMARY KEY, embedding VECTOR(2))")
    db.err("INSERT INTO v VALUES (1,'[1,2,3]')")
    ok &= db.rows("SELECT id FROM v") == []
    return record("empty/hard", ok, "empty table stays empty after a rejected insert")


def case_primary_key(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, content VARCHAR(16))")
    db.q("INSERT INTO t VALUES (1,'first')")
    db.err("INSERT INTO t VALUES (1,'second')")
    got = db.rows("SELECT id, content FROM t")
    ok = same([list(x) for x in got], [[1, "first"]])
    # duplicate after an update keeps the original
    db.q("UPDATE t SET content = 'updated' WHERE id = 1")
    db.err("INSERT INTO t VALUES (1,'third')")
    got = db.rows("SELECT id, content FROM t")
    ok &= same([list(x) for x in got], [[1, "updated"]])
    # multi row insert with one duplicate must not insert any of them
    db.q("INSERT INTO t VALUES (2,'two')")
    db.err("INSERT INTO t VALUES (3,'three'),(2,'dup')")
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(got, [1, 2])
    return record("primary_key/hard", ok, f"ids={got}")


# ---------------------------------------------------------------------------
# 6. sql_unicode
# ---------------------------------------------------------------------------
def case_unicode(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, content VARCHAR(4))")
    ok = True
    # exactly 4 code points fits, 5 does not
    db.q("INSERT INTO t VALUES (1,'中文测试')")
    db.err("INSERT INTO t VALUES (2,'中文测试啊')")
    # 4 byte utf-8 (emoji) counts as one code point
    db.q("INSERT INTO t VALUES (3,'😀😀')")
    ok &= same([list(x) for x in db.rows("SELECT id, content FROM t ORDER BY id")],
               [[1, "中文测试"], [3, "😀😀"]])
    # code point ordering
    db.q("CREATE TABLE u (id BIGINT PRIMARY KEY, content VARCHAR(8))")
    words = ["a", "Z", "中", "😀", "é"]
    db.q("INSERT INTO u VALUES " + ",".join(f"({i},'{w}')" for i, w in enumerate(words)))
    got = [x[0] for x in db.rows("SELECT id FROM u ORDER BY content")]
    want = sorted(range(len(words)), key=lambda i: [ord(c) for c in words[i]])
    ok &= same(got, want)
    return record("unicode/hard", ok, f"code point order {got} vs {want}")


# ---------------------------------------------------------------------------
# 7. sql_bigint
# ---------------------------------------------------------------------------
def case_bigint(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT)")
    values = [9007199254740993, -9007199254740993, 9223372036854775807,
              -9223372036854775808, 0, 1, -1, 4294967297]
    db.q("INSERT INTO t VALUES " + ",".join(f"({i},{v})" for i, v in enumerate(values)))
    got = db.rows("SELECT id, value FROM t ORDER BY id")
    ok = same([list(x) for x in got], [[i, v] for i, v in enumerate(values)])
    # comparisons must be exact, not float based
    got = [x[0] for x in db.rows(f"SELECT id FROM t WHERE value > {9007199254740992}")]
    ok &= same(got, [0, 2])
    return record("bigint/hard", ok, f"{len(values)} exact integers")


# ---------------------------------------------------------------------------
# 8. sql_unsupported: every unsupported construct must be an error
# ---------------------------------------------------------------------------
def case_unsupported(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, content VARCHAR(16), embedding VECTOR(2))")
    db.q("CREATE TABLE o (id BIGINT PRIMARY KEY, tag VARCHAR(8))")
    db.q("INSERT INTO o VALUES (1,'t')")
    db.q("INSERT INTO t VALUES (1,'x','[1,0]')")
    unsupported = [
        "ALTER TABLE t ADD extra BIGINT",
        "ALTER TABLE t DROP COLUMN content",
        "ALTER TABLE t MODIFY content VARCHAR(32)",
        "SELECT t.id FROM t JOIN o ON t.id = o.id",
        "SELECT COUNT(*) FROM t",
        "SELECT SUM(id) FROM t",
        "SELECT id FROM t GROUP BY id",
        "SELECT id FROM t WHERE id IN (SELECT id FROM o)",
        "INSERT INTO t VALUES (2,NULL,'[1,0]')",
        "SELECT id FROM t WHERE content IS NULL",
        "INSERT INTO t VALUES (3,'y','[1,0]','extra')",
    ]
    ok = True
    for sql in unsupported:
        try:
            db.err(sql)
        except AssertionError as exc:
            ok = False
            print(f"    {exc}", flush=True)
    # nested transaction must not silently succeed
    try:
        db.err("BEGIN; BEGIN; COMMIT;")
    except AssertionError as exc:
        ok = False
        print(f"    {exc}", flush=True)
    # the table must still be intact and usable
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(got, [1])
    return record("unsupported/hard", ok, f"{len(unsupported) + 1} rejected constructs")


# ---------------------------------------------------------------------------
# 10./15./16. vector cases against a brute force oracle
# ---------------------------------------------------------------------------
def case_vectors(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(4))")
    truth = {}
    rows = []
    for i in range(1, 81):
        cat = "keep" if i % 2 == 0 else "skip"
        v = [i % 5, (i * 3) % 7, (i * 5) % 11, (i * 7) % 13]
        truth[i] = (cat, v)
        rows.append(f"({i},'{cat}','{vec(v)}')")
    db.q("INSERT INTO t VALUES " + ",".join(rows))

    def oracle(query_vec, category, k):
        cands = []
        for i, (cat, v) in truth.items():
            if category is not None and cat != category:
                continue
            cands.append((l2(v, query_vec), i))
        cands.sort(key=lambda p: (p[0], p[1]))
        return [(i, d) for d, i in cands[:k]]

    ok = True
    for query_vec, category, k in [
        ([0, 0, 0, 0], None, 10),
        ([0, 0, 0, 0], "keep", 10),
        ([0, 0, 0, 0], "keep", 100),
        ([0, 0, 0, 0], "absent", 10),
        ([3, 1, 4, 1], "keep", 5),
        ([12, 34, 56, 78], None, 3),
    ]:
        where = "" if category is None else f" WHERE category = '{category}'"
        sql = (f"SELECT id, L2_DISTANCE(embedding,'{vec(query_vec)}') AS d FROM t{where} "
               f"ORDER BY d LIMIT {k}")
        got = db.rows(sql)
        want = oracle(query_vec, category, k)
        ids_ok = [g[0] for g in got] == [w[0] for w in want]
        dist_ok = all(same(g[1], w[1]) for g, w in zip(got, want)) and len(got) == len(want)
        if not (ids_ok and dist_ok):
            ok = False
            print(f"    mismatch q={query_vec} cat={category} k={k}: "
                  f"got {[g[0] for g in got][:6]} want {[w[0] for w in want][:6]}", flush=True)
    return record("vectors/hard", ok, "6 TopK shapes against a brute force oracle")


def case_vector_float32(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, embedding VECTOR(2))")
    raw = [[16777217, 0], [16777216, 0], [0.1, 0.2], [1e20, 1], [3.4e38, 0]]
    db.q("INSERT INTO t VALUES " + ",".join(f"({i},'{vec(v)}')" for i, v in enumerate(raw)))
    query = [16777216, 0]
    got = db.rows(f"SELECT id, L2_DISTANCE(embedding,'{vec(query)}') AS d FROM t ORDER BY d")
    cands = sorted(((l2(v, query), i) for i, v in enumerate(raw)), key=lambda p: (p[0], p[1]))
    ok = same([g[0] for g in got], [c[1] for c in cands])
    ok &= all(same(g[1], c[0]) for g, c in zip(got, cands))
    return record("vector_float32/hard", ok, "float32 rounding of stored values")


# ---------------------------------------------------------------------------
# 11./12./18./19./20. transaction cases
# ---------------------------------------------------------------------------
def case_transactions(db, session):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), "
         "embedding VECTOR(2))")
    db.q("INSERT INTO t VALUES (1,'a','one','[0,0]'),(2,'b','two','[1,0]'),(3,'a','three','[0,1]')")

    # commit: the transaction sees its own writes, including vector writes
    db.q("BEGIN")
    db.q("UPDATE t SET content = 'c1' WHERE id = 1")
    db.q("UPDATE t SET embedding = '[3,4]' WHERE id = 2")
    db.q("INSERT INTO t VALUES (4,'b','four','[2,2]')")
    inside = db.rows("SELECT id FROM t ORDER BY id")
    db.q("COMMIT")
    ok = same([x[0] for x in inside], [1, 2, 3, 4])
    got = db.rows("SELECT id, content FROM t ORDER BY id")
    ok &= same([list(x) for x in got], [[1, "c1"], [2, "two"], [3, "three"], [4, "four"]])
    # the vector update is visible after the commit
    d = db.rows("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM t WHERE id = 2")
    ok &= same(d[0][1], 5.0)

    # rollback: nothing of the transaction survives
    db.q("BEGIN")
    db.q("UPDATE t SET content = 'rolled' WHERE id = 1")
    db.q("DELETE FROM t WHERE id = 3")
    db.q("INSERT INTO t VALUES (5,'x','five','[0,0]')")
    db.q("ROLLBACK")
    got = db.rows("SELECT id, content FROM t ORDER BY id")
    ok &= same([list(x) for x in got], [[1, "c1"], [2, "two"], [3, "three"], [4, "four"]])

    # failed insert inside a transaction: the earlier statements stay committable
    db.q("BEGIN")
    db.q("UPDATE t SET content = 'kept' WHERE id = 1")
    db.err("INSERT INTO t VALUES (6,'x','dup','[0,0]'),(1,'x','dupkey','[0,0]')")
    inside = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(inside, [1, 2, 3, 4])
    db.q("COMMIT")
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(got, [1, 2, 3, 4])

    # autocommit atomicity: a rejected multi row insert inserts nothing
    db.err("INSERT INTO t VALUES (7,'x','ok','[0,0]'),(8,'x','bad','[1,2,3]')")
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(got, [1, 2, 3, 4])

    # shutdown rolls the open transaction back
    db.q("BEGIN")
    db.q("DELETE FROM t WHERE id >= 1")
    db.q("INSERT INTO t VALUES (9,'z','nine','[0,0]')")
    session.shutdown()
    session.start()
    got = [x[0] for x in db.rows("SELECT id FROM t ORDER BY id")]
    ok &= same(got, [1, 2, 3, 4])

    # a vector query inside a transaction must see the transaction's own
    # vector update ("事务内读到自己的修改；表数据与向量检索一致")
    db.q("BEGIN")
    db.q("UPDATE t SET embedding = '[9,9]' WHERE id = 3")
    inside = db.rows("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM t WHERE id = 3")
    ok &= same(inside[0][1], math.sqrt(162.0))
    db.q("ROLLBACK")
    after = db.rows("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM t WHERE id = 3")
    ok &= same(after[0][1], 1.0)     # row 3 keeps its original [0,1] vector
    return record("transactions/hard", ok, f"ids after restart={got}")


# ---------------------------------------------------------------------------
# 13./14./17. storage and index maintenance
# ---------------------------------------------------------------------------
def case_multiple_tables(db, session):
    for name, cols in (("a", "id BIGINT PRIMARY KEY, v BIGINT"),
                       ("b", "id BIGINT PRIMARY KEY, v VARCHAR(16)"),
                       ("c", "id BIGINT PRIMARY KEY, e VECTOR(2)")):
        db.q(f"CREATE TABLE {name} ({cols})")
    db.q("INSERT INTO a VALUES (1,10),(2,20),(3,30)")
    db.q("INSERT INTO b VALUES (1,'x'),(2,'y')")
    db.q("INSERT INTO c VALUES (1,'[0,0]'),(2,'[3,4]')")
    session.shutdown()
    session.start()
    ok = same([list(x) for x in db.rows("SELECT id, v FROM a ORDER BY id")],
              [[1, 10], [2, 20], [3, 30]])
    ok &= same([list(x) for x in db.rows("SELECT id, v FROM b ORDER BY id")],
               [[1, "x"], [2, "y"]])
    ok &= same([list(x) for x in db.rows("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM c ORDER BY d")],
               [[1, 0.0], [2, 5.0]])
    return record("multiple_tables/hard", ok, "3 tables survive a restart")


def case_index_maintenance(db, session):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(3))")
    truth = {}
    rows = []
    for i in range(1, 61):
        cat = "keep" if i % 2 == 0 else "skip"
        v = [i % 4, (i * 2) % 6, (i * 3) % 8]
        truth[i] = (cat, v)
        rows.append(f"({i},'{cat}','{vec(v)}')")
    db.q("INSERT INTO t VALUES " + ",".join(rows))
    db.q("CREATE VECTOR INDEX tv ON t(embedding)")
    # churn after the index exists
    for i in range(1, 11):
        truth[i] = (truth[i][0], [9, 9, 9])
    db.q("UPDATE t SET embedding = '[9,9,9]' WHERE id >= 1 AND id <= 10")
    for i in range(21, 31):
        truth.pop(i, None)
    db.q("DELETE FROM t WHERE id >= 21 AND id <= 30")
    for i in range(101, 111):
        truth[i] = ("keep", [1, 2, 3])
    db.q("INSERT INTO t VALUES " + ",".join(f"({i},'keep','[1,2,3]')" for i in range(101, 111)))
    session.shutdown()
    session.start()

    def oracle(query_vec, category, k):
        cands = sorted(((l2(v, query_vec), i) for i, (cat, v) in truth.items()
                        if category is None or cat == category), key=lambda p: (p[0], p[1]))
        return [(i, d) for d, i in cands[:k]]

    ok = True
    for mode in ("exact", "ann"):
        got = db.rows("SELECT id, L2_DISTANCE(embedding,'[0,0,0]') AS d FROM t "
                      "WHERE category = 'keep' ORDER BY d LIMIT 10", mode)
        want = oracle([0, 0, 0], "keep", 10)
        ids_ok = [g[0] for g in got] == [w[0] for w in want]
        # an approximate search may legitimately return a different subset, but
        # it must be valid, duplicate free and inside the filtered set
        valid = len({g[0] for g in got}) == len(got)
        allowed = all(truth.get(g[0], (None,))[0] == "keep" for g in got)
        dist_ok = all(same(g[1], l2(truth[g[0]][1], [0, 0, 0])) for g in got)
        if not (valid and allowed and dist_ok) or (mode == "exact" and not ids_ok):
            ok = False
            print(f"    {mode}: ids={[g[0] for g in got][:8]} valid={valid} "
                  f"allowed={allowed} dist_ok={dist_ok} exact_match={ids_ok}", flush=True)
    # dropping the index must work as well
    db.q("DROP INDEX tv ON t")
    db.q("CREATE VECTOR INDEX tv ON t(embedding)")
    db.q("DROP INDEX tv")
    return record("index_maintenance/hard", ok, "index churn + restart + exact/ann agreement")


def case_sql_forms(db):
    """The ordinary SQL spellings the task statement asks for.

    The public pack only ever uses a handful of shapes; the formal pack is the
    same cases made harder and may well use the other obvious spellings.
    """
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(8), v BIGINT, e VECTOR(2))")
    required = [
        ("insert column list", "INSERT INTO t (id, tag, v, e) VALUES (1,'a',10,'[1,0]')", True),
        ("insert multi row", "INSERT INTO t VALUES (2,'b',20,'[0,1]'),(3,'c',30,'[1,1]')", True),
        ("select qualified", "SELECT t.id FROM t", True),
        ("select alias", "SELECT id AS k, v AS val FROM t", True),
        ("where paren", "SELECT id FROM t WHERE (v = 10 OR v = 20) AND id > 0", True),
        ("where not", "SELECT id FROM t WHERE NOT (v = 10)", True),
        ("order two keys", "SELECT id FROM t ORDER BY tag, v DESC", True),
        ("order by alias", "SELECT id, v AS val FROM t ORDER BY val", True),
        ("limit zero", "SELECT id FROM t ORDER BY id LIMIT 0", True),
        ("limit beyond", "SELECT id FROM t ORDER BY id LIMIT 1000", True),
        ("update no match", "UPDATE t SET v = 13 WHERE id = 999", True),
        ("delete no match", "DELETE FROM t WHERE id = 999", True),
        ("vector index", "CREATE VECTOR INDEX tv ON t(e)", True),
        ("drop index", "DROP INDEX tv ON t", True),
        ("create if not exists", "CREATE TABLE IF NOT EXISTS t (id BIGINT PRIMARY KEY)", True),
        ("drop table if exists", "DROP TABLE IF EXISTS t", True),
    ]
    optional = [
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
        ("null value", "INSERT INTO t VALUES (9,'n',NULL,'[1,1]')"),
        ("update pk", "UPDATE t SET id = 77 WHERE id = 9"),
    ]
    ok = True
    for name, sql, _ in required:
        try:
            db.q(sql)
        except AssertionError as exc:
            ok = False
            print(f"    {exc}", flush=True)
    for name, sql in optional:
        try:
            db.err(sql)
        except AssertionError as exc:
            ok = False
            print(f"    {exc}", flush=True)
    return record("forms/hard", ok, f"{len(required)} supported + {len(optional)} rejected spellings")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/hardened")
    ap.add_argument("--image", default=None)
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)

    # The package states that every case starts from an empty database and its
    # own data directory, so each case gets a fresh session here as well.
    def run_case(name, fn):
        case_dir = work / name
        case_dir.mkdir(parents=True, exist_ok=True)
        log = case_dir / "stderr.log"
        if log.exists():
            log.unlink()
        if args.image:
            session = Session(CONFIG, str(log), image=args.image)
        else:
            session = Session(CONFIG, str(log),
                              command=[BIN, "--data-dir", str(case_dir), "--protocol", "jsonl"])
        try:
            session.start()
            fn(Db(session), session)
        except Exception as exc:  # noqa: BLE001
            record(name, False, f"exception: {type(exc).__name__}: {exc}")
        finally:
            try:
                session.shutdown()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass

    steps = [
        ("crud", lambda db, s: case_crud(db)),
        ("comparisons", lambda db, s: case_comparisons(db)),
        ("ordering", lambda db, s: case_ordering(db)),
        ("empty", lambda db, s: case_empty(db)),
        ("primary_key", lambda db, s: case_primary_key(db)),
        ("unicode", lambda db, s: case_unicode(db)),
        ("bigint", lambda db, s: case_bigint(db)),
        ("unsupported", lambda db, s: case_unsupported(db)),
        ("forms", lambda db, s: case_sql_forms(db)),
        ("vectors", lambda db, s: case_vectors(db)),
        ("vector_float32", lambda db, s: case_vector_float32(db)),
        ("transactions", lambda db, s: case_transactions(db, s)),
        ("multiple_tables", lambda db, s: case_multiple_tables(db, s)),
        ("index_maintenance", lambda db, s: case_index_maintenance(db, s)),
        ("storage_mutations", lambda db, s: case_storage_mutations(db, s, work)),
    ]
    for name, fn in steps:
        run_case(name, fn)

    out = work / "hardened.json"
    out.write_text(json.dumps(RESULTS, ensure_ascii=False, indent=2) + "\n")
    failed = [r for r in RESULTS if not r["ok"]]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} hardened cases ok -> {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
