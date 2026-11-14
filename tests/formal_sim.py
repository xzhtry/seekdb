#!/usr/bin/env python3
"""Formal-suite simulation for MiniVectorDB.

Reuses the official driver transport (protocol_check/_transport.py) so the
timeouts, the handshake order and the assertion rules are identical to the
judge.  With --image the container runs with the judge resource limits
(--memory=2g --cpus=2 --read-only --pids-limit=128), which is the only way to
reproduce the real environment locally.

Scenarios cover the requirements the public sample pack only mentions in
prose: data volume beyond the memory budget, committed data surviving a forced
kill, a second kill during recovery, NaN/Infinity/dimension/overflow
rejection and ANN mode TopK validity.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))

from _transport import Session  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
BIN = str(ROOT / "build" / "minivectordb")

RESULTS = []


def record(name, ok, detail, seconds=None):
    RESULTS.append({"scenario": name, "ok": bool(ok), "detail": detail,
                    "seconds": None if seconds is None else round(seconds, 3)})
    flag = "PASS" if ok else "FAIL"
    print(f"[{flag}] {name} ({detail})" + (f" {seconds:.2f}s" if seconds else ""), flush=True)
    return ok


class Harness:
    """One data directory and one driver session, reused across restarts.

    The judge keeps a single Session alive for a case and restarts the
    container with the same data directory, so the session object is reused
    here as well.
    """

    def __init__(self, name, data_dir, image=None):
        self.name = name
        self.image = image
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.log = Path(f"/tmp/formal-sim-{name}.stderr.log")
        if self.log.exists():
            self.log.unlink()
        if image:
            self.command = None
            self.data_root = str(self.data_dir)
        else:
            self.command = [BIN, "--data-dir", str(self.data_dir), "--protocol", "jsonl"]
            self.data_root = None
        self.session = Session(CONFIG, str(self.log), image=image, command=self.command,
                               data_root=self.data_root)

    def start(self):
        t0 = time.monotonic()
        self.session.start()
        return time.monotonic() - t0

    def sql(self, text, mode="exact"):
        return self.session.request(text, mode)

    @property
    def data_path(self):
        """Directory the database actually writes to."""
        if self.session.bind_data is not None:
            return Path(self.session.bind_data.name)
        return self.data_dir

    def shutdown(self):
        self.session.shutdown()

    def crash(self):
        self.session.crash()

    def close(self):
        try:
            self.session.close()
        except Exception:  # noqa: BLE001
            pass


def expect_error(h, sql, label):
    r = h.sql(sql)
    code = r.get("error", {}).get("code") if r.get("ok") is False else None
    ok = r.get("ok") is False and isinstance(code, str) and bool(code)
    record(f"{label}", ok, f"sql={sql!r} -> {json.dumps(r, ensure_ascii=False)[:180]}")
    return ok


# ---------------------------------------------------------------------------
# Scenario 1: bulk load beyond the memory budget + restart.
# ---------------------------------------------------------------------------
def scenario_bulk(rows, batch, dims, workdir, image):
    h = Harness("bulk", Path(workdir) / "bulk", image)
    ready_s = h.start()
    t0 = time.monotonic()
    h.sql(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), embedding VECTOR({dims}))")
    for start in range(0, rows, batch):
        parts = []
        for i in range(batch):
            rid = start + i
            vec = ",".join(str((rid + j) % 97) for j in range(dims))
            parts.append(f"({rid},'x','[{vec}]')")
        r = h.sql("INSERT INTO docs VALUES " + ",".join(parts))
        if r.get("ok") is not True:
            record("bulk/insert", False, json.dumps(r, ensure_ascii=False)[:200])
            h.close()
            return False
    load_s = time.monotonic() - t0
    r = h.sql("SELECT id FROM docs ORDER BY id")
    seen = len(r.get("rows", []))
    h.shutdown()
    data_mb = sum(f.stat().st_size for f in h.data_path.glob("*")) / 1048576.0
    ready2 = h.start()
    r2 = h.sql("SELECT id FROM docs ORDER BY id")
    seen2 = len(r2.get("rows", []))
    h.shutdown()
    h.close()
    ok = seen == rows and seen2 == rows
    return record("bulk/restart", ok,
                  f"rows={seen} after_restart={seen2} data={data_mb:.1f}MB "
                  f"ready={ready_s:.2f}s ready_after_restart={ready2:.2f}s",
                  load_s) and record("bulk/startup_budget", ready2 < CONFIG["startup_timeout"],
                                     f"ready_after_restart={ready2:.2f}s limit={CONFIG['startup_timeout']}s")


# ---------------------------------------------------------------------------
# Scenario 2: repeated updates grow the data file; recovery must stay fast.
# ---------------------------------------------------------------------------
def scenario_update_growth(rows, rounds, workdir, image):
    h = Harness("growth", Path(workdir) / "growth", image)
    h.start()
    h.sql("CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(2))")
    vals = ",".join(f"({i},'[0,0]')" for i in range(rows))
    h.sql(f"INSERT INTO docs VALUES {vals}")
    t0 = time.monotonic()
    for _ in range(rounds):
        r = h.sql("UPDATE docs SET embedding = '[1,1]' WHERE id >= 0")
        if r.get("ok") is not True:
            record("growth/update", False, json.dumps(r, ensure_ascii=False)[:200])
            h.close()
            return False
    upd_s = time.monotonic() - t0
    h.shutdown()
    data_mb = sum(f.stat().st_size for f in h.data_path.glob("*")) / 1048576.0
    ready2 = h.start()
    r2 = h.sql("SELECT id, embedding FROM docs ORDER BY id")
    n = len(r2.get("rows", []))
    h.shutdown()
    h.close()
    return record("growth/recovery", n == rows and ready2 < CONFIG["startup_timeout"],
                  f"rows={n} data={data_mb:.1f}MB updates={upd_s:.2f}s ready_after_restart={ready2:.2f}s",
                  upd_s)


# ---------------------------------------------------------------------------
# Scenario 3: forced kill keeps committed data, drops uncommitted work.
# ---------------------------------------------------------------------------
def scenario_crash(workdir, image):
    h = Harness("crash", Path(workdir) / "crash", image)
    h.start()
    h.sql("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16))")
    h.sql("INSERT INTO docs VALUES (1,'kept'),(2,'kept2')")
    h.sql("BEGIN;")
    h.sql("INSERT INTO docs VALUES (3,'never')")
    h.crash()                      # SIGKILL, no shutdown
    ready2 = h.start()
    r = h.sql("SELECT id, content FROM docs ORDER BY id")
    rows = r.get("rows", [])
    h.shutdown()
    h.close()
    ok = rows == [[1, "kept"], [2, "kept2"]]
    return record("crash/commit_survives", ok,
                  f"rows={json.dumps(rows, ensure_ascii=False)} ready={ready2:.2f}s")


# ---------------------------------------------------------------------------
# Scenario 4: kill again *while* the store is recovering (local process only).
# ---------------------------------------------------------------------------
def scenario_crash_during_recovery(rows, workdir):
    h = Harness("crash2", Path(workdir) / "crash2")
    h.start()
    h.sql("CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(64))")
    for start in range(0, rows, 200):
        parts = []
        for i in range(200):
            rid = start + i
            vec = ",".join(str((rid + j) % 53) for j in range(64))
            parts.append(f"({rid},'[{vec}]')")
        h.sql("INSERT INTO docs VALUES " + ",".join(parts))
    h.crash()
    killer = subprocess.Popen([BIN, "--data-dir", str(h.data_dir), "--protocol", "jsonl"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(0.05)
    try:
        os.killpg(killer.pid, 9)
    except ProcessLookupError:
        pass
    killer.wait(timeout=10)
    ready2 = h.start()
    r = h.sql("SELECT id FROM docs ORDER BY id")
    n = len(r.get("rows", []))
    h.shutdown()
    h.close()
    return record("crash/during_recovery", n == rows and ready2 < CONFIG["startup_timeout"],
                  f"rows={n}/{rows} ready={ready2:.2f}s")


# ---------------------------------------------------------------------------
# Scenario 5: illegal vectors are rejected, never crash or hang.
# ---------------------------------------------------------------------------
def scenario_bad_vectors(workdir, image):
    h = Harness("badvec", Path(workdir) / "badvec", image)
    h.start()
    h.sql("CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(2))")
    ok = True
    for label, sql in [
        ("dimension", "INSERT INTO docs VALUES (1,'[0]')"),
        ("dimension_long", "INSERT INTO docs VALUES (1,'[0,0,0]')"),
        ("nan", "INSERT INTO docs VALUES (1,'[NaN,0]')"),
        ("inf", "INSERT INTO docs VALUES (1,'[Infinity,0]')"),
        ("overflow", "INSERT INTO docs VALUES (1,'[1e400,0]')"),
        ("query_nan", "SELECT id, L2_DISTANCE(embedding,'[NaN,0]') AS d FROM docs"),
        ("query_overflow", "SELECT id, L2_DISTANCE(embedding,'[1e400,0]') AS d FROM docs"),
        ("query_dim", "SELECT id, L2_DISTANCE(embedding,'[1,2,3]') AS d FROM docs"),
    ]:
        ok &= expect_error(h, sql, f"badvec/{label}")
    r = h.sql("SELECT id FROM docs")
    empty = r.get("rows") == []
    h.shutdown()
    h.close()
    return record("badvec/no_partial_row", empty, f"rows={json.dumps(r.get('rows'))}") and ok


# ---------------------------------------------------------------------------
# Scenario 6: ANN mode returns valid TopK (ids exist, no duplicates, filtered).
# ---------------------------------------------------------------------------
def scenario_ann(rows, dims, k, workdir, image):
    h = Harness("ann", Path(workdir) / "ann", image)
    h.start()
    h.sql(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), embedding VECTOR({dims}))")
    for start in range(0, rows, 200):
        parts = []
        for i in range(200):
            rid = start + i
            tag = "keep" if rid % 2 == 0 else "skip"
            vec = ",".join(str((rid * 7 + j * 13) % 101) for j in range(dims))
            parts.append(f"({rid},'{tag}','[{vec}]')")
        h.sql("INSERT INTO docs VALUES " + ",".join(parts))
    h.sql("CREATE VECTOR INDEX docs_vec ON docs(embedding)")
    query = "[" + ",".join("0" for _ in range(dims)) + "]"
    out = {}
    for mode in ("exact", "ann"):
        r = h.sql(f"SELECT id, L2_DISTANCE(embedding,'{query}') AS d FROM docs "
                  "WHERE tag = 'keep' ORDER BY d LIMIT 10", mode)
        rows = r.get("rows", [])
        ids = [row[0] for row in rows]
        out[mode] = (len(rows), len(set(ids)), r.get("ok"))
    r2 = h.sql("SELECT id FROM docs WHERE tag='keep' ORDER BY id")
    keep = len(r2.get("rows", []))
    h.shutdown()
    h.close()
    ok = out["ann"][0] == min(k, keep) and out["ann"][1] == out["ann"][0] \
        and out["exact"][0] == out["ann"][0]
    return record("ann/topk", ok, f"exact={out['exact']} ann={out['ann']} keep_rows={keep}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="/tmp/formal-sim")
    ap.add_argument("--image", default=None, help="run against a built image (judge limits)")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--dims", type=int, default=128)
    ap.add_argument("--growth-rows", type=int, default=5000)
    ap.add_argument("--growth-rounds", type=int, default=2)
    ap.add_argument("--ann-rows", type=int, default=2000)
    ap.add_argument("--ann-k", type=int, default=10)
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    work = Path(args.workdir)
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)

    wanted = {x for x in args.only.split(",") if x}
    todo = {
        "bulk": lambda: scenario_bulk(args.rows, args.batch, args.dims, work, args.image),
        "growth": lambda: scenario_update_growth(args.growth_rows, args.growth_rounds, work, args.image),
        "crash": lambda: scenario_crash(work, args.image),
        "crash2": lambda: scenario_crash_during_recovery(min(args.rows, 20000), work),
        "badvec": lambda: scenario_bad_vectors(work, args.image),
        "ann": lambda: scenario_ann(args.ann_rows, args.dims, args.ann_k, work, args.image),
    }
    for name, fn in todo.items():
        if wanted and name not in wanted:
            continue
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            record(name, False, f"exception: {type(exc).__name__}: {exc}")

    out = Path(args.workdir) / "formal-sim.json"
    out.write_text(json.dumps(RESULTS, ensure_ascii=False, indent=2) + "\n")
    failed = [r for r in RESULTS if not r["ok"]]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} scenarios ok -> {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
