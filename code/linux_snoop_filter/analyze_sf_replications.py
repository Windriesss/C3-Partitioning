#!/usr/bin/env python3
"""Aggregate independent RK3588 SF replications and draw two paper figures."""

from __future__ import annotations

import argparse
import csv
import math
import shutil
import statistics
import subprocess
from collections import defaultdict
from pathlib import Path

from plot_sf_attribution_paper import COLORS, SVG, heat_color, line_axes


VARIANT_LABELS = {
    "original_matching": "Original",
    "probe_only_flipped": "Probe only",
    "joint_group_flipped": "Joint",
}


def read_result(path: Path):
    metadata: dict[str, str] = {}
    data_lines: list[str] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        for line in handle:
            if line.startswith("# "):
                key, _, value = line[2:].rstrip("\r\n").partition(",")
                metadata[key] = value
            elif not line.startswith("#") and line.strip():
                data_lines.append(line)
    rows = list(csv.DictReader(data_lines))
    metadata.setdefault("experiment_run_id", path.stem)
    metadata["source_path"] = str(path)
    return metadata, rows


def as_int(row, key, default=0):
    value = row.get(key, "")
    return int(value) if value not in (None, "") else default


def as_float(row, key, default=math.nan):
    value = row.get(key, "")
    return float(value) if value not in (None, "") else default


def median_range(values):
    values = [float(value) for value in values if not math.isnan(float(value))]
    if not values:
        return math.nan, math.nan, math.nan
    return statistics.median(values), min(values), max(values)


def write_csv(path: Path, rows: list[dict]):
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def weighted(rows, value_key):
    total_weight = sum(as_int(row, "repetitions") for row in rows)
    if not total_weight:
        return math.nan
    return sum(as_float(row, value_key) * as_int(row, "repetitions") for row in rows) / total_weight


def aggregate(paths: list[Path], fill_rounds: int):
    item_run = []
    joint_run = []
    matrix_run = []
    seen_runs = defaultdict(set)

    for path in paths:
        metadata, rows = read_result(path)
        run_id = metadata["experiment_run_id"]
        pair = f"cpu{metadata.get('stimulus_cpus', '?')}_to_cpu{metadata.get('probe_cpu', '?')}"
        experiment = metadata.get("experiment", "")

        if experiment == "owned_same_cross_sf_attribution":
            seen_runs[pair].add(run_id.rsplit("_attribution", 1)[0])
            for row in rows:
                if row.get("record") != "item_sweep" or as_int(row, "fill_rounds") != fill_rounds:
                    continue
                repetitions = as_int(row, "repetitions")
                miss_pct = 100.0 * as_int(row, "probe_ll_cache_miss_samples") / repetitions if repetitions else math.nan
                item_run.append({
                    "pair": pair,
                    "run_id": run_id,
                    "items": as_int(row, "test_bit"),
                    "variant": row["variant"],
                    "mean_ticks": as_float(row, "mean_ticks"),
                    "ll_miss_percent": miss_pct,
                    "success_percent": 100.0 * as_int(row, "successes") / repetitions if repetitions else math.nan,
                })

            grouped = defaultdict(list)
            for row in rows:
                if row.get("record") == "joint_bit_test" and row.get("available") == "1" and as_int(row, "test_bit") in (16, 17) and row.get("variant") in VARIANT_LABELS:
                    grouped[(as_int(row, "test_bit"), row["variant"])].append(row)
            for (bit, variant), group in grouped.items():
                total_repetitions = sum(as_int(row, "repetitions") for row in group)
                joint_run.append({
                    "pair": pair,
                    "run_id": run_id,
                    "bit": bit,
                    "variant": variant,
                    "mean_ticks": weighted(group, "mean_ticks"),
                    "success_percent": 100.0 * sum(as_int(row, "successes") for row in group) / total_repetitions,
                })

        if experiment == "owned_address_color_matrix":
            seen_runs[pair].add(run_id.rsplit("_matrix", 1)[0])
            grouped = defaultdict(list)
            for row in rows:
                if row.get("record") == "color_matrix" and row.get("available") == "1":
                    grouped[(as_int(row, "test_bit"), as_int(row, "test_bit2"))].append(row)
            for (source, destination), group in grouped.items():
                total_repetitions = sum(as_int(row, "repetitions") for row in group)
                matrix_run.append({
                    "pair": pair,
                    "run_id": run_id,
                    "candidate_color": source,
                    "probe_color": destination,
                    "mean_ticks": weighted(group, "mean_ticks"),
                    "success_percent": 100.0 * sum(as_int(row, "successes") for row in group) / total_repetitions,
                })

    return item_run, joint_run, matrix_run, seen_runs


def summarize(rows, keys, value_names):
    grouped = defaultdict(list)
    for row in rows:
        grouped[tuple(row[key] for key in keys)].append(row)
    output = []
    for key_values, group in sorted(grouped.items()):
        record = dict(zip(keys, key_values))
        record["runs"] = len({row["run_id"] for row in group})
        for value_name in value_names:
            median, minimum, maximum = median_range([row[value_name] for row in group])
            record[f"{value_name}_median"] = median
            record[f"{value_name}_min"] = minimum
            record[f"{value_name}_max"] = maximum
        output.append(record)
    return output


def draw_whisker(svg, mx, my, x, median, minimum, maximum, color):
    px = mx(x)
    svg.line(px, my(minimum), px, my(maximum), color, 1.2, opacity=0.7)
    svg.line(px - 4, my(minimum), px + 4, my(minimum), color, 1.2)
    svg.line(px - 4, my(maximum), px + 4, my(maximum), color, 1.2)
    svg.circle(px, my(median), 3.5, color)


def maybe_write_png(svg_path: Path):
    """Rasterize when a common SVG converter exists; SVG is authoritative."""
    png_path = svg_path.with_suffix(".png")
    converter = shutil.which("rsvg-convert")
    if converter:
        subprocess.run([converter, "-o", str(png_path), str(svg_path)], check=True)
        return png_path
    converter = shutil.which("magick")
    if converter:
        subprocess.run([converter, str(svg_path), str(png_path)], check=True)
        return png_path
    return None


def plot_figure_x(pair, item_summary, output_dir):
    rows = [row for row in item_summary if row["pair"] == pair]
    if not rows:
        return
    lookup = {(row["items"], row["variant"]): row for row in rows}
    items = sorted({row["items"] for row in rows})
    colors = {"same_candidate": COLORS["same_candidate"], "cross_candidate": COLORS["cross_candidate"]}
    labels = {"same_candidate": "Same-core candidate", "cross_candidate": "Cross-core candidate"}
    svg = SVG(1180, 620, f"Independent-allocation replication: {pair}")
    boxes = ((80, 88, 440, 390), (660, 88, 420, 390))
    mx_a, my_a = line_axes(svg, boxes[0], (min(items), max(items)), (60, 320),
                           "Stimulus items N (probe excluded)", "Mean probe latency (cycles)",
                           items, range(60, 321, 50))
    svg.text(300, 68, "(a) Same-core and cross-core item sweep", size=14, weight="bold", anchor="middle")
    for variant in ("same_candidate", "cross_candidate"):
        selected = [lookup[(item, variant)] for item in items if (item, variant) in lookup]
        points = [(mx_a(row["items"]), my_a(row["mean_ticks_median"])) for row in selected]
        svg.polyline(points, colors[variant], 2.5)
        for row in selected:
            draw_whisker(svg, mx_a, my_a, row["items"], row["mean_ticks_median"],
                         row["mean_ticks_min"], row["mean_ticks_max"], colors[variant])
    for variant, color in (("same_baseline", "#708090"), ("cross_baseline", "#222222")):
        selected = [lookup[(item, variant)] for item in items if (item, variant) in lookup]
        if selected:
            svg.polyline([(mx_a(row["items"]), my_a(row["mean_ticks_median"])) for row in selected], color, 1.5, "6,4")
    svg.line(boxes[0][0], my_a(150), boxes[0][0] + boxes[0][2], my_a(150), "#888888", 1, "2,4")

    mx_b, my_b = line_axes(svg, boxes[1], (min(items), max(items)), (60, 320),
                           "Stimulus items N (probe excluded)", "Mean probe latency (cycles)",
                           items, range(60, 321, 50))
    svg.text(870, 68, "(b) Latency and LL-cache-miss incidence", size=14, weight="bold", anchor="middle")
    bx, by, bw, bh = boxes[1]
    def my_percent(value):
        return by + bh - value / 100.0 * bh
    svg.line(bx + bw, by, bx + bw, by + bh, COLORS["axis"], 1.2)
    for tick in range(0, 101, 20):
        py = my_percent(tick)
        svg.line(bx + bw, py, bx + bw + 5, py, COLORS["axis"], 1)
        svg.text(bx + bw + 10, py + 4, tick, size=10, fill="#555")
    svg.text(bx + bw + 58, by + bh / 2, "Trials with LL-cache-miss event (%)", size=12, anchor="middle", rotate=90)
    for variant in ("same_candidate", "cross_candidate"):
        selected = [lookup[(item, variant)] for item in items if (item, variant) in lookup]
        latency_points = [(mx_b(row["items"]), my_b(row["mean_ticks_median"])) for row in selected]
        miss_points = [(mx_b(row["items"]), my_percent(row["ll_miss_percent_median"])) for row in selected]
        svg.polyline(latency_points, colors[variant], 2.4)
        svg.polyline(miss_points, colors[variant], 2.0, "7,4")
        for point in latency_points:
            svg.circle(*point, 3.2, colors[variant])
    legend_y = 548
    entries = [
        ("Same candidate", colors["same_candidate"], None),
        ("Cross candidate", colors["cross_candidate"], None),
        ("Same baseline", "#708090", "6,4"),
        ("Cross baseline", "#222222", "6,4"),
        ("Same LL miss", colors["same_candidate"], "7,4"),
        ("Cross LL miss", colors["cross_candidate"], "7,4"),
    ]
    for index, (label, color, dash) in enumerate(entries):
        x = 95 + (index % 3) * 360
        y = legend_y + (index // 3) * 24
        svg.line(x, y, x + 28, y, color, 2.2, dash)
        svg.text(x + 36, y + 4, label, size=11)
    out = output_dir / f"figure_x_item_pmu_{pair}.svg"
    svg.save(out)
    maybe_write_png(out)


def plot_figure_y(pair, joint_summary, matrix_summary, output_dir):
    joint_rows = [row for row in joint_summary if row["pair"] == pair]
    matrix_rows = [row for row in matrix_summary if row["pair"] == pair]
    if not joint_rows or not matrix_rows:
        return
    svg = SVG(1120, 620, f"Address-color causality: {pair}")
    order = [(bit, variant) for bit in (16, 17) for variant in VARIANT_LABELS]
    lookup = {(row["bit"], row["variant"]): row for row in joint_rows}
    colors = ["#c43b3b", "#708090", "#2d8a57"] * 2
    box = (70, 90, 465, 390)
    mx, my = line_axes(svg, box, (-0.6, 5.6), (0, 320), "Condition", "Mean probe latency (cycles)", [], range(0, 321, 50))
    svg.text(302, 68, "(a) Probe-only isolation and joint rescue", size=14, weight="bold", anchor="middle")
    bar_width = 52
    for index, key in enumerate(order):
        row = lookup[key]
        x = mx(index)
        y = my(row["mean_ticks_median"])
        svg.rect(x - bar_width / 2, y, bar_width, my(0) - y, fill=colors[index], opacity=0.9)
        svg.line(x, my(row["mean_ticks_min"]), x, my(row["mean_ticks_max"]), "#222", 1.2)
        svg.line(x - 5, my(row["mean_ticks_min"]), x + 5, my(row["mean_ticks_min"]), "#222", 1.2)
        svg.line(x - 5, my(row["mean_ticks_max"]), x + 5, my(row["mean_ticks_max"]), "#222", 1.2)
        bit, variant = key
        svg.text(x, box[1] + box[3] + 19, f"b{bit}", size=10, anchor="middle")
        svg.text(x, box[1] + box[3] + 34, VARIANT_LABELS[variant], size=9, anchor="middle")
    svg.line(box[0], my(150), box[0] + box[2], my(150), "#555", 1, "2,4")

    matrix = [[math.nan for _ in range(4)] for _ in range(4)]
    for row in matrix_rows:
        matrix[int(row["candidate_color"])][int(row["probe_color"])] = row["mean_ticks_median"]
    x0, y0, cell = 690, 112, 86
    svg.text(x0 + 2 * cell, 68, "(b) Four-color latency matrix", size=14, weight="bold", anchor="middle")
    color_names = ["00", "01", "10", "11"]
    for candidate in range(4):
        for probe in range(4):
            if not math.isnan(matrix[candidate][probe]):
                value = matrix[candidate][probe]
                svg.rect(x0 + probe * cell, y0 + candidate * cell, cell - 3, cell - 3,
                         fill=heat_color(value), stroke="white", sw=1)
                svg.text(x0 + probe * cell + (cell - 3) / 2,
                         y0 + candidate * cell + cell / 2 + 5,
                         f"{value:.0f}", size=14, weight="bold", anchor="middle",
                         fill="white" if value < 190 else "#222")
        svg.text(x0 - 13, y0 + candidate * cell + cell / 2 + 4, color_names[candidate], size=11, anchor="end")
    for probe in range(4):
        svg.text(x0 + probe * cell + (cell - 3) / 2, y0 - 10, color_names[probe], size=11, anchor="middle")
    svg.text(x0 + 2 * cell, y0 + 4 * cell + 34, "Probe PA[17:16]", size=12, anchor="middle")
    svg.text(x0 - 57, y0 + 2 * cell, "Candidate PA[17:16]", size=12, anchor="middle", rotate=-90)
    svg.text(x0 + 2 * cell, y0 + 4 * cell + 62, "Cell: median across run-level mean latencies (cycles)", size=10, anchor="middle", fill="#555")
    out = output_dir / f"figure_y_joint_matrix_{pair}.svg"
    svg.save(out)
    maybe_write_png(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="CSV files or directories containing replication CSVs")
    parser.add_argument("--output-dir", type=Path, default=Path("sf_replication_analysis"))
    parser.add_argument("--fill-rounds", type=int, default=1000)
    args = parser.parse_args()

    paths = []
    for source in args.inputs:
        if source.is_dir():
            paths.extend(sorted(source.glob("*.csv")))
        elif source.suffix.lower() == ".csv":
            paths.append(source)
    if not paths:
        parser.error("no CSV files found")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    item_run, joint_run, matrix_run, seen_runs = aggregate(paths, args.fill_rounds)
    item_summary = summarize(item_run, ["pair", "items", "variant"], ["mean_ticks", "ll_miss_percent", "success_percent"])
    joint_summary = summarize(joint_run, ["pair", "bit", "variant"], ["mean_ticks", "success_percent"])
    matrix_summary = summarize(matrix_run, ["pair", "candidate_color", "probe_color"], ["mean_ticks", "success_percent"])

    write_csv(args.output_dir / "run_level_item_metrics.csv", item_run)
    write_csv(args.output_dir / "run_level_joint_metrics.csv", joint_run)
    write_csv(args.output_dir / "run_level_color_matrix.csv", matrix_run)
    write_csv(args.output_dir / "item_median_minmax.csv", item_summary)
    write_csv(args.output_dir / "joint_median_minmax.csv", joint_summary)
    write_csv(args.output_dir / "color_matrix_median_minmax.csv", matrix_summary)

    pairs = sorted(set(row["pair"] for row in item_summary) & set(row["pair"] for row in matrix_summary))
    for pair in pairs:
        plot_figure_x(pair, item_summary, args.output_dir)
        plot_figure_y(pair, joint_summary, matrix_summary, args.output_dir)
    for pair, runs in sorted(seen_runs.items()):
        status = "OK" if len(runs) >= 3 else "NEEDS_MORE_RUNS"
        print(f"{pair}: independent run IDs={len(runs)} ({status}; target >=3)")


if __name__ == "__main__":
    main()
