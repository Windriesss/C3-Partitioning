#!/usr/bin/env python3
"""Audit v15 replications and generate the compact four-panel motivation figure."""

from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path

from analyze_sf_replications import aggregate, as_int, read_result, summarize
from plot_sf_attribution_paper import SVG, line_axes


# Seaborn-deep colors already used by the paper's distribution figures.
PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "green": "#55A868",
    "red": "#C44E52",
    "purple": "#8172B3",
    "gray": "#BAB0AC",
    "dark_gray": "#595959",
    "light_blue": "#EAF1F7",
    "text": "#2F2F2F",
}


def logical_run_id(experiment_run_id: str) -> str:
    for suffix in ("_attribution", "_matrix"):
        if experiment_run_id.endswith(suffix):
            return experiment_run_id[: -len(suffix)]
    return experiment_run_id


def ratio(rows, numerator="successes", denominator="repetitions"):
    num = sum(as_int(row, numerator) for row in rows)
    den = sum(as_int(row, denominator) for row in rows)
    return num, den, 100.0 * num / den if den else math.nan


def weighted_mean(rows, field="mean_ticks"):
    den = sum(as_int(row, "repetitions") for row in rows)
    return sum(float(row[field]) * as_int(row, "repetitions") for row in rows) / den if den else math.nan


def module_hash(environment_path: Path):
    if not environment_path.exists():
        return ""
    text = environment_path.read_text(encoding="utf-8", errors="replace")
    matches = re.findall(r"\b[0-9a-fA-F]{64}\b", text)
    return matches[0].lower() if matches else ""


def write_csv(path: Path, rows: list[dict]):
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def audit(input_dir: Path):
    attribution = {}
    matrices = {}
    for path in sorted(input_dir.glob("*.csv")):
        metadata, rows = read_result(path)
        experiment = metadata.get("experiment", "")
        key = logical_run_id(metadata.get("experiment_run_id", path.stem))
        if experiment == "owned_same_cross_sf_attribution":
            attribution[key] = (path, metadata, rows)
        elif experiment == "owned_address_color_matrix":
            matrices[key] = (path, metadata, rows)

    audit_rows = []
    for key in sorted(set(attribution) | set(matrices)):
        attr = attribution.get(key)
        matrix = matrices.get(key)
        attr_rows = attr[2] if attr else []
        matrix_rows = matrix[2] if matrix else []
        cross_pass = [row for row in attr_rows if row.get("record") == "pass_validation" and row.get("variant") == "cross_fixed_set_matching"]
        same_pass = [row for row in attr_rows if row.get("record") == "pass_validation" and row.get("variant") == "same_fixed_set_matching"]
        diagonal = [row for row in matrix_rows if row.get("record") == "color_matrix" and row.get("variant") == "diagonal"]
        off_diagonal = [row for row in matrix_rows if row.get("record") == "color_matrix" and row.get("variant") == "off_diagonal"]
        cross_num, cross_den, cross_pct = ratio(cross_pass)
        same_num, same_den, same_pct = ratio(same_pass)
        diagonal_num, diagonal_den, diagonal_pct = ratio(diagonal)
        off_num, off_den, off_pct = ratio(off_diagonal)
        attr_meta = attr[1] if attr else {}
        matrix_meta = matrix[1] if matrix else {}
        environment = input_dir / f"{key}_environment.txt"
        manifest = input_dir / f"{key}_manifest.csv"
        flags = []
        if not attr or not matrix or not environment.exists() or not manifest.exists():
            flags.append("incomplete_bundle")
        if attr_meta.get("schema_version") != "15" or matrix_meta.get("schema_version") != "15":
            flags.append("schema_not_v15")
        if attr_meta.get("owned_pages_only") != "true" or matrix_meta.get("owned_pages_only") != "true":
            flags.append("owned_page_flag_missing")
        if attr_meta.get("ll_cache_events_available") != "true":
            flags.append("ll_events_unavailable")
        if cross_pct < 95.0:
            flags.append("cross_fixed_set_unstable")
        if diagonal_pct < 95.0 or off_pct > 5.0:
            flags.append("matrix_separation_failed")
        audit_rows.append({
            "logical_run": key,
            "complete_bundle": int(attr is not None and matrix is not None and environment.exists() and manifest.exists()),
            "probe_cpu": attr_meta.get("probe_cpu", ""),
            "stimulus_cpus": attr_meta.get("stimulus_cpus", ""),
            "pool_pages": attr_meta.get("pool_pages", ""),
            "active_candidates": attr_meta.get("active_candidates", ""),
            "attribution_search_attempt": attr_meta.get("candidate_search_attempt", ""),
            "matrix_search_attempt": matrix_meta.get("candidate_search_attempt", ""),
            "attribution_owned_pool": attr_meta.get("owned_candidate_pool", ""),
            "matrix_owned_pool": matrix_meta.get("owned_candidate_pool", ""),
            "attribution_counter_calibration": attr_meta.get("counter_calibration_delta", ""),
            "matrix_counter_calibration": matrix_meta.get("counter_calibration_delta", ""),
            "ll_cache_events_available": attr_meta.get("ll_cache_events_available", ""),
            "cross_pass_successes": cross_num,
            "cross_pass_repetitions": cross_den,
            "cross_pass_percent": cross_pct,
            "same_pass_successes": same_num,
            "same_pass_repetitions": same_den,
            "same_pass_percent": same_pct,
            "matrix_diagonal_successes": diagonal_num,
            "matrix_diagonal_repetitions": diagonal_den,
            "matrix_diagonal_percent": diagonal_pct,
            "matrix_off_diagonal_successes": off_num,
            "matrix_off_diagonal_repetitions": off_den,
            "matrix_off_diagonal_percent": off_pct,
            "module_sha256": module_hash(environment),
            "quality_flags": ";".join(flags) if flags else "OK",
        })
    return attribution, matrices, audit_rows


def median_range(values):
    values = [float(value) for value in values]
    return statistics.median(values), min(values), max(values)


def summary_lookup(rows, keys):
    return {tuple(row[key] for key in keys): row for row in rows}


def whisker(svg, mx, my, x, row, prefix, color):
    median = float(row[f"{prefix}_median"])
    minimum = float(row[f"{prefix}_min"])
    maximum = float(row[f"{prefix}_max"])
    px = mx(x)
    svg.line(px, my(minimum), px, my(maximum), color, 1.15, opacity=0.75)
    svg.line(px - 4, my(minimum), px + 4, my(minimum), color, 1.15)
    svg.line(px - 4, my(maximum), px + 4, my(maximum), color, 1.15)
    svg.circle(px, my(median), 3.6, color)


def legend(svg, entries, x, y, spacing=145):
    for index, (label, color, dash) in enumerate(entries):
        px = x + index * spacing
        svg.line(px, y, px + 25, y, color, 2.2, dash)
        svg.text(px + 31, y + 4, label, size=10.5)


def plot_motivation(item_summary, joint_summary, matrix_summary, output_path: Path):
    pair = "cpu6_to_cpu7"
    item = summary_lookup([row for row in item_summary if row["pair"] == pair], ["items", "variant"])
    joint = summary_lookup([row for row in joint_summary if row["pair"] == pair], ["bit", "variant"])
    matrix = summary_lookup([row for row in matrix_summary if row["pair"] == pair], ["candidate_color", "probe_color"])
    items = list(range(13, 21))
    svg = SVG(1420, 850)

    # Panel (a): latency item sweep.
    box_a = (75, 70, 555, 290)
    mx, my = line_axes(svg, box_a, (13, 20), (60, 310), "Stimulus items N (probe excluded)", "Mean probe latency (cycles)", items, range(60, 311, 50))
    svg.text(75, 39, "(a) Same-core vs. cross-core item sweep", size=15, weight="bold")
    for variant, color in (("same_candidate", PAPER["blue"]), ("cross_candidate", PAPER["red"])):
        points = []
        for n in items:
            row = item[(n, variant)]
            points.append((mx(n), my(float(row["mean_ticks_median"]))))
            whisker(svg, mx, my, n, row, "mean_ticks", color)
        svg.polyline(points, color, 2.5)
    for variant, color in (("same_baseline", PAPER["dark_gray"]), ("cross_baseline", PAPER["dark_gray"])):
        svg.polyline([(mx(n), my(float(item[(n, variant)]["mean_ticks_median"]))) for n in items], color, 1.4, "6,4")
    svg.line(box_a[0], my(150), box_a[0] + box_a[2], my(150), "#8A8A8A", 0.9, "2,4")
    svg.text(box_a[0] + box_a[2] - 5, my(150) - 6, "150-cycle threshold", size=9, anchor="end", fill="#666666")
    legend(svg, [("Same-core", PAPER["blue"], None), ("Cross-core", PAPER["red"], None), ("Matched baseline", PAPER["dark_gray"], "6,4")], 125, 438, 175)

    # Panel (b): LL miss incidence.
    box_b = (795, 70, 555, 290)
    mx, my = line_axes(svg, box_b, (13, 20), (0, 100), "Stimulus items N (probe excluded)", "Trials with LL-cache-miss event (%)", items, range(0, 101, 20))
    svg.text(795, 39, "(b) LL-cache-miss event incidence", size=15, weight="bold")
    for variant, color in (("same_candidate", PAPER["blue"]), ("cross_candidate", PAPER["red"])):
        points = []
        for n in items:
            row = item[(n, variant)]
            points.append((mx(n), my(float(row["ll_miss_percent_median"]))))
            whisker(svg, mx, my, n, row, "ll_miss_percent", color)
        svg.polyline(points, color, 2.5)
    legend(svg, [("Same-core", PAPER["blue"], None), ("Cross-core", PAPER["red"], None)], 935, 438, 190)

    # Panel (c): bit rescue bars.
    box_c = (75, 510, 555, 260)
    mx, my = line_axes(svg, box_c, (-0.6, 5.6), (0, 310), "", "Mean probe latency (cycles)", [], range(0, 311, 50))
    svg.text(75, 474, "(c) Single-bit isolation and joint rescue", size=15, weight="bold")
    order = [(16, "original_matching"), (16, "probe_only_flipped"), (16, "joint_group_flipped"), (17, "original_matching"), (17, "probe_only_flipped"), (17, "joint_group_flipped")]
    labels = {"original_matching": "Original", "probe_only_flipped": "Probe only", "joint_group_flipped": "Joint"}
    bar_colors = {"original_matching": PAPER["blue"], "probe_only_flipped": PAPER["gray"], "joint_group_flipped": PAPER["orange"]}
    for index, key in enumerate(order):
        row = joint[key]
        median = float(row["mean_ticks_median"])
        minimum = float(row["mean_ticks_min"])
        maximum = float(row["mean_ticks_max"])
        x = mx(index)
        y = my(median)
        svg.rect(x - 30, y, 60, my(0) - y, fill=bar_colors[key[1]], opacity=0.95)
        svg.line(x, my(minimum), x, my(maximum), "#222", 1.1)
        svg.line(x - 5, my(minimum), x + 5, my(minimum), "#222", 1.1)
        svg.line(x - 5, my(maximum), x + 5, my(maximum), "#222", 1.1)
        svg.text(x, y - 8, f"{median:.0f}", size=10, anchor="middle", weight="bold")
        svg.text(x, box_c[1] + box_c[3] + 19, labels[key[1]], size=9, anchor="middle")
    svg.line(mx(2.5), box_c[1] + 8, mx(2.5), box_c[1] + box_c[3] - 1, "#D9D9D9", 0.9)
    svg.text(mx(1), box_c[1] + box_c[3] + 40, "PA[16]", size=11, anchor="middle", weight="bold")
    svg.text(mx(4), box_c[1] + box_c[3] + 40, "PA[17]", size=11, anchor="middle", weight="bold")
    svg.line(box_c[0], my(150), box_c[0] + box_c[2], my(150), "#888", 1, "2,4")

    # Panel (d): four-color matrix.
    svg.text(795, 474, "(d) PA[17:16] color matrix", size=15, weight="bold")
    x0, y0, cell = 925, 520, 70
    names = ["00", "01", "10", "11"]
    for candidate in range(4):
        for probe in range(4):
            row = matrix[(candidate, probe)]
            latency = float(row["mean_ticks_median"])
            success = float(row["success_percent_median"])
            is_conflict = candidate == probe
            fill = PAPER["red"] if is_conflict else PAPER["light_blue"]
            text_color = "white" if is_conflict else PAPER["text"]
            svg.rect(x0 + probe * cell, y0 + candidate * cell, cell - 3, cell - 3, fill=fill, stroke="white", sw=1.5)
            svg.text(x0 + probe * cell + 34, y0 + candidate * cell + 29, f"{latency:.0f}", size=13, weight="bold", anchor="middle", fill=text_color)
            svg.text(x0 + probe * cell + 34, y0 + candidate * cell + 47, f"{success:.1f}%", size=8.5, anchor="middle", fill=text_color)
        svg.text(x0 - 13, y0 + candidate * cell + 37, names[candidate], size=11, anchor="end")
    for probe in range(4):
        svg.text(x0 + probe * cell + 34, y0 - 10, names[probe], size=11, anchor="middle")
    svg.text(x0 + 2 * cell, y0 + 4 * cell + 29, "Probe color", size=12, anchor="middle")
    svg.text(x0 - 56, y0 + 2 * cell, "Stimulus color", size=12, anchor="middle", rotate=-90)
    svg.save(output_path)


def plot_run_curves(item_run, output_path: Path):
    rows = [row for row in item_run if row["pair"] == "cpu6_to_cpu7" and row["variant"] in ("same_candidate", "cross_candidate")]
    grouped = defaultdict(dict)
    for row in rows:
        grouped[(row["run_id"], row["variant"])][int(row["items"])] = float(row["mean_ticks"])
    svg = SVG(1120, 650, "Fresh-allocation item-sweep trajectories")
    panels = ((70, 80, 450, 430), (630, 80, 420, 430))
    for panel, variant, title, color in ((panels[0], "cross_candidate", "Cross-core CPU6 to CPU7", PAPER["red"]), (panels[1], "same_candidate", "Same-core control", PAPER["blue"])):
        mx, my = line_axes(svg, panel, (13, 20), (60, 310), "Stimulus items N", "Mean probe latency (cycles)", range(13, 21), range(60, 311, 50))
        svg.text(panel[0] + panel[2] / 2, 58, title, size=14, weight="bold", anchor="middle")
        for (run_id, row_variant), values in sorted(grouped.items()):
            if row_variant != variant:
                continue
            points = [(mx(n), my(values[n])) for n in sorted(values)]
            svg.polyline(points, color, 1.25)
            for point in points:
                svg.circle(*point, 2.4, color, stroke="white", sw=0.5)
        svg.line(panel[0], my(150), panel[0] + panel[2], my(150), "#888", 1, "2,4")
    svg.text(560, 610, "Each thin trajectory is one fresh owned-page allocation; N=15 varies, while N=16 is consistently high only cross-core.", size=11, anchor="middle", fill="#555")
    svg.save(output_path)


def key_results(item_run, joint_run, matrix_run):
    output = []
    for n in (15, 16, 19):
        for variant in ("same_candidate", "cross_candidate"):
            rows = [row for row in item_run if row["items"] == n and row["variant"] == variant]
            med, low, high = median_range([row["mean_ticks"] for row in rows])
            miss_med, miss_low, miss_high = median_range([row["ll_miss_percent"] for row in rows])
            pooled_success = sum(row["success_percent"] for row in rows) / len(rows)
            output.append({"metric": f"N{n}_{variant}", "run_median": med, "run_min": low, "run_max": high, "ll_miss_median_percent": miss_med, "ll_miss_min_percent": miss_low, "ll_miss_max_percent": miss_high, "mean_of_run_trigger_percent": pooled_success})
    return output


def pooled_counts(attribution, matrices):
    output = []

    def add(name, rows):
        num, den, percent = ratio(rows)
        output.append({
            "condition": name,
            "successes": num,
            "repetitions": den,
            "success_percent": percent,
            "weighted_mean_ticks": weighted_mean(rows),
        })

    attr_rows = [row for _, _, rows in attribution.values() for row in rows]
    matrix_rows = [row for _, _, rows in matrices.values() for row in rows]
    for variant in ("same_baseline", "cross_baseline", "same_candidate", "cross_candidate"):
        add(f"item_N16_{variant}", [row for row in attr_rows if row.get("record") == "item_sweep" and row.get("test_bit") == "16" and row.get("variant") == variant and row.get("fill_rounds") == "1000"])
    for bit in (16, 17):
        for variant in ("original_matching", "probe_only_flipped", "joint_group_flipped"):
            add(f"bit{bit}_{variant}", [row for row in attr_rows if row.get("record") == "joint_bit_test" and row.get("test_bit") == str(bit) and row.get("variant") == variant and row.get("available") == "1"])
    add("matrix_diagonal", [row for row in matrix_rows if row.get("record") == "color_matrix" and row.get("variant") == "diagonal"])
    add("matrix_off_diagonal", [row for row in matrix_rows if row.get("record") == "color_matrix" and row.get("variant") == "off_diagonal"])
    add("cross_fixed_set_pass_validation", [row for row in attr_rows if row.get("record") == "pass_validation" and row.get("variant") == "cross_fixed_set_matching"])
    add("same_fixed_set_pass_validation", [row for row in attr_rows if row.get("record") == "pass_validation" and row.get("variant") == "same_fixed_set_matching"])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--fill-rounds", type=int, default=1000)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    attribution, matrices, audit_rows = audit(args.input_dir)
    paths = [entry[0] for entry in attribution.values()] + [entry[0] for entry in matrices.values()]
    item_run, joint_run, matrix_run, seen_runs = aggregate(paths, args.fill_rounds)
    item_summary = summarize(item_run, ["pair", "items", "variant"], ["mean_ticks", "ll_miss_percent", "success_percent"])
    joint_summary = summarize(joint_run, ["pair", "bit", "variant"], ["mean_ticks", "success_percent"])
    matrix_summary = summarize(matrix_run, ["pair", "candidate_color", "probe_color"], ["mean_ticks", "success_percent"])

    write_csv(args.output_dir / "run_audit.csv", audit_rows)
    write_csv(args.output_dir / "key_results.csv", key_results(item_run, joint_run, matrix_run))
    write_csv(args.output_dir / "pooled_counts.csv", pooled_counts(attribution, matrices))
    write_csv(args.output_dir / "item_run_level.csv", item_run)
    write_csv(args.output_dir / "item_median_minmax.csv", item_summary)
    write_csv(args.output_dir / "joint_run_level.csv", joint_run)
    write_csv(args.output_dir / "joint_median_minmax.csv", joint_summary)
    write_csv(args.output_dir / "matrix_run_level.csv", matrix_run)
    write_csv(args.output_dir / "matrix_median_minmax.csv", matrix_summary)
    plot_motivation(item_summary, joint_summary, matrix_summary, args.output_dir / "figure_motivation_four_panel.svg")
    plot_run_curves(item_run, args.output_dir / "figure_supplement_independent_runs.svg")

    print(f"complete logical runs: {len(audit_rows)}")
    for row in audit_rows:
        print(f"{row['logical_run']}: {row['quality_flags']}")


if __name__ == "__main__":
    main()
