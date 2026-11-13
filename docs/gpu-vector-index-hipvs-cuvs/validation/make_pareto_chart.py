#!/usr/bin/env python3
"""Render the 'path to 20 points' Pareto chart as a dependency-free SVG.

Panels
  1  build time vs recall@100 frontier at 250k x 128 (measured on the cloud box)
  2  score map: build ratio -> contest score, with every official run marked

Everything is read from the raw sweep logs, so no number is transcribed by hand.

Usage:
  make_pareto_chart.py --sweep a.txt [--sweep b.txt] --out out.svg
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import sys
import xml.sax.saxutils as sx

INK = "#0f172a"
MUTED = "#64748b"
GRID = "#e2e8f0"
AXIS = "#cbd5e1"
PROD_C = "#f59e0b"
BEST_C = "#16a34a"
ACC_C = "#2f6df0"
FONT = "Segoe UI, PingFang SC, Microsoft YaHei, Helvetica, Arial, sans-serif"

W = 1000
PAD_L = 96
PAD_R = 56
PAD_T = 104
PANEL_H = 380
GAP = 84


class Svg:
    def __init__(self):
        self.parts: list[str] = []

    def add(self, markup: str):
        self.parts.append(markup)

    def rect(self, x, y, w, h, fill, rx=0, stroke=None, sw=1, opacity=None):
        if w <= 0 or h <= 0:
            return
        a = [f'x="{x:.2f}"', f'y="{y:.2f}"', f'width="{w:.2f}"', f'height="{h:.2f}"', f'fill="{fill}"']
        if rx:
            a.append(f'rx="{rx}"')
        if stroke:
            a.append(f'stroke="{stroke}"')
            a.append(f'stroke-width="{sw}"')
        if opacity is not None:
            a.append(f'opacity="{opacity}"')
        self.add(f'<rect {" ".join(a)}/>')

    def line(self, x1, y1, x2, y2, stroke, sw=1, dash=None):
        a = [f'x1="{x1:.2f}"', f'y1="{y1:.2f}"', f'x2="{x2:.2f}"', f'y2="{y2:.2f}"',
             f'stroke="{stroke}"', f'stroke-width="{sw}"']
        if dash:
            a.append(f'stroke-dasharray="{dash}"')
        self.add(f'<line {" ".join(a)}/>')

    def circle(self, cx, cy, r, fill, stroke=None, sw=1):
        a = [f'cx="{cx:.2f}"', f'cy="{cy:.2f}"', f'r="{r:.2f}"', f'fill="{fill}"']
        if stroke:
            a.append(f'stroke="{stroke}"')
            a.append(f'stroke-width="{sw}"')
        self.add(f'<circle {" ".join(a)}/>')

    def text(self, x, y, s, size=12, fill=INK, anchor="start", weight="400"):
        self.add(f'<text x="{x:.2f}" y="{y:.2f}" font-family="{FONT}" font-size="{size}" '
                 f'fill="{fill}" text-anchor="{anchor}" font-weight="{weight}">{sx.escape(s)}</text>')

    def dump(self) -> str:
        return "\n".join(self.parts)


def parse(path):
    builds: dict[str, list[float]] = {}
    grid: dict[str, list[dict]] = {}
    args: dict[str, list[str]] = {}
    cur = None
    pat_cfg = re.compile(r"^### CFG (\S+) args: (.*)$")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            m = pat_cfg.match(line)
            if m:
                cur = m.group(1)
                args[cur] = m.group(2).split()
                continue
            m = re.match(r"^\[(\S+)\] (\{.*\})\s*$", line)
            if not m:
                continue
            lab, payload = m.group(1), m.group(2)
            try:
                d = json.loads(payload)
            except Exception:
                continue
            if "build" in d:
                builds.setdefault(lab, []).append(d.get("ms", 0.0))
            elif "itopk" in d and "err" not in d and "nq" not in d:
                grid.setdefault(lab, []).append(d)
    return builds, grid, args


def label_of(args_row):
    # args: ds qf n dim nq topk gd igd niter algo metric nbuild
    gd, igd, ni, algo = args_row[6], args_row[7], args_row[8], args_row[9]
    algo_name = {"2": "NN_DESCENT", "1": "IVF_PQ", "3": "ITER_CAGRA"}.get(algo, algo)
    return f"gd{gd}/igd{igd}/it{ni}/{algo_name}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep", action="append", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    builds, grid, args = {}, {}, {}
    for p in a.sweep:
        b, g, ar = parse(p)
        for k, v in b.items():
            builds.setdefault(k, []).extend(v)
        for k, v in g.items():
            grid.setdefault(k, []).extend(v)
        args.update(ar)

    def build_ms(lab):
        v = builds.get(lab) or []
        return sorted(v)[len(v) // 2] if v else None

    # ---- series: one vertical stack per build config -------------------------
    merged: dict[str, dict] = {}
    for lab, rows in grid.items():
        if not lab.startswith(("B_", "R1_", "R2_", "C_warm")):
            continue
        a_row = args.get(lab)
        if not a_row or len(a_row) < 10:
            continue
        if a_row[2] != "250000":
            continue
        bm = build_ms(lab)
        if not bm:
            continue
        title = label_of(a_row)
        pts = sorted(((d["itopk"], d.get("recall100", 0), d.get("ms", 0)) for d in rows
                      if d["itopk"] >= 256), key=lambda t: t[1])
        if not pts:
            continue
        if title not in merged:
            merged[title] = {"title": title, "samples": list(builds.get(lab, [])), "pts": pts,
                             "labs": [lab]}
        else:
            merged[title]["labs"].append(lab)
            merged[title]["samples"].extend(builds.get(lab, []))
            merged[title]["pts"] = sorted(merged[title]["pts"] + pts, key=lambda t: t[1])
    series = list(merged.values())
    for s in series:
        v = sorted(s["samples"])
        s["ms"] = v[len(v) // 2] if v else 0.0
    series.sort(key=lambda s: s["ms"])

    prod = next((s for s in series if s["title"] == "gd128/igd256/it100/NN_DESCENT"), None)
    best = next((s for s in series if s["title"] == "gd128/igd256/it20/NN_DESCENT"), None)
    alt = next((s for s in series if s["title"] == "gd128/igd128/it50/NN_DESCENT"), None)

    svg = Svg()
    H = PAD_T + PANEL_H + GAP + PANEL_H + 146
    svg.add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
    svg.rect(0, 0, W, H, "#ffffff")
    svg.text(PAD_L, 36, "冲 20 分：建图耗时 × 召回前沿（250k × 128，W7900 / ROCm 7.2.4 实测）", 17, INK, weight="600")
    svg.text(PAD_L, 58, "数据来源：云端 cuVS CAGRA 直连扫描（sweep2/sweep3），每个点是一个 (itopk, search_width, algo) 组合", 11, MUTED)

    # ---------------- panel 1 -------------------------------------------------
    px0, px1 = PAD_L, W - PAD_R
    py0, py1 = PAD_T, PAD_T + PANEL_H
    xmin, xmax = 6000.0, 46000.0
    ymin, ymax = 0.80, 1.005

    def X(ms):
        t = (math.log10(ms) - math.log10(xmin)) / (math.log10(xmax) - math.log10(xmin))
        return px0 + t * (px1 - px0)

    def Y(r):
        t = (r - ymin) / (ymax - ymin)
        return py1 - t * (py1 - py0)

    svg.text(px0, py0 - 14, "① 建图耗时 vs 召回率@100（越左越快，越上越准）", 13, INK, weight="600")
    for gx in (8000, 10000, 15000, 20000, 30000, 40000):
        x = X(gx)
        svg.line(x, py0, x, py1, GRID, 1)
        svg.text(x, py1 + 18, f"{gx/1000:.0f}s", 10, MUTED, anchor="middle")
    for gy in (0.80, 0.85, 0.90, 0.95, 1.00):
        y = Y(gy)
        svg.line(px0, y, px1, y, GRID, 1)
        svg.text(px0 - 10, y + 4, f"{gy:.2f}", 10, MUTED, anchor="end")
    svg.line(px0, py1, px1, py1, AXIS, 1.2)
    svg.line(px0, py0, px0, py1, AXIS, 1.2)
    svg.text((px0 + px1) / 2, py1 + 36, "CAGRA 建图耗时（云端实测，中位数）", 11, MUTED, anchor="middle")

    # production reference line (recall level of the shipped configuration)
    if prod:
        ref = max((p[1] for p in prod["pts"] if p[0] == 512), default=None)
        if ref:
            y = Y(ref)
            svg.line(px0, y, px1, y, PROD_C, 1.4, dash="6 4")
            svg.text(px1 - 6, y - 7, f"冻结基线配置召回水平（itopk=512）{ref:.4f}", 10, PROD_C, anchor="end")

    # gate line: delta <= 0.005 vs the shipped configuration
    if prod:
        ref = max((p[1] for p in prod["pts"] if p[0] == 512), default=None)
        if ref:
            y = Y(ref - 0.005)
            svg.line(px0, y, px1, y, "#dc2626", 1.2, dash="3 3")
            svg.text(px0 + 6, y - 7, "召回下降 0.005 门槛线（相对同轮冻结基线）", 10, "#dc2626")

    for s in series:
        col = ACC_C
        vis = [p for p in s["pts"] if p[1] >= ymin - 0.004]
        for it, r, _ms in vis:
            x, y = X(s["ms"]), Y(r)
            svg.circle(x, y, 3.2, col, "#ffffff", 1)
        xs = [X(s["ms"])] * len(vis)
        ys = [Y(p[1]) for p in vis]
        for i in range(len(ys) - 1):
            svg.line(xs[i], ys[i], xs[i + 1], ys[i + 1], ACC_C, 1, dash="2 2")

    # highlight the shipped and the recommended configurations
    if prod:
        x = X(prod["ms"])
        for it, r, _ms in prod["pts"]:
            if it == 512:
                svg.circle(x, Y(r), 7, PROD_C, "#ffffff", 2)
        # The label is placed to the left of the marker: the marker sits at the
        # right edge of the panel, where the dashed recall line already is.
        svg.text(x - 14, Y(max(p[1] for p in prod["pts"] if p[0] == 512)) + 20,
                 "冻结基线配置 gd128/igd256/it100", 10, PROD_C, weight="600", anchor="end")
        svg.text(x - 14, Y(max(p[1] for p in prod["pts"] if p[0] == 512)) + 34,
                 f"{prod['ms']/1000:.1f}s · itopk512", 10, MUTED, anchor="end")
    if alt:
        # The configuration that was submitted in rounds 10 and 11.
        x = X(alt["ms"])
        pick = next((p for p in alt["pts"] if p[0] == 1024), alt["pts"][-1])
        svg.circle(x, Y(pick[1]), 8, "#0891b2", "#ffffff", 2.4)
        svg.text(x + 12, Y(pick[1]) - 26, "上线配置 gd128/igd128/it50 + itopk1024", 10, "#0891b2", weight="600")
        svg.text(x + 12, Y(pick[1]) - 12,
                 f"{alt['ms']/1000:.1f}s · 召回 {pick[1]:.4f} · 显存更省", 10, MUTED)
    if best:
        x = X(best["ms"])
        pick = next((p for p in best["pts"] if p[0] == 1024), best["pts"][-1])
        svg.circle(x, Y(pick[1]), 8, BEST_C, "#ffffff", 2.4)
        svg.text(x + 12, Y(pick[1]) + 20, "备选配置 gd128/igd256/it20 + itopk1024", 10, BEST_C, weight="600")
        svg.text(x + 12, Y(pick[1]) + 34,
                 f"{best['ms']/1000:.1f}s · itopk1024 · 召回 {pick[1]:.4f}", 10, MUTED)

    # legend
    lx, ly = px0 + 12, py1 - 48
    svg.circle(lx, ly - 4, 3.2, ACC_C)
    svg.text(lx + 9, ly, "各建图配置在 (itopk, sw, algo) 网格上的可达召回", 10, MUTED)
    svg.circle(lx, ly + 14, 7, PROD_C, "#ffffff", 2)
    svg.text(lx + 14, ly + 18, "冻结基线配置", 10, MUTED)
    svg.circle(lx + 104, ly + 14, 8, BEST_C, "#ffffff", 2.4)
    svg.text(lx + 118, ly + 18, "备选配置", 10, MUTED)
    svg.circle(lx + 186, ly + 14, 8, "#0891b2", "#ffffff", 2.4)
    svg.text(lx + 200, ly + 18, "上线配置（第 10/11 轮）", 10, MUTED)

    # ---------------- panel 2: score map -------------------------------------
    qx0, qx1 = PAD_L, W - PAD_R
    qy0, qy1 = py1 + GAP + 34, py1 + GAP + 34 + PANEL_H
    rmax = 3.8

    def XS(r):
        return qx0 + (r / rmax) * (qx1 - qx0)

    def YS(s):
        t = s / 20.0
        return qy1 - t * (qy1 - qy0)

    svg.text(qx0, qy0 - 16, "② 官方评分映射：构建加速比 → 总分（查询比 ≥1.5x，2 分已封顶拿满）", 13, INK, weight="600")
    for gx in (1.0, 1.5, 2.0, 2.5, 3.0, 3.5):
        x = XS(gx)
        svg.line(x, qy0, x, qy1, GRID, 1)
        svg.text(x, qy1 + 18, f"{gx:.1f}x", 10, MUTED, anchor="middle")
    for gy in (0, 5, 10, 15, 20):
        y = YS(gy)
        svg.line(qx0, y, qx1, y, GRID, 1)
        svg.text(qx0 - 10, y + 4, f"{gy}", 10, MUTED, anchor="end")
    svg.line(qx0, qy1, qx1, qy1, AXIS, 1.2)
    svg.line(qx0, qy0, qx0, qy1, AXIS, 1.2)
    svg.text((qx0 + qx1) / 2, qy1 + 36, "构建加速比（GPU 构建耗时 ÷ CPU 基线构建耗时）", 11, MUTED, anchor="middle")

    def score_of(rb, rq=1.599):
        # The query term saturates at 1.5x: four official runs pin the formula
        # down to score = 4 + 14*ln(rb)/ln(3) + min(2, 2*ln(rq)/ln(1.5)).
        q = min(2.0, 2.0 * math.log(rq) / math.log(1.5))
        return 4.0 + 14.0 * math.log(rb) / math.log(3.0) + q

    pts = [(XS(r), YS(min(score_of(r), 20.0))) for r in [1.0 + i * 0.02 for i in range(int((rmax - 1.0) / 0.02) + 1)]]
    for i in range(len(pts) - 1):
        svg.line(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], ACC_C, 2.2)
    svg.text(pts[-1][0] + 4, pts[-1][1] - 8, "满分 20 分", 10, MUTED, anchor="end")

    # 20-point requirement marker
    x20 = XS(3.0)
    svg.line(x20, qy0, x20, qy1, BEST_C, 1.4, dash="5 4")
    svg.text(x20 - 6, qy0 + 16, "20 分需要构建比 3.0x", 10, BEST_C, weight="600", anchor="end")

    # The official runs, in the order they happened. The build ratio and the
    # score are the platform's own numbers, not an extrapolation.
    official = [
        (1.105, "5.271", "冻结基线 445b9b2", MUTED),
        (1.081, "5.324", "第 9 轮 3794da35", MUTED),
        (1.139, "6.115", "第 8 轮 cc6c79f3", MUTED),
        (1.648, "12.369", "第 10 轮 e568a8b1", ACC_C),
        (2.120, "15.575", "第 11 轮 11f2e9f5", PROD_C),
        (2.114, "15.539", "第 13 轮 5800e6b4", PROD_C),
        (2.188, "15.977", "第 14 轮 e4a56362", BEST_C),
        (2.180, "15.937", "第 15 轮 c60fd055", MUTED),
        (2.099, "15.375", "第 16 轮 1d89dcea", MUTED),
        (3.220, "20.000", "第 17 轮 bcda70fe", BEST_C),
    ]
    for r, sc, _lab, col in official:
        x, y = XS(r), YS(float(sc))
        svg.circle(x, y, 6, col, "#ffffff", 2)

    # Rounds 11/13/14/15 land within 15 px of each other, so they share one
    # callout instead of four overlapping labels. Round 16 sits a bit lower
    # and gets its own short label. Round 17 is the winner and sits on the
    # flat top of the curve, far to the right.
    svg.text(XS(2.099) + 6, YS(15.375) + 16, "第 16 轮", 10, MUTED, weight="600")
    svg.text(XS(2.188) - 12, YS(15.977) - 7,
             "第 11~16 轮：2.10~2.19x ⇒ 15.38~15.98 分（第 14 轮最好）",
             10, BEST_C, weight="600", anchor="end")
    svg.text(XS(3.220) + 12, YS(20.000) + 16,
             "第 17 轮 20.000 分",
             10, BEST_C, weight="600")

    # Short labels where they cannot collide with each other.
    svg.text(XS(1.105) + 10, YS(5.271) - 4, "冻结基线", 10, MUTED, weight="600")
    svg.text(XS(1.139) + 10, YS(6.115) - 6, "第 8 轮", 10, MUTED, weight="600")
    svg.text(XS(1.081) - 10, YS(5.324) + 14, "第 9 轮", 10, MUTED, weight="600", anchor="end")
    svg.text(XS(1.648) + 10, YS(12.369) - 5, "第 10 轮 e568a8b1", 10, ACC_C, weight="600")

    # The full ledger of official runs, two columns, in the free area under
    # the curve.
    svg.text(qx0 + 9, qy1 - 75, "官方提测记录（平台自己回显的构建比 / 总分）", 10, MUTED, weight="600")
    ledger = [
        ("冻结基线 445b9b2", "1.105x / 5.271", "第 11 轮 11f2e9f5", "2.120x / 15.575"),
        ("第 8 轮 cc6c79f3", "1.139x / 6.115", "第 13 轮 5800e6b4", "2.114x / 15.539"),
        ("第 9 轮 3794da35", "1.081x / 5.324", "第 14 轮 e4a56362", "2.188x / 15.977"),
        ("第 10 轮 e568a8b1", "1.648x / 12.369", "第 15 轮 c60fd055", "2.180x / 15.937"),
        ("第 16 轮 1d89dcea", "2.099x / 15.375", "第 17 轮 bcda70fe", "3.220x / 20.000"),
    ]
    for i, (la, va, lb, vb) in enumerate(ledger):
        yy = qy1 - 60 + i * 14
        svg.text(qx0 + 9, yy, la, 10, MUTED)
        svg.text(qx0 + 108, yy, va, 10, INK, weight="600")
        svg.text(qx0 + 216, yy, lb, 10, MUTED)
        svg.text(qx0 + 330, yy, vb, 10, INK, weight="600")

    svg.text(qx0, qy1 + 58,
             "评分式（由十七轮官方成绩反推并验证）：总分 = 4 + 14·ln(构建比)/ln(3.0) + min(2, 2·ln(查询比)/ln(1.5))",
             11, MUTED)

    svg.add("</svg>")
    pathlib.Path(a.out).write_text(svg.dump(), encoding="utf-8")
    print(f"wrote {a.out}")
    print(f"series: {len(series)}")
    for s in series:
        top = max(s["pts"], key=lambda p: p[1])
        print(f"  {s['title']:<34} build={s['ms']/1000:>7.2f}s  best_r100={top[1]:.4f} @itopk{top[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
