#!/usr/bin/env python3
"""Self-test for MiniVectorDB.

Run from the repository root after ./build.sh:
    python3 tests/selftest.py
"""
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("MVDB_EXE", os.path.join(ROOT, "build", "minivectordb"))


class DB:
    def __init__(self, data_dir, extra_env=None):
        env = dict(os.environ)
        if extra_env:
            env.update(extra_env)
        self.proc = subprocess.Popen(
            [EXE, "--data-dir", data_dir, "--protocol", "framed"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )
        ready = self.proc.stdout.readline().strip()
        if ready != "READY":
            raise RuntimeError("did not become ready: %r" % ready)

    def send(self, sql):
        self.proc.stdin.write(sql + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("unexpected EOF (stderr=%r)" % self.proc.stderr.read())
            line = line.rstrip("\n")
            if line.startswith("OK"):
                return ("OK", int(line.split()[1]))
            if line.startswith("ERROR"):
                parts = line.split(" ", 2)
                return ("ERR", int(parts[1]), parts[2] if len(parts) > 2 else "")
            if line.startswith("RESULT"):
                ncols, nrows = map(int, line.split()[1:])
                header = self.proc.stdout.readline().rstrip("\n").split("\t")
                rows = [self.proc.stdout.readline().rstrip("\n").split("\t") for _ in range(nrows)]
                return ("RES", header, rows)
            raise RuntimeError("bad response line: %r" % line)

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.wait(timeout=30)
        err = self.proc.stderr.read()
        if err.strip():
            print("stderr:", err[:1000], file=sys.stderr)

    def kill(self):
        self.proc.kill()
        self.proc.wait()


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def test_basic(tmp):
    db = DB(tmp)
    expect(db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(5), v VECTOR(3));")[0] == "OK", "create")
    expect(db.send("INSERT INTO t VALUES (1,'héllo','[1,2,3]'),(2,'abc','[4,5,6]');") == ("OK", 2), "insert")
    st = db.send("SELECT * FROM t ORDER BY id;")
    expect(st[0] == "RES" and st[2][0] == ["1", "héllo", "[1,2,3]"], "select *")
    expect(db.send("INSERT INTO t VALUES (1,'dup','[0,0,0]');")[1] == 1062, "dup pk")
    expect(db.send("INSERT INTO t VALUES (3,'toolong','[1,1,1]');")[1] == 1406, "varchar length")
    expect(db.send("INSERT INTO t VALUES (4,'ok','[1,2]');")[1] == 1366, "vector dim")
    expect(db.send("INSERT INTO t VALUES (5,'ok','[nan,1,1]');")[1] == 1366, "nan")
    expect(db.send("INSERT INTO t VALUES (6,'ok','[1e999,1,1]');")[1] == 1366, "overflow")
    st = db.send("SELECT id FROM t WHERE name >= 'b' ORDER BY id;")
    expect(st[2] == [["1"]], "utf8 compare")
    db.close()

    db = DB(tmp)
    st = db.send("SELECT * FROM t ORDER BY id;")
    expect(len(st[2]) == 2, "persistence")
    db.close()


def test_txn(tmp):
    db = DB(tmp)
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));")
    db.send("INSERT INTO t VALUES (1,'[1,0]'),(2,'[0,1]');")
    expect(db.send("BEGIN;")[0] == "OK", "begin")
    db.send("UPDATE t SET v='[0,0]' WHERE id=1;")
    db.send("DELETE FROM t WHERE id=2;")
    st = db.send("SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d;")
    expect([r[0] for r in st[2]] == ["1"], "txn sees own changes")
    db.send("ROLLBACK;")
    st = db.send("SELECT id FROM t ORDER BY id;")
    expect([r[0] for r in st[2]] == ["1", "2"], "rollback")
    db.send("BEGIN;")
    db.send("INSERT INTO t VALUES (3,'[3,3]');")
    db.send("COMMIT;")
    st = db.send("SELECT id FROM t ORDER BY id;")
    expect([r[0] for r in st[2]] == ["1", "2", "3"], "commit")
    db.close()

    db = DB(tmp)
    st = db.send("SELECT id FROM t ORDER BY id;")
    expect([r[0] for r in st[2]] == ["1", "2", "3"], "txn persistence")
    db.close()


def test_errors(tmp):
    db = DB(tmp)
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));")
    expect(db.send("SELECT * FROM nope;")[1] == 1146, "unknown table")
    expect(db.send("SELECT bad FROM t;")[1] == 1054, "unknown column")
    expect(db.send("INSERT INTO t VALUES (1,'[1,1]');")[0] == "OK", "insert")
    expect(db.send("BEGIN;")[0] == "OK", "begin")
    db.send("INSERT INTO t VALUES (2,'[2,2]');")
    expect(db.send("INSERT INTO t VALUES (1,'[0,0]');")[1] == 1062, "dup in txn")
    st = db.send("SELECT id FROM t ORDER BY id;")
    expect([r[0] for r in st[2]] == ["1", "2"], "txn continues after error")
    expect(db.send("CREATE TABLE x (id BIGINT PRIMARY KEY);")[0] == "OK", "ddl implicit commit")
    db.send("ROLLBACK;")
    expect(db.send("SELECT id FROM t ORDER BY id;")[2] == [["1"], ["2"]], "implicit commit kept rows")
    db.send("DROP TABLE x;")
    expect(db.send("COMMIT;")[0] == "OK", "commit without txn is a no-op")
    expect(db.send("ROLLBACK;")[0] == "OK", "rollback without txn is a no-op")
    db.close()


def test_vector(tmp):
    db = DB(tmp)
    random.seed(123)
    dim = 16
    n = 400
    rows = []
    for i in range(n):
        v = [f32(random.uniform(-1, 1)) for _ in range(dim)]
        rows.append((i + 1, "cat%d" % (i % 5), v))
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), v VECTOR(16));")
    for b in range(0, n, 50):
        vals = ",".join(
            "(%d,'%s','[%s]')" % (i, c, ",".join("%.7f" % x for x in v))
            for i, c, v in rows[b:b + 50]
        )
        expect(db.send("INSERT INTO t VALUES " + vals + ";")[0] == "OK", "vector insert")
    db.send("CREATE VECTOR INDEX vi ON t(v);")
    db.send("SET search_mode='ann';")
    for q in range(10):
        qv = [f32(random.uniform(-1, 1)) for _ in range(dim)]
        qs = "[" + ",".join("%.7f" % x for x in qv) + "]"
        filt = q % 2 == 0
        where = " WHERE c='cat3'" if filt else ""
        st = db.send("SELECT id, L2_DISTANCE(v, '%s') AS d FROM t%s ORDER BY d LIMIT 10;" % (qs, where))
        got = [int(r[0]) for r in st[2]]
        truth = []
        for i, c, v in rows:
            if filt and c != "cat3":
                continue
            d = math.sqrt(sum((a - b) ** 2 for a, b in zip(v, qv)))
            truth.append((d, i))
        truth = [i for _, i in sorted(truth)[:10]]
        expect(got == truth, "topk mismatch q=%d" % q)
    # tie -> pk ascending
    db.send("CREATE TABLE tie (id BIGINT PRIMARY KEY, v VECTOR(2));")
    db.send("INSERT INTO tie VALUES (5,'[1,1]'),(2,'[1,1]'),(9,'[1,1]'),(1,'[1,1]');")
    st = db.send("SELECT id FROM tie ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 3;")
    expect([r[0] for r in st[2]] == ["1", "2", "5"], "distance tie")
    db.close()


def test_crash(tmp):
    db = DB(tmp)
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16));")
    db.send("INSERT INTO t VALUES (1,'a'),(2,'b');")
    db.send("BEGIN;")
    db.send("INSERT INTO t VALUES (3,'c');")
    db.send("COMMIT;")
    db.send("BEGIN;")
    db.send("INSERT INTO t VALUES (4,'d');")
    db.send("UPDATE t SET s='x' WHERE id=1;")
    db.send("DELETE FROM t WHERE id=2;")
    db.kill()

    db = DB(tmp)
    st = db.send("SELECT id, s FROM t ORDER BY id;")
    expect(st[2] == [["1", "a"], ["2", "b"], ["3", "c"]], "crash recovery")
    db.close()

    # crash repeatedly during recovery
    for _ in range(3):
        p = subprocess.Popen(
            [EXE, "--data-dir", tmp, "--protocol", "framed"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(0.03)
        p.kill()
        p.wait()
    db = DB(tmp)
    st = db.send("SELECT id, s FROM t ORDER BY id;")
    expect(st[2] == [["1", "a"], ["2", "b"], ["3", "c"]], "repeated crash recovery")
    db.close()


def test_large_stream(tmp):
    db = DB(tmp, {"MVDB_CACHE_MB": "1"})
    random.seed(77)
    n, dim = 3000, 32
    rows = []
    for i in range(n):
        v = [f32(random.uniform(-1, 1)) for _ in range(dim)]
        rows.append((i + 1, v))
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(32));")
    for b in range(0, n, 100):
        vals = ",".join(
            "(%d,'[%s]')" % (i, ",".join("%.7f" % x for x in v))
            for i, v in rows[b:b + 100]
        )
        db.send("INSERT INTO t VALUES " + vals + ";")
    qv = [f32(random.uniform(-1, 1)) for _ in range(dim)]
    qs = "[" + ",".join("%.7f" % x for x in qv) + "]"
    st = db.send("SELECT id, L2_DISTANCE(v, '%s') AS d FROM t ORDER BY d LIMIT 5;" % qs)
    got = [int(r[0]) for r in st[2]]
    truth = sorted(
        (math.sqrt(sum((a - b) ** 2 for a, b in zip(v, qv))), i) for i, v in rows
    )[:5]
    expect(got == [i for _, i in truth], "streaming topk")
    db.close()


def main():
    if not os.path.exists(EXE):
        print("missing executable:", EXE)
        return 1
    random.seed(1)
    with tempfile.TemporaryDirectory(prefix="mvdb-selftest-") as base:
        tests = [
            ("basic", test_basic),
            ("txn", test_txn),
            ("errors", test_errors),
            ("vector", test_vector),
            ("crash", test_crash),
            ("stream", test_large_stream),
        ]
        for name, fn in tests:
            d = os.path.join(base, name)
            os.makedirs(d)
            t0 = time.time()
            fn(d)
            print("PASS %-8s %.3fs" % (name, time.time() - t0))
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
