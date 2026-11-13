#!/usr/bin/env python3
import json
import re
from collections import defaultdict

paths = ["/mnt/d/oceanbase大赛/work/sweep2_out.txt",
         "/mnt/d/oceanbase大赛/work/sweep3_out.txt"]
builds = defaultdict(list)
grid = defaultdict(list)
args = {}
for p in paths:
    for raw in open(p, encoding="utf-8", errors="replace"):
        line = raw.rstrip("\n")
        m = re.match(r"^### CFG (\S+) args: (.*)$", line)
        if m:
            args[m.group(1)] = m.group(2).split()
            continue
        m = re.match(r"^\[(\S+)\] (\{.*\})\s*$", line)
        if not m:
            continue
        lab, pl = m.group(1), m.group(2)
        try:
            d = json.loads(pl)
        except Exception:
            continue
        if "build" in d:
            builds[lab].append(d.get("ms", 0))
        elif "itopk" in d and "err" not in d and "nq" not in d:
            grid[lab].append(d)

print(f"{'config':<22}{'gd/igd/it/algo':<26}{'builds(ms)':<32}{'med':>8}  best_r100@it   r100@1024  r10@1024")
print("-" * 132)
for lab in sorted(builds, key=lambda l: (builds[l][0] if builds[l] else 1e9)):
    a = args.get(lab, [])
    if len(a) < 10 or a[2] != "250000":
        continue
    v = sorted(builds[lab])
    med = v[len(v) // 2]
    nm = {"2": "NN", "1": "IVFPQ", "3": "ITER"}.get(a[9], a[9])
    tag = f"gd{a[6]}/igd{a[7]}/it{a[8]}/{nm}"
    pts = grid.get(lab, [])
    best = max(pts, key=lambda d: d.get("recall100", 0)) if pts else None
    p1024 = [d for d in pts if d["itopk"] == 1024]
    p512 = [d for d in pts if d["itopk"] == 512]
    b1024 = max((d.get("recall100", 0) for d in p1024), default=float("nan"))
    b512 = max((d.get("recall100", 0) for d in p512), default=float("nan"))
    s1024 = max((d.get("recall10", 0) for d in p1024), default=float("nan"))
    s512 = max((d.get("recall10", 0) for d in p512), default=float("nan"))
    bs = f"{best['itopk']}={best.get('recall100', 0):.4f}" if best else "-"
    print(f"{lab:<22}{tag:<26}{str([round(x) for x in builds[lab]]):<32}{med:>8.0f}  {bs:<14}"
          f"{b1024:.4f}     {s1024:.4f}   (it512: r10={s512:.4f} r100={b512:.4f})")

print()
print("ITER / IVF_PQ configs detail:")
for lab in builds:
    if "iter" in lab.lower() or "ivfpq" in lab.lower():
        pts = grid.get(lab, [])
        top = max(pts, key=lambda d: d.get("recall100", 0)) if pts else None
        print(f"  {lab:<22} builds={[round(x) for x in builds[lab]]}  "
              f"best r100={top.get('recall100', 0):.4f} @itopk{top['itopk']} sw{top['sw']} algo{top['algo']}"
              if top else f"  {lab:<22} builds={[round(x) for x in builds[lab]]}  (no successful search)")

print()
print("single-query latency (nq=1, search_width=4):")
for p in paths:
    for raw in open(p, encoding="utf-8", errors="replace"):
        m = re.match(r"^\[(\S+)\] (\{.*\})\s*$", raw.rstrip("\n"))
        if not m:
            continue
        try:
            d = json.loads(m.group(2))
        except Exception:
            continue
        if d.get("nq") == 1 and d.get("err") == 1:
            print(f"  {m.group(1):<22} itopk={d['itopk']:<6} algo={d['algo']} ms={d['ms']:.3f}")
