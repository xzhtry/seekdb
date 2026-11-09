#!/usr/bin/env python3
"""Run the official public MiniVectorDB cases against the implementation.

Usage: python3 tests/run_public_cases.py [mysqltest_dir]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("MVDB_EXE", os.path.join(ROOT, "build", "minivectordb"))
CASES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "mysqltest")
CASES = os.path.abspath(CASES)


class DB:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.proc = None
        self.start()

    def start(self):
        self.proc = subprocess.Popen(
            [EXE, "--data-dir", self.data_dir, "--protocol", "framed"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        ready = self.proc.stdout.readline().strip()
        assert ready == "READY", "bad ready %r" % ready

    def send(self, sql):
        self.proc.stdin.write(sql + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("eof stderr=%r" % self.proc.stderr.read())
            line = line.rstrip("\n")
            if line.startswith("OK"):
                return ("OK", int(line.split()[1]) if len(line.split()) > 1 else 0)
            if line.startswith("ERROR"):
                parts = line.split(" ", 2)
                return ("ERR", int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0,
                        parts[2] if len(parts) > 2 else "")
            if line.startswith("RESULT"):
                ncols, nrows = map(int, line.split()[1:])
                header = self.proc.stdout.readline().rstrip("\n").split("\t") if ncols else []
                rows = [self.proc.stdout.readline().rstrip("\n").split("\t") for _ in range(nrows)]
                return ("RES", header, rows)
            raise RuntimeError("bad response %r" % line)

    def restart(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.wait(timeout=30)
        err = self.proc.stderr.read()
        if err.strip():
            raise RuntimeError("restart stderr: " + err[:500])
        self.start()

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.wait(timeout=30)


def parse_num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)


def value_equal(expected, got):
    if isinstance(expected, bool):
        return str(expected).lower() == got.lower()
    if isinstance(expected, int):
        try:
            return int(got) == expected
        except ValueError:
            return False
    if isinstance(expected, float):
        try:
            g = float(got)
        except ValueError:
            return False
        return abs(g - expected) <= max(1e-5, 1e-5 * abs(expected))
    return str(expected) == got


def rows_equal(expected, got, unordered):
    if len(expected) != len(got):
        return False
    def norm(r):
        return tuple(r)
    if unordered:
        return sorted(map(norm, expected)) == sorted(map(norm, got))
    return all(len(e) == len(g) and all(value_equal(a, b) for a, b in zip(e, g))
               for e, g in zip(expected, got))


def run_case(path, data_dir):
    with open(path + ".test", encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    with open(path + ".result", encoding="utf-8") as f:
        expected = json.load(f)
    db = DB(data_dir)
    ei = 0
    errors = []
    try:
        for line in lines:
            line = line.strip()
            if line == "--restart":
                db.restart()
                continue
            if line.startswith("--mode"):
                db.send("SET search_mode='%s';" % line.split()[-1])
                continue
            if ei >= len(expected):
                errors.append("too many assertions at %r" % line)
                break
            exp = expected[ei]
            ei += 1
            got = db.send(line)
            if "error" in exp:
                if got[0] != "ERR" or got[1] == 0:
                    errors.append("expected error for %r, got %r" % (line, got))
                continue
            if got[0] == "ERR":
                errors.append("unexpected error for %r: %r" % (line, got))
                continue
            if "affected_rows" in exp:
                if got[0] != "OK" or got[1] != exp["affected_rows"]:
                    errors.append("affected_rows for %r expected %r got %r" %
                                  (line, exp["affected_rows"], got))
                continue
            if "columns" in exp or "rows" in exp:
                if got[0] != "RES":
                    errors.append("expected result for %r got %r" % (line, got))
                    continue
                header, rows = got[1], got[2]
                if "columns" in exp and header != exp["columns"]:
                    errors.append("columns for %r expected %r got %r" %
                                  (line, exp["columns"], header))
                if "rows" in exp:
                    exp_rows = exp["rows"]
                    if not rows_equal(exp_rows, rows, exp.get("unordered", False)):
                        errors.append("rows for %r\nexpected %r\ngot      %r" %
                                      (line, exp_rows, rows))
    finally:
        db.close()
    if ei != len(expected):
        errors.append("assertion count mismatch: consumed %d of %d" % (ei, len(expected)))
    return errors


def main():
    manifest = json.load(open(os.path.join(CASES, "manifest.json"), encoding="utf-8"))
    total = 0
    failed = 0
    with tempfile.TemporaryDirectory(prefix="mvdb-public-") as base:
        for case in manifest["cases"]:
            path = os.path.join(CASES, case["test"])[:-5]
            d = os.path.join(base, case["id"])
            os.makedirs(d)
            errs = run_case(path, d)
            total += 1
            if errs:
                failed += 1
                print("FAIL", case["id"])
                for e in errs[:10]:
                    print("  ", e)
            else:
                print("PASS", case["id"])
    print("%d/%d cases passed" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
