#!/usr/bin/env python3
"""Exploratory probe: what do we print for expressions and vector columns?

The hidden cases compare `columns` names and `rows` values exactly, so the
shape of an unaliased expression header and of a vector value matters.
"""
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
    with tempfile.TemporaryDirectory(prefix="mvdb-probe-") as tmp:
        db = DB(tmp)
        db.query("CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(16), v VECTOR(3));")
        db.query("INSERT INTO t VALUES (1, 'a', '[1, 2.5, -3]');")
        probes = [
            "SELECT * FROM t;",
            "SELECT v FROM t;",
            "SELECT L2_DISTANCE(v, '[0,0,0]') FROM t;",
            "SELECT id, L2_DISTANCE(v, '[0,0,0]') AS d FROM t;",
            "SELECT id+1 FROM t;",
            "SELECT id * 2, name FROM t;",
            "SELECT v * '[1,1,1]' FROM t;",
            "SHOW TABLES;",
            "DESCRIBE t;",
            "SELECT id FROM t WHERE name = 'a';",
            "SELECT id FROM t WHERE id IN (1,2);",
            "SELECT id FROM t LIMIT 1 OFFSET 0;",
            "SELECT COUNT(*) FROM t;",
            "SELECT id FROM t WHERE v IS NULL;",
            "INSERT INTO t VALUES (2, 'b', NULL);",
            "INSERT INTO t VALUES (3, 'b', '[1,2]');",
            "INSERT INTO t VALUES (4, 'b', '[1,2,NaN]');",
            "INSERT INTO t VALUES (5, 'b', '[1,2,Infinity]');",
            "SELECT id FROM t WHERE id = 1;",
            "UPDATE t SET id = 9 WHERE id = 1;",
            "BEGIN;",
            "BEGIN;",
            "COMMIT;",
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
