#!/usr/bin/env python3
"""Probe the SQL corners the hidden cases are most likely to touch."""
import os
import subprocess
import sys
import tempfile
import threading
import queue

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("MVDB_EXE", os.path.join(ROOT, "build", "minivectordb"))


class DB:
    def __init__(self, data_dir):
        self.proc = subprocess.Popen(
            [EXE, "--data-dir", data_dir, "--protocol", "framed"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.ready = self.read_line(10)

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))

    def read_line(self, timeout=5.0):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def query(self, sql):
        self.proc.stdin.write(sql + "\n")
        self.proc.stdin.flush()
        head = self.read_line()
        if head is None:
            return ["<timeout>"]
        if head.startswith("RESULT"):
            ncols, nrows = map(int, head.split()[1:])
            cols = self.read_line()
            rows = [self.read_line() for _ in range(nrows)]
            return [head, cols] + rows
        return [head]


def main():
    with tempfile.TemporaryDirectory(prefix="mvdb-probe2-") as tmp:
        db = DB(tmp)
        print("--- setup")
        for line in db.query("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8), f BIGINT, v VECTOR(2));"):
            print("    %s" % line)
        for line in db.query("INSERT INTO t VALUES (1, 'a', 15, '[1,0]'), (2, 'bb', 25, '[0,1]'), (3, 'ccc', 35, '[1,1]');"):
            print("    %s" % line)
        probes = [
            # ordering shapes
            "SELECT id FROM t ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 2;",
            "SELECT id FROM t ORDER BY s LIMIT 2;",
            "SELECT id FROM t ORDER BY id DESC LIMIT 1;",
            "SELECT id FROM t ORDER BY f DESC LIMIT 1;",
            "SELECT id FROM t LIMIT 1, 1;",
            "SELECT id FROM t ORDER BY 1 LIMIT 1;",
            "SELECT id FROM t ORDER BY nosuch LIMIT 1;",
            # expression / alias shapes
            "SELECT id AS k FROM t ORDER BY k LIMIT 1;",
            "SELECT id, s FROM t WHERE s > 'a' ORDER BY id;",
            # vector shapes
            "SELECT L2_DISTANCE(v, '[0,0,0]') FROM t;",
            "SELECT L2_DISTANCE(v, '[0, 0]') FROM t LIMIT 1;",
            "SELECT id FROM t WHERE v = '[1,0]';",
            # dml shapes
            "UPDATE t SET f = f + 1 WHERE id = 1;",
            "UPDATE t SET s = 'zz' WHERE id = 99;",
            "DELETE FROM t WHERE id = 99;",
            "INSERT INTO t (id, s) VALUES (4, 'd');",
            # limits and ranges
            "INSERT INTO t VALUES (5, 'toolongvalue', 1, '[0,0]');",
            "INSERT INTO t VALUES (9223372036854775807, 'x', 1, '[0,0]');",
            "INSERT INTO t VALUES (9223372036854775808, 'x', 1, '[0,0]');",
            "INSERT INTO t VALUES (6, 'x', 3.5e38, '[0,0]');",
            "INSERT INTO t VALUES (7, 'x', 1, '[1e40,0]');",
            "INSERT INTO t VALUES (8, 'x', 1, '[1.5e-8,0]');",
            # transaction corners
            "BEGIN;",
            "BEGIN;",
            "INSERT INTO t VALUES (10, 'x', 1, '[0,0]');",
            "CREATE TABLE t2 (id BIGINT PRIMARY KEY);",
            "SELECT id FROM t WHERE id = 10;",
            "ROLLBACK;",
            "SELECT id FROM t WHERE id = 10;",
        ]
        for p in probes:
            out = db.query(p)
            print("--- %s" % p)
            for line in out:
                print("    %s" % line)
        db.proc.stdin.close()
        db.proc.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
