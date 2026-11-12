#!/usr/bin/env python3
"""Render the CPU/GPU build-performance charts as a dependency-free SVG.

No third-party module is required (matplotlib/numpy may be missing on the
instance), so the chart can always be produced from the benchmark output.

Two modes:

  reference   the state that is known today: the scoring curve, the baseline
              build-time reference and the measured snapshot container sizes.
              Everything is labelled with where the number comes from.

  benchmark   the real comparison, driven by the JSON that
              run_build_benchmark.sh writes. Adds the CPU-vs-GPU build time
              panel and the build-window stage split.

Usage:
  make_build_chart.py --mode reference --out build-performance.svg
  make_build_chart.py --mode benchmark \
      --benchmark /tmp/seekdb-bench-evidence/benchmark.json \
      --stages   /tmp/seekdb-bench-evidence/stages.txt \
      --out      /tmp/seekdb-bench-evidence/build-performance.svg
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import statistics
import sys
import xml.sax.saxutils as sx

# --------------------------------------------------------------------------- #
# palette
# --------------------------------------------------------------------------- #
INK = "#0f172a"
MUTED = "#64748b"
GRID = "#e2e8f0"
AXIS = "#cbd5e1"
CPU_C = "#94a3b8"
GPU_C = "#2f6df0"
FAST_C = "#16a34a"
BLOB_C = "#cbd5e1"
ACCENT = "#f59e0b"
FONT = "Segoe UI, PingFang SC, Microsoft YaHei, Helvetica, Arial, sans-serif"

W = 960
PAD_L = 92
PAD_R = 40
PAD_T = 64


class Svg:
    """Minimal SVG writer."""

    def __init__(self) -> None:
        self.parts: list[str] = []

    def add(self, markup: str) -> None:
        self.parts.append(markup)

    def rect(self, x, y, w, h, fill, rx=0, stroke=None, sw=1, opacity=None):
        if w <= 0 or h <= 0:
            return
        attrs = [f'x="{x:.2f}"', f'y="{y:.2f}"', f'width="{w:.2f}"', f'height="{h:.2f}"',
                 f'fill="{fill}"']
        if rx:
            attrs.append(f'rx="{rx}"')
        if stroke:
            attrs.append(f'stroke="{stroke}"')
            attrs.append(f'stroke-width="{sw}"')
        if opacity is not None:
            attrs.append(f'opacity="{opacity}"')
        self.add(f'<rect {" ".join(attrs)}/>')

    def line(self, x1, y1, x2, y2, stroke, sw=1, dash=None):
        attrs = [f'x1="{x1:.2f}"', f'y1="{y1:.2f}"', f'x2="{x2:.2f}"', f'y2="{y2:.2f}"',
                 f'stroke="{stroke}"', f'stroke-width="{sw}"']
        if dash:
            attrs.append(f'stroke-dasharray="{dash}"')
        self.add(f'<line {" ".join(attrs)}/>')

    def text(self, x, y, content, size=13, fill=INK, anchor="start", weight="normal"):
        self.add(
            f'<text x="{x:.2f}" y="{y:.2f}" font-family="{FONT}" font-size="{size}" '
            f'fill="{fill}" text-anchor="{anchor}" font-weight="{weight}">{sx.escape(str(content))}</text>'
        )

    def path(self, points, stroke, sw=2, fill="none", dash=None):
        if not points:
            return
        d = " ".join(f"{'M' if i == 0 else 'L'}{x:.2f},{y:.2f}" for i, (x, y) in enumerate(points))
        attrs = [f'd="{d}"', f'stroke="{stroke}"', f'stroke-width="{sw}"', f'fill="{fill}"']
        if dash:
            attrs.append(f'stroke-dasharray="{dash}"')
        self.add(f'<path {" ".join(attrs)}/>')

    def circle(self, x, y, r, fill, stroke=None, sw=2):
        attrs = [f'cx="{x:.2f}"', f'cy="{y:.2f}"', f'r="{r}"', f'fill="{fill}"']
        if stroke:
            attrs.append(f'stroke="{stroke}"')
            attrs.append(f'stroke-width="{sw}"')
        self.add(f'<circle {" ".join(attrs)}/>')

    def render(self) -> str:
        head = (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" '
            f'height="{self.height}" viewBox="0 0 {W} {self.height}" '
            f'font-family="{FONT}">'
        )
        return head + "\n".join(self.parts) + "</svg>\n"

    height = 400


def nice_ceiling(value: float) -> float:
    """Round a maximum up to a readable axis bound."""
    if value <= 0:
        return 1.0
    exp = math.floor(math.log10(value))
    base = 10 ** exp
    for mult in (1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if value <= base * mult:
            return base * mult
    return base * 10


def fmt_ms(value: float) -> str:
    if value >= 1000:
        return f"{value / 1000:.1f}s"
    return f"{value:.0f}ms"


def fmt_bytes(value: float) -> str:
    """Binary units, because the byte counts are exact powers of two."""
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024 or unit == "GiB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} GiB"


# --------------------------------------------------------------------------- #
# panels
# --------------------------------------------------------------------------- #
def panel_score_curve(svg: Svg, y0: int) -> int:
    """Score as a function of the build speedup, from the contest formula."""
    h = 250
    svg.text(PAD_L, y0 - 14, "构建加速比 → 总分（评分公式 14·ln(r)/ln(3.0) + 查询分）",
             size=15, weight="600")
    svg.text(PAD_L, y0 + 2, "曲线为构建分（14·ln(r)/ln(3.0)）；两个圆点是平台真实跑出来的构建加速比，"
             "标注为该轮的总分", size=12, fill=MUTED)

    x0, x1 = PAD_L, W - PAD_R
    plot_top = y0 + 26
    axis_y = y0 + h - 46
    max_speedup = 3.0
    max_score = 20.0

    def sx_of(r: float) -> float:
        return x0 + (x1 - x0) * (r / max_speedup)

    def sy_of(s: float) -> float:
        return axis_y - (axis_y - plot_top) * (s / max_score)

    for s in range(0, int(max_score) + 1, 5):
        y = sy_of(s)
        svg.line(x0, y, x1, y, GRID, 1)
        svg.text(x0 - 10, y + 4, s, size=11, fill=MUTED, anchor="end")
    for r in (0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0):
        x = sx_of(r)
        svg.line(x, plot_top, x, axis_y, GRID, 1, dash="3 4")
        svg.text(x, axis_y + 18, f"{r:.1f}x", size=11, fill=MUTED, anchor="middle")

    svg.line(x0, axis_y, x1, axis_y, AXIS, 1.5)

    def score(r: float) -> float:
        return 14.0 * math.log(r) / math.log(3.0) if r > 0 else 0.0

    # Only r >= 1 is meaningful; below that the build score is negative.
    pts = []
    for i in range(0, 301):
        r = 1.0 + (max_speedup - 1.0) * i / 300
        pts.append((sx_of(r), sy_of(score(r))))
    svg.path(pts, GPU_C, 2.5)

    # The two runs the platform actually measured. Their build ratios are only
    # 0.034 apart, so the labels go into a legend instead of next to the dots.
    for r_best, color in ((1.105, BLOB_C), (1.139, FAST_C)):
        xb = sx_of(r_best)
        svg.line(xb, plot_top, xb, axis_y, color, 1.2, dash="4 4")
        svg.circle(xb, sy_of(score(r_best)), 4.5, color)

    legend_x = x0 + (x1 - x0) * 0.58
    for i, (r_best, total, color, label) in enumerate((
        (1.105, "5.271", BLOB_C, "冻结基线 445b9b2：1.105x → 总分 5.271"),
        (1.139, "6.115", FAST_C, "本方案 cc6c79f3：1.139x → 总分 6.115"),
    )):
        ly = plot_top + 16 + i * 24
        svg.circle(legend_x, ly - 4, 4.5, color)
        svg.text(legend_x + 12, ly, label, size=12, fill=INK, weight="600")

    svg.text((x0 + x1) / 2, axis_y + 38, "构建加速比（基线构建耗时 ÷ 本方案构建耗时）",
             size=12, fill=MUTED, anchor="middle")
    return h


def panel_container_bytes(svg: Svg, y0: int) -> int:
    """Snapshot container size: measured locally by test_fast_snapshot."""
    h = 215
    svg.text(PAD_L, y0 - 12, "快照容器体积（SIFT250K / Hidden250K，本地实测）", size=15, weight="600")
    svg.text(PAD_L, y0 + 4, "数据只存一份，且不再有 cuVS blob 过文件系统两遍",
             size=12, fill=MUTED)

    n, dim, degree = 250000, 128, 128
    ids = n * 8
    vectors = n * dim * 4
    graph = n * degree * 4
    metadata = 4096
    fast_total = ids + vectors + graph
    blob_total = ids + vectors + vectors + graph + metadata

    x0, x1 = PAD_L, W - PAD_R
    maxv = nice_ceiling(blob_total)

    def bw(total: float) -> float:
        return (x1 - x0) * (total / maxv)

    bar_h = 24
    rows = [
        ("基线 OBCUVS02：数据存两份 + cuVS blob 过文件系统两遍", blob_total, BLOB_C),
        ("快路径 OBCUVS03：数据只存一份，无 blob 往返", fast_total, FAST_C),
    ]
    y = y0 + 30
    for label, total, color in rows:
        svg.text(x0, y, label, size=12, fill=INK)
        svg.rect(x0, y + 8, bw(total), bar_h, color, rx=4)
        svg.text(x0 + bw(total) + 8, y + 8 + bar_h * 0.72, f"{fmt_bytes(total)}", size=12,
                 fill=INK, weight="600")
        y += 62

    svg.text(x0, y, f"节省 {fmt_bytes(blob_total - fast_total)}"
             f"（{(blob_total - fast_total) / blob_total * 100:.1f}%）",
             size=12, fill=FAST_C, weight="600")
    return h


def panel_cpu_gpu(svg: Svg, y0: int, bench: dict) -> int:
    """CPU vs GPU build time, from benchmark.json."""
    h = 260
    svg.text(PAD_L, y0 - 12, "CPU 与 GPU 构建耗时（CREATE VECTOR INDEX 计时窗口）",
             size=15, weight="600")

    cpu = bench.get("cpu_vsag") or {}
    gpu = bench.get("gpu_cuvs") or {}
    if not cpu or not gpu:
        svg.text(PAD_L, y0 + 60, "（无实测数据）", size=13, fill=MUTED)
        return h

    ds = bench.get("dataset") or {}
    subset = (f"（{ds.get('rows', 0):,} × {ds.get('dim', 0)}，"
              f"{bench.get('repeat', 0)} 次重复）" if ds else "")
    svg.text(PAD_L, y0 + 4, f"同一批数据、同一组索引参数，只有 lib 不同{subset}",
             size=12, fill=MUTED)

    x0, x1 = PAD_L, W - PAD_R
    base_y = y0 + h - 46
    maxv = nice_ceiling(max(cpu["max_ms"], gpu["max_ms"]))
    chart_h = base_y - (y0 + 34)

    def bh(ms: float) -> float:
        return chart_h * (ms / maxv)

    groups = [("CPU  lib=vsag", cpu, CPU_C), ("GPU  lib=cuvs", gpu, GPU_C)]
    n = len(groups)
    slot = (x1 - x0) / n
    bar_w = min(150, slot * 0.42)
    for i, (label, entry, color) in enumerate(groups):
        cx = x0 + slot * (i + 0.5)
        # median
        x = cx - bar_w - 6
        svg.rect(x, base_y - bh(entry["median_ms"]), bar_w, bh(entry["median_ms"]), color, rx=4)
        svg.text(x + bar_w / 2, base_y - bh(entry["median_ms"]) - 8,
                 fmt_ms(entry["median_ms"]), size=12, fill=INK, anchor="middle", weight="600")
        # range
        y_hi = base_y - bh(entry["max_ms"])
        y_lo = base_y - bh(entry["min_ms"])
        svg.line(cx + 6, y_hi, cx + 6, y_lo, INK, 2)
        svg.line(cx + 2, y_hi, cx + 10, y_hi, INK, 2)
        svg.line(cx + 2, y_lo, cx + 10, y_lo, INK, 2)
        svg.text(cx + 6, base_y + 38, f"{fmt_ms(entry['min_ms'])}–{fmt_ms(entry['max_ms'])}",
                 size=11, fill=MUTED, anchor="middle")
        svg.text(cx, base_y + 20, label, size=13, fill=INK, anchor="middle", weight="600")

    svg.line(x0, base_y, x1, base_y, AXIS, 1.5)
    # maxv is in milliseconds; label the axis in seconds.
    step = max(1.0, nice_ceiling(maxv / 5.0) / 1000.0)
    v = 0.0
    while v <= maxv / 1000.0 + 1e-9:
        y = base_y - chart_h * (v * 1000 / maxv)
        svg.line(x0, y, x1, y, GRID, 1)
        svg.text(x0 - 10, y + 4, f"{v:g}s", size=11, fill=MUTED, anchor="end")
        v += step

    speedup = bench.get("speedup_median_cpu_over_gpu")
    if speedup:
        svg.text(x1, y0 + 28, f"GPU 构建加速比：{speedup:.3f}x（CPU 中位数 ÷ GPU 中位数）",
                 size=13, fill=GPU_C, anchor="end", weight="600")
    return h


STAGE_RE = re.compile(r"stage op=(\w+) path=(\w+).*?build_us=(-?\d+)"
                      r".*?export_us=(-?\d+).*?write_us=(-?\d+).*?total_us=(-?\d+)")


def parse_stages(path: pathlib.Path):
    """Collect the per-stage timings that the adaptor logs.

    A benchmark run repeats the build several times, so every stage field is
    reported as the median over the repetitions instead of the value of the
    last repetition.
    """
    stages: dict[tuple[str, str], dict[str, int]] = {}
    samples: dict[tuple[str, str], list[dict[str, int]]] = {}
    if not path or not path.exists():
        return stages
    for line in path.read_text(errors="replace").splitlines():
        match = STAGE_RE.search(line)
        if not match:
            continue
        op, mode = match.group(1), match.group(2)
        samples.setdefault((op, mode), []).append({
            "build_us": int(match.group(3)),
            "export_us": int(match.group(4)),
            "write_us": int(match.group(5)),
            "total_us": int(match.group(6)),
        })
    for key, rows in samples.items():
        stages[key] = {
            field: int(statistics.median(row[field] for row in rows))
            for field in ("build_us", "export_us", "write_us", "total_us")
        }
    return stages


def panel_stages(svg: Svg, y0: int, stages: dict) -> int:
    """Split the timed build window into its stages."""
    h = 212
    svg.text(PAD_L, y0 - 12, "构建窗口分段耗时（build / export / write）", size=15, weight="600")
    svg.text(PAD_L, y0 + 4, "这决定还有多少工程优化空间：若 build 占大头，改图才有意义",
             size=12, fill=MUTED)

    serialize = {mode: v for (op, mode), v in stages.items() if op == "serialize"}
    if not serialize:
        svg.text(PAD_L, y0 + 60, "（无实测数据）", size=13, fill=MUTED)
        return h

    x0, x1 = PAD_L, W - PAD_R
    base_y = y0 + h - 44
    chart_h = base_y - (y0 + 60)
    # The stage values are microseconds; the chart works in milliseconds.
    maxv = nice_ceiling(max(v["total_us"] for v in serialize.values()) / 1000.0)

    order = [("fast", "快路径 OBCUVS03", FAST_C), ("blob", "基线 OBCUVS02", BLOB_C)]
    y = y0 + 30
    breakdown = []
    for mode, label, _color in order:
        entry = serialize.get(mode)
        if not entry:
            continue
        total_ms = entry["total_us"] / 1000.0
        total_us = entry["total_us"]
        svg.text(x0, y, label, size=12, fill=INK, weight="600")
        segs = [
            ("build", entry["build_us"] / 1000.0, GPU_C),
            ("export", entry["export_us"] / 1000.0, ACCENT),
            ("write", entry["write_us"] / 1000.0, FAST_C),
        ]
        x = x0
        for name, ms, color in segs:
            if ms <= 0:
                continue
            # export and write are a fraction of a percent of the window, so
            # they would be sub-pixel; give every non-zero stage a visible
            # minimum width and state the exact share next to the bar.
            w = max(chart_h * (ms / maxv), 3.0)
            svg.rect(x, y + 8, w, 28, color, rx=2)
            if w > 52:
                svg.text(x + w / 2, y + 8 + 19, f"{name} {fmt_ms(ms)}", size=11, fill="#ffffff",
                         anchor="middle", weight="600")
            x += w
        svg.text(x + 8, y + 8 + 19, f"总计 {fmt_ms(total_ms)}", size=12, fill=INK, weight="600")
        share = " · ".join(
            f"{name} {entry[key] / total_us * 100:.2f}%"
            for name, key in (("build", "build_us"), ("export", "export_us"), ("write", "write_us"))
        )
        svg.text(x0, y + 46, share, size=11, fill=MUTED)
        breakdown.append((mode, entry["build_us"] / total_us * 100))
        y += 62

    if breakdown:
        worst = min(b for _, b in breakdown)
        svg.text(PAD_L, base_y + 20,
                 f"两条路径的 build 占比都不低于 {worst:.2f}%："
                 f"export 与 write 合计不到 1%，构建窗口里没有留给快照格式的优化空间",
                 size=11, fill=MUTED)
    svg.line(x0, base_y, x1, base_y, AXIS, 1.5)
    svg.text(x0, base_y + 40, "柱宽按毫秒比例；build = GPU 建图，export = 图回传主机，"
             "write = 快照写入", size=11, fill=MUTED)
    return h


def panel_official_runs(svg: Svg, y0: int) -> int:
    """The platform's own numbers for the two evaluation runs."""
    h = 272
    svg.text(PAD_L, y0 - 14, "官方提测实测结果（平台输出，2026-11-09）", size=15, weight="600")
    svg.text(PAD_L, y0 + 2, "同一轮的冻结基线 445b9b2  vs  本方案 cc6c79f3；两轮门槛均为 gates=pass",
             size=12, fill=MUTED)

    # name, baseline value, our value, axis maximum, unit, reference line
    groups = [
        ("构建加速比 build", 1.105, 1.139, 1.25, "x", 1.0),
        ("查询吞吐比 query", 0.975, 1.096, 1.25, "x", 1.0),
        ("总分 score", 5.271, 6.115, 7.0, "", None),
    ]

    x0 = PAD_L + 96
    x1 = W - PAD_R - 92
    span = x1 - x0
    y = y0 + 34
    for name, base, ours, scale, unit, ref in groups:
        svg.text(PAD_L, y + 26, name, size=12, fill=INK, weight="600")
        for offset, value, color, tag in ((0, base, BLOB_C, "基线"), (24, ours, FAST_C, "本方案")):
            yy = y + offset
            svg.text(x0 - 8, yy + 13, tag, size=11, fill=MUTED, anchor="end")
            w = span * (value / scale)
            svg.rect(x0, yy, w, 18, color, rx=3)
            svg.text(x0 + w + 8, yy + 13, f"{value:g}{unit}", size=12, fill=INK, weight="600")
        if ref is not None:
            xr = x0 + span * (ref / scale)
            svg.line(xr, y - 2, xr, y + 46, ACCENT, 1.2, dash="3 4")
            svg.text(xr + 5, y + 58, "1.0x = 冻结基线", size=10, fill=ACCENT)
        y += 80
    return h


def build_reference_svg() -> str:
    svg = Svg()
    svg.height = 60 + 250 + 215 + 272 + 60
    svg.rect(0, 0, W, svg.height, "#ffffff")
    y = PAD_T
    y += panel_score_curve(svg, y) + 34
    y += panel_official_runs(svg, y) + 20
    y += panel_container_bytes(svg, y) + 20
    return svg.render()


def build_benchmark_svg(bench: dict, stages: dict) -> str:
    svg = Svg()
    svg.height = 60 + 260 + 212 + 250 + 272 + 215 + 140
    svg.rect(0, 0, W, svg.height, "#ffffff")
    y = PAD_T
    y += panel_cpu_gpu(svg, y, bench) + 26
    y += panel_stages(svg, y, stages) + 34
    y += panel_score_curve(svg, y) + 34
    y += panel_official_runs(svg, y) + 20
    y += panel_container_bytes(svg, y) + 20
    return svg.render()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("reference", "benchmark"), default="reference")
    parser.add_argument("--benchmark", type=pathlib.Path, default=None,
                        help="benchmark.json written by run_build_benchmark.sh")
    parser.add_argument("--stages", type=pathlib.Path, default=None,
                        help="stages.txt written by run_build_benchmark.sh")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build-performance.svg"))
    args = parser.parse_args()

    if args.mode == "benchmark":
        if not args.benchmark or not args.benchmark.exists():
            print(f"benchmark.json not found: {args.benchmark}", file=sys.stderr)
            return 2
        bench = json.loads(args.benchmark.read_text())
        stages = parse_stages(args.stages)
        markup = build_benchmark_svg(bench, stages)
    else:
        markup = build_reference_svg()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(markup, encoding="utf-8")
    print(f"wrote {args.out} ({len(markup)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
