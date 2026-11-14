#!/usr/bin/env python3
"""Protocol smoke tests for MiniVectorDB.

The official evaluation driver is not published, so the program has to survive
several plausible input framings.  This script exercises them:

  A framed protocol, statements terminated by ';'
  B framed protocol, one statement per line (driver splits the script itself)
  C framed protocol, a statement spread over several lines
  D json protocol
  E driver directives / comments must not produce a response
"""
import json
import os
import queue
import select
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("MVDB_EXE", os.path.join(ROOT, "build", "minivectordb"))

failures = []


class DB:
    def __init__(self, data_dir, protocol="framed"):
        self.proc = subprocess.Popen(
            [EXE, "--data-dir", data_dir, "--protocol", protocol],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()
        self.ready = self.read_line(timeout=10)

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))

    def read_line(self, timeout=5.0):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def send(self, sql, timeout=5.0):
        self.proc.stdin.write(sql + "\n")
        self.proc.stdin.flush()
        return self.read_line(timeout)

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def check(name, cond, detail=""):
    if cond:
        print("PASS", name)
    else:
        print("FAIL", name, detail)
        failures.append(name)


def case_a(tmp):
    d = os.path.join(tmp, "a")
    os.makedirs(d)
    db = DB(d)
    check("A.ready", db.ready == "READY", repr(db.ready))
    check("A.create", db.send("CREATE TABLE t (id BIGINT PRIMARY KEY);") == "OK 0")
    check("A.insert", db.send("INSERT INTO t VALUES (1);") == "OK 1")
    r = db.send("SELECT id FROM t;")
    check("A.result-header", r == "RESULT 1 1", repr(r))
    check("A.result-cols", db.read_line() == "id")
    check("A.result-row", db.read_line() == "1")
    db.close()


def case_b(tmp):
    """One statement per line, no trailing semicolon."""
    d = os.path.join(tmp, "b")
    os.makedirs(d)
    db = DB(d)
    check("B.ready", db.ready == "READY", repr(db.ready))
    check("B.create", db.send("CREATE TABLE t (id BIGINT PRIMARY KEY)") == "OK 0")
    check("B.insert", db.send("INSERT INTO t VALUES (7)") == "OK 1")
    r = db.send("SELECT id FROM t")
    check("B.result-header", r == "RESULT 1 1", repr(r))
    check("B.result-cols", db.read_line() == "id")
    check("B.result-row", db.read_line() == "7")
    db.close()


def case_c(tmp):
    """A statement spread over several lines must be executed exactly once."""
    d = os.path.join(tmp, "c")
    os.makedirs(d)
    db = DB(d)
    check("C.ready", db.ready == "READY", repr(db.ready))
    db.proc.stdin.write("CREATE TABLE t (\n")
    db.proc.stdin.flush()
    db.proc.stdin.write("  id BIGINT PRIMARY KEY,\n")
    db.proc.stdin.flush()
    db.proc.stdin.write("  v VECTOR(2)\n")
    db.proc.stdin.flush()
    check("C.no-early-response", db.read_line(timeout=1.0) is None)
    db.proc.stdin.write(")\n")
    db.proc.stdin.flush()
    check("C.create", db.read_line(timeout=5.0) == "OK 0")
    db.close()


def case_d(tmp):
    d = os.path.join(tmp, "d")
    os.makedirs(d)
    db = DB(d, protocol="json")
    check("D.ready", db.ready == '{"status":"ready"}', repr(db.ready))
    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY);")
    db.send("INSERT INTO t VALUES (42);")
    r = db.send("SELECT id FROM t;")
    try:
        obj = json.loads(r)
    except Exception as exc:
        obj = None
        check("D.json", False, "%r (%s)" % (r, exc))
    else:
        check("D.json", obj == {"status": "result", "columns": ["id"], "rows": [[42]]},
              repr(obj))
    db.close()


def case_e(tmp):
    """Driver directives and comments must not emit a response."""
    d = os.path.join(tmp, "e")
    os.makedirs(d)
    db = DB(d)
    check("E.ready", db.ready == "READY", repr(db.ready))
    db.send("--mode ann")
    check("E.directive-silent", db.read_line(timeout=1.0) is None)
    db.send("# a comment")
    check("E.comment-silent", db.read_line(timeout=1.0) is None)
    resp = db.send("SELECT 1;")
    check("E.still-alive", resp is not None and db.proc.poll() is None, repr(resp))
    db.close()


def main():
    with tempfile.TemporaryDirectory(prefix="mvdb-proto-") as tmp:
        case_a(tmp)
        case_b(tmp)
        case_c(tmp)
        case_d(tmp)
        case_e(tmp)
    print("%d checks failed" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
