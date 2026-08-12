#!/usr/bin/env python3
"""Generate dependency-free SVG paper-figure candidates from SF attribution CSVs."""

from __future__ import annotations

import argparse
import csv
import html
import math
from collections import defaultdict
from pathlib import Path


WIDTH = 1120
HEIGHT = 720
FONT = "Arial, Helvetica, sans-serif"
COLORS = {
    "same_candidate": "#4C78A8",
    "cross_candidate": "#E45756",
    "same_baseline": "#72A0C1",
    "cross_baseline": "#F2A19D",
    "original": "#4C78A8",
    "probe_only": "#E45756",
    "joint": "#54A24B",
    "idle": "#8A8A8A",
    "grid": "#D9D9D9",
    "axis": "#333333",
}


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


class SVG:
    def __init__(self, width: int = WIDTH, height: int = HEIGHT, title: str = ""):
        self.width = width
        self.height = height
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}">',
            '<rect width="100%" height="100%" fill="white"/>',
        ]
        if title:
            self.text(width / 2, 31, title, size=19, weight="bold", anchor="middle")

    def line(self, x1, y1, x2, y2, color="#333", width=1.2, dash=None, opacity=1):
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        self.parts.append(
            f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" '
            f'stroke="{color}" stroke-width="{width}" opacity="{opacity}"{dash_attr}/>'
        )

    def rect(self, x, y, width, height, fill="none", stroke="none", sw=1, opacity=1, rx=0):
        self.parts.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{width:.2f}" height="{height:.2f}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}" opacity="{opacity}" rx="{rx}"/>'
        )

    def circle(self, x, y, r=3.5, fill="#333", stroke="white", sw=0.8):
        self.parts.append(
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"/>'
        )

    def polyline(self, points, color, width=2.2, dash=None):
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        value = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
        self.parts.append(
            f'<polyline points="{value}" fill="none" stroke="{color}" '
            f'stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round"{dash_attr}/>'
        )

    def text(self, x, y, value, size=13, fill="#222", anchor="start", weight="normal", rotate=None):
        transform = f' transform="rotate({rotate} {x:.2f} {y:.2f})"' if rotate is not None else ""
        self.parts.append(
            f'<text x="{x:.2f}" y="{y:.2f}" font-family="{FONT}" font-size="{size}" '
            f'fill="{fill}" text-anchor="{anchor}" font-weight="{weight}"{transform}>{esc(value)}</text>'
        )

    def save(self, path: Path):
        path.write_text("\n".join(self.parts + ["</svg>"]), encoding="utf-8")


def read_csv(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def f(row, field):
    return float(row[field])


def item_rows(rows):
    result = defaultdict(dict)
    for row in rows:
        if row["record"] == "item_sweep":
            result[int(row["test_bit"])][row["variant"]] = row
    return dict(sorted(result.items()))


def weighted(rows, field):
    total_n = sum(int(row["repetitions"]) for row in rows)
    if not total_n:
        return math.nan
    return sum(float(row[field]) * int(row["repetitions"]) for row in rows) / total_n


def line_axes(svg, box, xlim, ylim, xlabel, ylabel, xticks, yticks, markers=()):
    x, y, w, h = box
    xmin, xmax = xlim
    ymin, ymax = ylim

    def mx(v):
        return x + (v - xmin) / (xmax - xmin) * w

    def my(v):
        return y + h - (v - ymin) / (ymax - ymin) * h

    for tick in yticks:
        py = my(tick)
        svg.line(x, py, x + w, py, COLORS["grid"], 0.9)
        svg.text(x - 10, py + 4, f"{tick:g}", size=11, anchor="end", fill="#555")
    for tick in xticks:
        px = mx(tick)
        svg.line(px, y + h, px, y + h + 5, COLORS["axis"], 1)
        svg.text(px, y + h + 21, f"{tick:g}", size=11, anchor="middle", fill="#555")
    for value, label, color in markers:
        px = mx(value)
        svg.line(px, y, px, y + h, color, 1.1, dash="5,5", opacity=0.75)
        svg.text(px + 4, y + 14, label, size=10, fill=color)
    svg.line(x, y + h, x + w, y + h, COLORS["axis"], 1.2)
    svg.line(x, y, x, y + h, COLORS["axis"], 1.2)
    svg.text(x + w / 2, y + h + 47, xlabel, size=13, anchor="middle")
    svg.text(x - 57, y + h / 2, ylabel, size=13, anchor="middle", rotate=-90)
    return mx, my


def legend(svg, entries, x, y, columns=1, column_width=190):
    for index, (label, color, dash) in enumerate(entries):
        col = index % columns
        row = index // columns
        px = x + col * column_width
        py = y + row * 24
        svg.line(px, py, px + 28, py, color, 2.4, dash=dash)
        if not dash:
            svg.circle(px + 14, py, 3.1, color)
        svg.text(px + 36, py + 4, label, size=11.5)


def plot_item_all(items, out):
    svg = SVG(title="Mean probe latency with same-core and cross-core baselines")
    box = (92, 68, 970, 500)
    markers = ((4, "L1 onset", "#777"), (8, "L2 onset", "#777"),
               (15, "cross transition", COLORS["cross_candidate"]),
               (19, "same transition", COLORS["same_candidate"]))
    mx, my = line_axes(svg, box, (0, 24), (60, 310),
                       "Stimulus items (probe excluded)", "Mean probe latency (cycles)",
                       range(0, 25, 2), range(60, 311, 50), markers)
    specs = (
        ("same_candidate", "Same-core candidate", None),
        ("cross_candidate", "Cross-core candidate", None),
        ("same_baseline", "Same-core baseline", "7,4"),
        ("cross_baseline", "Cross-core baseline", "2,4"),
    )
    for variant, label, dash in specs:
        points = [(mx(n), my(f(values[variant], "mean_ticks"))) for n, values in items.items()]
        svg.polyline(points, COLORS[variant], 2.5 if "candidate" in variant else 1.9, dash)
        for px, py in points:
            svg.circle(px, py, 3.0 if "candidate" in variant else 2.2, COLORS[variant])
    legend(svg, [(label, COLORS[v], dash) for v, label, dash in specs], 120, 675, columns=4, column_width=245)
    svg.save(out)


def plot_item_panels(items, out):
    svg = SVG(title="Candidate–baseline mean latency by execution locality")
    panels = ((75, 78, 470, 475), (625, 78, 420, 475))
    definitions = (
        ("Same-core", "same_candidate", "same_baseline", COLORS["same_candidate"]),
        ("Cross-core", "cross_candidate", "cross_baseline", COLORS["cross_candidate"]),
    )
    for box, (title, candidate, baseline, color) in zip(panels, definitions):
        mx, my = line_axes(svg, box, (0, 24), (60, 310),
                           "Stimulus items", "Mean latency (cycles)",
                           range(0, 25, 4), range(60, 311, 50), ())
        svg.text(box[0] + box[2] / 2, 67, title, size=15, weight="bold", anchor="middle")
        for variant, label, dash, series_color in (
            (candidate, "Candidate", None, color),
            (baseline, "Baseline", "7,4", "#8A8A8A"),
        ):
            points = [(mx(n), my(f(values[variant], "mean_ticks"))) for n, values in items.items()]
            svg.polyline(points, series_color, 2.5, dash)
            for px, py in points:
                svg.circle(px, py, 3.0, series_color)
        legend(svg, [("Candidate", color, None), ("Baseline", "#8A8A8A", "7,4")],
               box[0] + 85, 650, columns=2, column_width=165)
    svg.save(out)


def plot_delta(items, out):
    svg = SVG(title="Mean latency increment over matched baseline")
    box = (92, 68, 970, 500)
    mx, my = line_axes(svg, box, (0, 24), (-10, 230),
                       "Stimulus items (probe excluded)",
                       "Candidate − baseline mean latency (cycles)",
                       range(0, 25, 2), range(0, 231, 50),
                       ((15, "cross ≥95%", COLORS["cross_candidate"]),
                        (19, "same ≥95%", COLORS["same_candidate"])))
    specs = (("Same-core", "same_candidate", "same_baseline", COLORS["same_candidate"]),
             ("Cross-core", "cross_candidate", "cross_baseline", COLORS["cross_candidate"]))
    for label, candidate, baseline, color in specs:
        points = [(mx(n), my(f(v[candidate], "mean_ticks") - f(v[baseline], "mean_ticks")))
                  for n, v in items.items()]
        svg.polyline(points, color, 2.7)
        for px, py in points:
            svg.circle(px, py, 3.2, color)
    legend(svg, [(x[0], x[3], None) for x in specs], 355, 675, columns=2, column_width=230)
    svg.save(out)


def plot_mean_pmu(items, out):
    svg = SVG(title="Mean latency transition aligns with the PMU LL-cache-miss event")
    boxes = ((80, 80, 445, 465), (620, 80, 425, 465))
    mx, my = line_axes(svg, boxes[0], (0, 24), (60, 310), "Stimulus items",
                       "Mean latency (cycles)", range(0, 25, 4), range(60, 311, 50), ())
    mx2, my2 = line_axes(svg, boxes[1], (0, 24), (0, 100), "Stimulus items",
                         "Trials with LL-cache miss event (%)", range(0, 25, 4), range(0, 101, 20), ())
    specs = (("same_candidate", "Same-core candidate", None),
             ("cross_candidate", "Cross-core candidate", None),
             ("same_baseline", "Same-core baseline", "7,4"),
             ("cross_baseline", "Cross-core baseline", "2,4"))
    for variant, _, dash in specs:
        color = COLORS[variant]
        latency = [(mx(n), my(f(v[variant], "mean_ticks"))) for n, v in items.items()]
        pmu = [(mx2(n), my2(100 * f(v[variant], "probe_ll_cache_miss_samples") /
                            max(1, f(v[variant], "repetitions")))) for n, v in items.items()]
        svg.polyline(latency, color, 2.2, dash)
        svg.polyline(pmu, color, 2.2, dash)
    legend(svg, [(label, COLORS[v], dash) for v, label, dash in specs], 105, 660, columns=4, column_width=245)
    svg.save(out)


def heat_color(value, low=70, high=290):
    value = max(low, min(high, value))
    t = (value - low) / (high - low)
    stops = ((247, 251, 255), (158, 202, 225), (49, 130, 189), (203, 24, 29))
    p = t * (len(stops) - 1)
    i = min(len(stops) - 2, int(p))
    frac = p - i
    rgb = tuple(round(stops[i][k] * (1 - frac) + stops[i + 1][k] * frac) for k in range(3))
    return "#%02x%02x%02x" % rgb


def aggregate_bits(rows):
    cross = defaultdict(lambda: defaultdict(list))
    same = defaultdict(lambda: defaultdict(list))
    for row in rows:
        if row["available"] != "1":
            continue
        bit = int(row["test_bit"])
        if row["record"] == "joint_bit_test":
            cross[bit][row["variant"]].append(row)
        elif row["record"] == "locality_bit_test":
            same[bit][row["variant"]].append(row)
    return cross, same


def plot_bit_heatmap(rows, out, derived_csv):
    cross, same = aggregate_bits(rows)
    bits = sorted(set(cross) & set(same))
    columns = (
        ("Cross O", cross, "original_matching"),
        ("Cross P", cross, "probe_only_flipped"),
        ("Cross J", cross, "joint_group_flipped"),
        ("Same O", same, "same_original_matching"),
        ("Same P", same, "same_probe_only_flipped"),
        ("Same J", same, "same_joint_group_flipped"),
    )
    values = []
    for bit in bits:
        values.append([weighted(mapping[bit][variant], "mean_ticks") for _, mapping, variant in columns])
    with derived_csv.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["bit"] + [label for label, _, _ in columns])
        writer.writerows([[bit] + [f"{v:.3f}" for v in row] for bit, row in zip(bits, values)])

    svg = SVG(width=980, height=820, title="Mean latency under probe-only and joint address transformations")
    x0, y0, cell_w, cell_h = 160, 78, 112, 24
    for col, (label, _, _) in enumerate(columns):
        svg.text(x0 + col * cell_w + cell_w / 2, y0 - 14, label, size=12, weight="bold", anchor="middle")
    for row_i, (bit, row_values) in enumerate(zip(bits, values)):
        y = y0 + row_i * cell_h
        svg.text(x0 - 16, y + 17, f"bit {bit}", size=11, anchor="end")
        for col, value in enumerate(row_values):
            x = x0 + col * cell_w
            color = heat_color(value)
            svg.rect(x, y, cell_w - 2, cell_h - 2, fill=color, stroke="white", sw=0.6)
            text_color = "white" if value > 190 else "#222"
            svg.text(x + (cell_w - 2) / 2, y + 16, f"{value:.0f}", size=10.5,
                     anchor="middle", fill=text_color)
    bar_x, bar_y, bar_w = 850, 145, 22
    for i in range(220):
        value = 290 - i
        svg.rect(bar_x, bar_y + i * 1.8, bar_w, 1.9, fill=heat_color(value), stroke="none")
    svg.text(bar_x + 11, bar_y - 10, "cycles", size=11, anchor="middle")
    svg.text(bar_x + 30, bar_y + 5, "290", size=10)
    svg.text(bar_x + 30, bar_y + 220 * 1.8, "70", size=10)
    svg.text(490, 790, "O: original matching   P: probe-only flipped   J: joint group flipped",
             size=12, anchor="middle", fill="#555")
    svg.save(out)


def plot_bits_16_17(rows, out):
    cross, same = aggregate_bits(rows)
    definitions = (
        ("Cross original", cross, "original_matching", COLORS["original"]),
        ("Cross probe-only", cross, "probe_only_flipped", COLORS["probe_only"]),
        ("Cross joint", cross, "joint_group_flipped", COLORS["joint"]),
        ("Same original", same, "same_original_matching", "#86A9C6"),
        ("Same probe-only", same, "same_probe_only_flipped", "#EF9A9A"),
        ("Same joint", same, "same_joint_group_flipped", "#8AC18A"),
    )
    svg = SVG(title="PA[16] and PA[17] control the cross-core congruence relation")
    box = (90, 80, 965, 465)
    x, y, w, h = box
    ymin, ymax = 60, 310

    def my(v):
        return y + h - (v - ymin) / (ymax - ymin) * h

    for tick in range(60, 311, 50):
        py = my(tick)
        svg.line(x, py, x + w, py, COLORS["grid"], 0.9)
        svg.text(x - 10, py + 4, tick, size=11, anchor="end")
    svg.line(x, y + h, x + w, y + h, COLORS["axis"], 1.2)
    svg.line(x, y, x, y + h, COLORS["axis"], 1.2)
    svg.text(x - 57, y + h / 2, "Mean probe latency (cycles)", size=13, anchor="middle", rotate=-90)
    group_centers = (355, 790)
    bar_w = 48
    gap = 8
    for center, bit in zip(group_centers, (16, 17)):
        start = center - (len(definitions) * bar_w + (len(definitions) - 1) * gap) / 2
        for i, (_, mapping, variant, color) in enumerate(definitions):
            value = weighted(mapping[bit][variant], "mean_ticks")
            bx = start + i * (bar_w + gap)
            by = my(value)
            svg.rect(bx, by, bar_w, y + h - by, fill=color, stroke="white", sw=0.8)
            svg.text(bx + bar_w / 2, by - 7, f"{value:.0f}", size=10, anchor="middle")
        svg.text(center, y + h + 30, f"bit {bit}", size=14, weight="bold", anchor="middle")
    legend(svg, [(label, color, None) for label, _, _, color in definitions], 145, 605,
           columns=3, column_width=285)
    svg.save(out)


def plot_color_matrix(path, out):
    _, rows = read_csv(path)
    active = defaultdict(list)
    idle = defaultdict(list)
    for row in rows:
        if row["record"] == "color_matrix":
            active[(int(row["test_bit"]), int(row["test_bit2"]))].append(row)
        elif row["record"] == "color_idle" and row["variant"] == "idle_cell":
            idle[(int(row["test_bit"]), int(row["test_bit2"]))].append(row)
    global_idle = weighted([row for group in idle.values() for row in group], "mean_ticks")
    svg = SVG(width=1050, height=650, title="PA[17:16] color matrix: active interference and idle baseline")
    panels = ((135, "Active candidate"), (600, "Idle baseline"))
    cell = 82
    y0 = 125
    for x0, title in panels:
        svg.text(x0 + 2 * cell, 88, title, size=15, weight="bold", anchor="middle")
        for col in range(4):
            svg.text(x0 + col * cell + cell / 2, y0 - 15, f"Probe {col:02b}", size=11, anchor="middle")
        for row_i in range(4):
            svg.text(x0 - 16, y0 + row_i * cell + cell / 2 + 4,
                     f"Stim {row_i:02b}", size=11, anchor="end")
            for col in range(4):
                key = (row_i, col)
                if title.startswith("Active"):
                    value = weighted(active[key], "mean_ticks")
                else:
                    value = weighted(idle[key], "mean_ticks") if key in idle else global_idle
                color = heat_color(value)
                text_color = "white" if value > 190 else "#222"
                svg.rect(x0 + col * cell, y0 + row_i * cell, cell - 3, cell - 3,
                         fill=color, stroke="white", sw=1)
                svg.text(x0 + col * cell + (cell - 3) / 2,
                         y0 + row_i * cell + cell / 2 + 5,
                         f"{value:.0f}", size=14, weight="bold", anchor="middle", fill=text_color)
    svg.text(525, 520, "Cell value: mean probe latency (cycles), averaged over three passes",
             size=12, anchor="middle", fill="#555")
    legend(svg, [("low latency", heat_color(78), None), ("high latency", heat_color(282), None)],
           355, 580, columns=2, column_width=220)
    svg.save(out)


def write_item_table(items, path):
    fields = ["items", "same_candidate_mean", "same_baseline_mean",
              "cross_candidate_mean", "cross_baseline_mean",
              "same_delta", "cross_delta", "same_ll_miss_pct", "cross_ll_miss_pct"]
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        for n, values in items.items():
            sc = f(values["same_candidate"], "mean_ticks")
            sb = f(values["same_baseline"], "mean_ticks")
            cc = f(values["cross_candidate"], "mean_ticks")
            cb = f(values["cross_baseline"], "mean_ticks")
            writer.writerow({
                "items": n,
                "same_candidate_mean": sc,
                "same_baseline_mean": sb,
                "cross_candidate_mean": cc,
                "cross_baseline_mean": cb,
                "same_delta": sc - sb,
                "cross_delta": cc - cb,
                "same_ll_miss_pct": 100 * f(values["same_candidate"], "probe_ll_cache_miss_samples") /
                                    f(values["same_candidate"], "repetitions"),
                "cross_ll_miss_pct": 100 * f(values["cross_candidate"], "probe_ll_cache_miss_samples") /
                                     f(values["cross_candidate"], "repetitions"),
            })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--color-matrix", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    metadata, rows = read_csv(args.csv_path)
    if metadata.get("counter") != "pmccntr_cycles":
        raise SystemExit("Expected pmccntr_cycles so that mean ticks can be labelled as cycles")
    items = item_rows(rows)
    required = {"same_candidate", "same_baseline", "cross_candidate", "cross_baseline"}
    if not items or any(set(group) != required for group in items.values()):
        raise SystemExit("Incomplete item-sweep candidate/baseline cells")

    plot_item_all(items, args.output_dir / "fig1_item_mean_all.svg")
    plot_item_panels(items, args.output_dir / "fig2_item_mean_panels.svg")
    plot_delta(items, args.output_dir / "fig3_candidate_baseline_delta.svg")
    plot_mean_pmu(items, args.output_dir / "fig4_mean_latency_pmu.svg")
    plot_bit_heatmap(rows, args.output_dir / "fig5_bitscan_mean_heatmap.svg",
                     args.output_dir / "plotted_bits_mean.csv")
    plot_bits_16_17(rows, args.output_dir / "fig6_bit16_17_mean.svg")
    if args.color_matrix:
        plot_color_matrix(args.color_matrix, args.output_dir / "fig7_color_matrix_mean.svg")
    write_item_table(items, args.output_dir / "plotted_item_mean.csv")
    print(args.output_dir)


if __name__ == "__main__":
    main()
