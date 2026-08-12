#!/usr/bin/env python3
"""Generate the three SF-motivation figures requested by the paper outline."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

from analyze_sf_replications import aggregate, as_int, read_result, summarize
from plot_sf_attribution_paper import SVG, line_axes
from svg_to_vector_pdf import convert as svg_to_pdf


PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "green": "#55A868",
    "red": "#C44E52",
    "purple": "#8172B3",
    "gray": "#BAB0AC",
    "dark_gray": "#595959",
    "light_blue": "#EAF1F7",
    "light_gray": "#F5F5F5",
    "text": "#2F2F2F",
}


def weighted(rows, field="mean_ticks"):
    count = sum(as_int(row, "repetitions") for row in rows)
    if not count:
        return math.nan
    return sum(float(row[field]) * as_int(row, "repetitions") for row in rows) / count


def pooled_percent(rows):
    count = sum(as_int(row, "repetitions") for row in rows)
    successes = sum(as_int(row, "successes") for row in rows)
    return 100.0 * successes / count if count else math.nan


def read_rows(path: Path):
    return read_result(path)[1]


def write_csv(path: Path, rows):
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def legend(svg, entries, x, y, spacing):
    for index, (label, color, dash) in enumerate(entries):
        px = x + index * spacing
        svg.line(px, y, px + 27, y, color, 2.3, dash)
        if not dash:
            svg.circle(px + 13.5, y, 3.1, color)
        svg.text(px + 35, y + 4, label, size=10.5)


def draw_series(svg, mx, my, values, color, width=2.4, dash=None, points=True):
    coordinates = [(mx(x), my(y)) for x, y in values]
    svg.polyline(coordinates, color, width, dash)
    if points:
        for x, y in coordinates:
            svg.circle(x, y, 3.0, color)


def draw_whisker(svg, mx, my, x, center, low, high, color):
    px = mx(x)
    svg.line(px, my(low), px, my(high), color, 1.05, opacity=0.72)
    svg.line(px - 4, my(low), px + 4, my(low), color, 1.05)
    svg.line(px - 4, my(high), px + 4, my(high), color, 1.05)
    svg.circle(px, my(center), 3.2, color)


def prepare_full_item(rows):
    result = []
    for n in range(0, 21):
        for variant in ("same_baseline", "same_candidate", "cross_baseline", "cross_candidate"):
            selected = [row for row in rows if row.get("record") == "item_sweep"
                        and row.get("test_bit") == str(n)
                        and row.get("variant") == variant
                        and row.get("fill_rounds") == "1000"]
            result.append({
                "items": n,
                "variant": variant,
                "mean_ticks": weighted(selected),
                "success_percent": pooled_percent(selected),
                "source": "v14 representative allocation",
            })
    return result


def prepare_replication(replication_dir: Path):
    paths = sorted(replication_dir.glob("*_attribution.csv"))
    item_run, _, _, _ = aggregate(paths, 1000)
    summary = summarize(item_run, ["pair", "items", "variant"],
                        ["mean_ticks", "ll_miss_percent", "success_percent"])
    return [row for row in summary if row["pair"] == "cpu6_to_cpu7"
            and 13 <= int(row["items"]) <= 20]


def prepare_bits(rows):
    result = []
    definitions = (
        ("same_original", "locality_bit_test", "same_original_matching"),
        ("same_probe_only", "locality_bit_test", "same_probe_only_flipped"),
        ("cross_original", "joint_bit_test", "original_matching"),
        ("cross_probe_only", "joint_bit_test", "probe_only_flipped"),
    )
    for bit in range(0, 25):
        for output_variant, record, source_variant in definitions:
            selected = [row for row in rows if row.get("record") == record
                        and row.get("test_bit") == str(bit)
                        and row.get("variant") == source_variant
                        and row.get("available") == "1"]
            result.append({
                "bit": bit,
                "variant": output_variant,
                "mean_ticks": weighted(selected),
                "success_percent": pooled_percent(selected),
                "repetitions": sum(as_int(row, "repetitions") for row in selected),
                "source": "v14 representative allocation; three passes pooled",
            })
    return result


def prepare_matrix(replication_dir: Path):
    paths = sorted(replication_dir.glob("*_matrix.csv"))
    _, _, matrix_run, _ = aggregate(paths, 1000)
    summary = summarize(matrix_run, ["pair", "candidate_color", "probe_color"],
                        ["mean_ticks", "success_percent"])
    return [row for row in summary if row["pair"] == "cpu6_to_cpu7"]


def plot_item(full_rows, replication_rows, path: Path):
    svg = SVG(1320, 535)
    svg.text(65, 34, "(a) Full item sweep (representative fresh allocation)", size=15, weight="bold")
    left = (70, 70, 555, 325)
    mx, my = line_axes(svg, left, (0, 20), (60, 310),
                       "Stimulus items X (probe excluded)", "Mean probe latency (cycles)",
                       range(0, 21, 2), range(60, 311, 50))
    lookup = {(int(row["items"]), row["variant"]): float(row["mean_ticks"]) for row in full_rows}
    draw_series(svg, mx, my, [(n, lookup[(n, "same_candidate")]) for n in range(21)], PAPER["blue"])
    draw_series(svg, mx, my, [(n, lookup[(n, "cross_candidate")]) for n in range(21)], PAPER["red"])
    draw_series(svg, mx, my, [(n, lookup[(n, "same_baseline")]) for n in range(21)], PAPER["dark_gray"], 1.4, "6,4", False)
    draw_series(svg, mx, my, [(n, lookup[(n, "cross_baseline")]) for n in range(21)], PAPER["dark_gray"], 1.4, "6,4", False)
    svg.line(mx(16), left[1], mx(16), left[1] + left[3], "#8A8A8A", 0.9, "2,4")
    svg.text(mx(16) + 5, left[1] + 16, "X=16", size=9, fill="#666666")
    svg.line(left[0], my(150), left[0] + left[2], my(150), "#8A8A8A", 0.9, "2,4")

    svg.text(710, 34, "(b) Five fresh allocations within the same boot", size=15, weight="bold")
    right = (710, 70, 555, 325)
    mx2, my2 = line_axes(svg, right, (13, 20), (60, 310),
                         "Stimulus items X (probe excluded)", "Mean probe latency (cycles)",
                         range(13, 21), range(60, 311, 50))
    repl = {(int(row["items"]), row["variant"]): row for row in replication_rows}
    for variant, color in (("same_candidate", PAPER["blue"]), ("cross_candidate", PAPER["red"])):
        values = []
        for n in range(13, 21):
            row = repl[(n, variant)]
            center = float(row["mean_ticks_median"])
            values.append((n, center))
            draw_whisker(svg, mx2, my2, n, center,
                         float(row["mean_ticks_min"]), float(row["mean_ticks_max"]), color)
        draw_series(svg, mx2, my2, values, color, points=False)
    for variant in ("same_baseline", "cross_baseline"):
        draw_series(svg, mx2, my2,
                    [(n, float(repl[(n, variant)]["mean_ticks_median"])) for n in range(13, 21)],
                    PAPER["dark_gray"], 1.4, "6,4", False)
    svg.line(mx2(16), right[1], mx2(16), right[1] + right[3], "#8A8A8A", 0.9, "2,4")
    svg.line(right[0], my2(150), right[0] + right[2], my2(150), "#8A8A8A", 0.9, "2,4")
    legend(svg, [("Same-core", PAPER["blue"], None), ("Cross-core", PAPER["red"], None),
                 ("Matched baseline", PAPER["dark_gray"], "6,4")], 345, 493, 215)
    svg.save(path)


def plot_bits(bit_rows, path: Path):
    svg = SVG(1080, 550)
    svg.text(70, 34, "Single-bit probe perturbation over PA[24:0]", size=16, weight="bold")
    box = (80, 70, 925, 350)
    mx, my = line_axes(svg, box, (0, 24), (60, 310),
                       "Flipped probe bit b", "Mean probe latency (cycles)",
                       range(0, 25, 2), range(60, 311, 50))
    # PA[5:0] changes the byte offset but not the cache-line identity.
    svg.rect(mx(0), box[1], mx(5.5) - mx(0), box[3], fill=PAPER["light_gray"])
    svg.text((mx(0) + mx(5.5)) / 2, box[1] + 18, "byte-offset control", size=9,
             anchor="middle", fill="#666666")
    lookup = {(int(row["bit"]), row["variant"]): float(row["mean_ticks"]) for row in bit_rows}
    definitions = (
        ("same_original", PAPER["blue"], "6,4", False),
        ("same_probe_only", PAPER["blue"], None, True),
        ("cross_original", PAPER["red"], "6,4", False),
        ("cross_probe_only", PAPER["red"], None, True),
    )
    for variant, color, dash, points in definitions:
        draw_series(svg, mx, my, [(bit, lookup[(bit, variant)]) for bit in range(25)],
                    color, 2.1 if dash else 2.5, dash, points)
    svg.line(box[0], my(150), box[0] + box[2], my(150), "#8A8A8A", 0.9, "2,4")
    svg.text(box[0] + box[2] - 4, my(150) - 7, "150-cycle threshold", size=9,
             anchor="end", fill="#666666")
    legend(svg, [("Same original", PAPER["blue"], "6,4"),
                 ("Same probe-only", PAPER["blue"], None),
                 ("Cross original", PAPER["red"], "6,4"),
                 ("Cross probe-only", PAPER["red"], None)], 120, 486, 225)
    svg.save(path)


def plot_matrix(matrix_rows, path: Path):
    svg = SVG(720, 620)
    svg.text(360, 36, "PA[17:16] stimulus-probe color matrix", size=17,
             weight="bold", anchor="middle")
    lookup = {(int(row["candidate_color"]), int(row["probe_color"])): row for row in matrix_rows}
    x0, y0, cell = 190, 95, 95
    names = ["00", "01", "10", "11"]
    for stimulus in range(4):
        for probe in range(4):
            row = lookup[(stimulus, probe)]
            conflict = stimulus == probe
            fill = PAPER["red"] if conflict else PAPER["light_blue"]
            text = "white" if conflict else PAPER["text"]
            latency = float(row["mean_ticks_median"])
            success = float(row["success_percent_median"])
            svg.rect(x0 + probe * cell, y0 + stimulus * cell, cell - 4, cell - 4,
                     fill=fill, stroke="white", sw=1.5)
            svg.text(x0 + probe * cell + 45, y0 + stimulus * cell + 39,
                     f"{latency:.0f}", size=17, weight="bold", anchor="middle", fill=text)
            svg.text(x0 + probe * cell + 45, y0 + stimulus * cell + 62,
                     f"{success:.1f}%", size=10, anchor="middle", fill=text)
        svg.text(x0 - 17, y0 + stimulus * cell + 50, names[stimulus], size=12, anchor="end")
    for probe in range(4):
        svg.text(x0 + probe * cell + 45, y0 - 13, names[probe], size=12, anchor="middle")
    svg.text(x0 + 2 * cell, y0 + 4 * cell + 37, "Probe color", size=13, anchor="middle")
    svg.text(x0 - 68, y0 + 2 * cell, "Stimulus color", size=13, anchor="middle", rotate=-90)
    svg.text(360, 574, "Diagonal: 59,900/60,000 (99.83%)    Off-diagonal: 0/180,000",
             size=11.5, anchor="middle", fill="#4A4A4A")
    svg.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v14", type=Path, required=True,
                        help="Representative v14 attribution CSV with X=0..24 and bit=0..26")
    parser.add_argument("--replication-dir", type=Path, required=True,
                        help="Directory containing the five v15 attribution/matrix bundles")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    figures = args.output_dir / "figures"
    processed = args.output_dir / "processed"
    figures.mkdir(parents=True, exist_ok=True)
    processed.mkdir(parents=True, exist_ok=True)

    v14_rows = read_rows(args.v14)
    full_item = prepare_full_item(v14_rows)
    replication = prepare_replication(args.replication_dir)
    bits = prepare_bits(v14_rows)
    matrix = prepare_matrix(args.replication_dir)
    write_csv(processed / "figure1_full_item_sweep.csv", full_item)
    write_csv(processed / "figure1_fresh_allocation_summary.csv", replication)
    write_csv(processed / "figure2_single_bit_scan.csv", bits)
    write_csv(processed / "figure3_color_matrix.csv", matrix)

    outputs = (
        ("figure1_item_sweep", lambda path: plot_item(full_item, replication, path), 7.16),
        ("figure2_single_bit_scan", lambda path: plot_bits(bits, path), 7.16),
        ("figure3_color_matrix", lambda path: plot_matrix(matrix, path), 4.7),
    )
    for name, plotter, width_inches in outputs:
        svg_path = figures / f"{name}.svg"
        pdf_path = figures / f"{name}.pdf"
        plotter(svg_path)
        svg_to_pdf(svg_path, pdf_path, width_inches)
        print(svg_path)
        print(pdf_path)


if __name__ == "__main__":
    main()
