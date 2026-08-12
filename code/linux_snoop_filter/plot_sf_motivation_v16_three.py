#!/usr/bin/env python3
"""Create three dependency-light paper figures from SF motivation v16 CSVs."""

from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

from plot_sf_attribution_paper import SVG, line_axes
from svg_to_vector_pdf import convert as svg_to_pdf


PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "red": "#C44E52",
    "dark": "#3A3A3A",
    "gray": "#7A7A7A",
    "light_gray": "#D6D6D6",
    "grid": "#E5E5E5",
    "pale": "#F4F6F8",
}


def read_result(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def run_name(metadata):
    return metadata["experiment_run_id"].split("_cpu", 1)[0]


def write_rows(path: Path, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def median_range(values):
    values = list(values)
    return statistics.median(values), min(values), max(values)


def collect(input_dir: Path):
    item = defaultdict(dict)
    bits = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    matrix = defaultdict(lambda: defaultdict(list))
    for path in sorted(input_dir.glob("*_attribution.csv")):
        metadata, rows = read_result(path)
        run = run_name(metadata)
        for row in rows:
            if row["record"] == "item_sweep" and row["available"] == "1":
                item[(int(row["test_bit"]), row["variant"])][run] = {
                    "mean_ticks": float(row["mean_ticks"]),
                    "successes": int(row["successes"]),
                    "repetitions": int(row["repetitions"]),
                }
            elif row["record"] == "cross_bit_test" and row["available"] == "1":
                key = (int(row["test_bit"]), row["variant"])
                bits[key][run]["mean_ticks"].append(float(row["mean_ticks"]))
                bits[key][run]["successes"].append(int(row["successes"]))
                bits[key][run]["repetitions"].append(int(row["repetitions"]))
    for path in sorted(input_dir.glob("*_matrix.csv")):
        metadata, rows = read_result(path)
        run = run_name(metadata)
        for row in rows:
            if row["record"] == "color_matrix" and row["available"] == "1":
                matrix[(int(row["test_bit"]), int(row["test_bit2"]))][run].append({
                    "mean_ticks": float(row["mean_ticks"]),
                    "successes": int(row["successes"]),
                    "repetitions": int(row["repetitions"]),
                })
    return item, bits, matrix


def draw_whisker(svg, mx, my, x, center, low, high, color, marker="circle", hollow=False):
    px = mx(x)
    svg.line(px, my(low), px, my(high), color, 1.0, opacity=0.62)
    svg.line(px - 3.5, my(low), px + 3.5, my(low), color, 1.0, opacity=0.75)
    svg.line(px - 3.5, my(high), px + 3.5, my(high), color, 1.0, opacity=0.75)
    fill = "white" if hollow else color
    if marker == "square":
        svg.rect(px - 3.2, my(center) - 3.2, 6.4, 6.4, fill=fill, stroke=color, sw=1.0)
    else:
        svg.circle(px, my(center), 3.25, fill=fill, stroke=color, sw=1.0)


def legend(svg, entries, x, y, spacing):
    for index, (label, color, dash, marker) in enumerate(entries):
        px = x + index * spacing
        svg.line(px, y, px + 28, y, color, 2.2, dash)
        if marker == "square":
            svg.rect(px + 11, y - 3, 6, 6, fill=color, stroke=color)
        elif marker == "circle":
            svg.circle(px + 14, y, 3.1, color, stroke=color)
        svg.text(px + 36, y + 4, label, size=10.5, fill=PAPER["dark"])


def plot_item(item, outdir):
    summary = []
    series = {}
    for variant in ("same_candidate", "cross_candidate"):
        values = []
        for n in range(21):
            runs = item[(n, variant)]
            center, low, high = median_range(row["mean_ticks"] for row in runs.values())
            values.append((n, center, low, high))
            summary.append({"stimulus_items": n, "variant": variant, "n_runs": len(runs),
                            "median_run_mean_cycles": center,
                            "min_run_mean_cycles": low, "max_run_mean_cycles": high})
        series[variant] = values
    baseline = statistics.median(
        row["mean_ticks"]
        for n in range(21)
        for variant in ("same_baseline", "cross_baseline")
        for row in item[(n, variant)].values())

    svg = SVG(1120, 550)
    box = (85, 48, 970, 390)
    mx, my = line_axes(svg, box, (0, 20), (60, 310),
                       "Number of stimulus addresses, X", "Mean probe latency (cycles)",
                       range(0, 21, 2), range(60, 311, 50))
    svg.line(box[0], my(baseline), box[0] + box[2], my(baseline), PAPER["gray"], 1.5, "7,5")
    svg.line(mx(16), box[1], mx(16), box[1] + box[3], PAPER["dark"], 0.9, "2,4", 0.72)
    svg.text(mx(16) + 5, box[1] + 15, "X = 16", size=9, fill=PAPER["dark"])
    for variant, color, marker in (("same_candidate", PAPER["blue"], "circle"),
                                   ("cross_candidate", PAPER["orange"], "square")):
        points = [(mx(n), my(center)) for n, center, _, _ in series[variant]]
        svg.polyline(points, color, 2.4)
        for n, center, low, high in series[variant]:
            draw_whisker(svg, mx, my, n, center, low, high, color, marker)
    legend(svg, [("Same core", PAPER["blue"], None, "circle"),
                 ("Cross core", PAPER["orange"], None, "square"),
                 ("Baseline", PAPER["gray"], "7,5", None)], 180, 510, 270)
    svg.text(1045, 468, "marker: median; whisker: min–max across fresh allocations",
             size=8.5, anchor="end", fill=PAPER["gray"])
    svg_path = outdir / "sf_item_sweep_mean_latency.svg"
    svg.save(svg_path)
    svg_to_pdf(svg_path, outdir / "sf_item_sweep_mean_latency.pdf", 7.16)
    write_rows(outdir / "sf_item_sweep_plot_data.csv", summary)


def bit_run(bits, bit, variant, run):
    rows = bits[(bit, variant)][run]
    return (statistics.mean(rows["mean_ticks"]),
            100.0 * sum(rows["successes"]) / sum(rows["repetitions"]))


def plot_bits(bits, outdir):
    summary = []
    series = {}
    for variant in ("original_matching", "probe_only_flipped"):
        values = []
        for bit in range(25):
            runs = sorted(bits[(bit, variant)])
            means = [bit_run(bits, bit, variant, run)[0] for run in runs]
            success = [bit_run(bits, bit, variant, run)[1] for run in runs]
            center, low, high = median_range(means)
            values.append((bit, center, low, high, len(runs)))
            summary.append({"bit": bit, "variant": variant, "n_runs": len(runs),
                            "median_run_mean_cycles": center,
                            "min_run_mean_cycles": low, "max_run_mean_cycles": high,
                            "median_success_percent": statistics.median(success),
                            "min_success_percent": min(success), "max_success_percent": max(success)})
        series[variant] = values

    svg = SVG(1120, 550)
    box = (85, 48, 970, 390)
    mx, my = line_axes(svg, box, (0, 24), (60, 310),
                       "Flipped physical-address bit, b",
                       "Mean cross-core probe latency (cycles)",
                       range(0, 25), range(60, 311, 50))
    svg.rect(mx(0), box[1], mx(5.5) - mx(0), box[3], fill="#F4F6F8", opacity=0.72)
    svg.line(mx(5.5), box[1], mx(5.5), box[1] + box[3], PAPER["dark"], 1.0, "3,4")
    svg.text(mx(5.5) - 6, box[1] + 16, "64-B line boundary", size=9,
             anchor="end", fill=PAPER["dark"])
    for variant, color, marker in (("original_matching", PAPER["orange"], "square"),
                                   ("probe_only_flipped", PAPER["blue"], "circle")):
        points = [(mx(bit), my(center)) for bit, center, _, _, _ in series[variant]]
        svg.polyline(points, color, 2.35)
        for bit, center, low, high, n_runs in series[variant]:
            draw_whisker(svg, mx, my, bit, center, low, high, color, marker,
                         hollow=(bit == 21 and n_runs == 4))
    svg.text(mx(21), my(247), "n = 4", size=8.5, anchor="middle", fill=PAPER["gray"])
    svg.line(mx(21), my(243), mx(21), my(272), PAPER["light_gray"], 0.8)
    legend(svg, [("Original probe", PAPER["orange"], None, "square"),
                 ("Probe-only bit flip", PAPER["blue"], None, "circle")], 315, 510, 310)
    svg.text(1045, 468, "marker: median; whisker: min–max across fresh allocations",
             size=8.5, anchor="end", fill=PAPER["gray"])
    svg_path = outdir / "sf_cross_only_single_bit_mean_latency.svg"
    svg.save(svg_path)
    svg_to_pdf(svg_path, outdir / "sf_cross_only_single_bit_mean_latency.pdf", 7.16)
    write_rows(outdir / "fig2_cross_bit_plot_data.csv", summary)


def mix_color(low, high, ratio):
    def rgb(value):
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    a, b = rgb(low), rgb(high)
    result = tuple(round(x + (y - x) * ratio) for x, y in zip(a, b))
    return "#" + "".join(f"{value:02X}" for value in result)


def plot_matrix(matrix, outdir):
    summary = []
    latency = [[0.0] * 4 for _ in range(4)]
    success = [[0.0] * 4 for _ in range(4)]
    for stimulus in range(4):
        for probe in range(4):
            run_means, pooled_successes, pooled_repetitions = [], 0, 0
            for rows in matrix[(stimulus, probe)].values():
                run_means.append(statistics.mean(row["mean_ticks"] for row in rows))
                pooled_successes += sum(row["successes"] for row in rows)
                pooled_repetitions += sum(row["repetitions"] for row in rows)
            center, low, high = median_range(run_means)
            latency[stimulus][probe] = center
            success[stimulus][probe] = 100.0 * pooled_successes / pooled_repetitions
            summary.append({"stimulus_color": format(stimulus, "02b"),
                            "probe_color": format(probe, "02b"),
                            "n_runs": len(run_means), "median_run_mean_cycles": center,
                            "min_run_mean_cycles": low, "max_run_mean_cycles": high,
                            "pooled_success_percent": success[stimulus][probe],
                            "pooled_successes": pooled_successes,
                            "pooled_repetitions": pooled_repetitions})

    svg = SVG(720, 600)
    x0, y0, cell = 155, 55, 105
    labels = ["00", "01", "10", "11"]
    for stimulus in range(4):
        for probe in range(4):
            ratio = max(0.0, min(1.0, (latency[stimulus][probe] - 70.0) / 215.0))
            fill = mix_color(PAPER["pale"], PAPER["red"], ratio)
            text = "white" if latency[stimulus][probe] > 180 else PAPER["dark"]
            svg.rect(x0 + probe * cell, y0 + stimulus * cell, cell - 3, cell - 3,
                     fill=fill, stroke="white", sw=1.8)
            svg.text(x0 + probe * cell + 51, y0 + stimulus * cell + 45,
                     f"{latency[stimulus][probe]:.0f}", size=18, weight="bold",
                     anchor="middle", fill=text)
            svg.text(x0 + probe * cell + 51, y0 + stimulus * cell + 70,
                     f"{success[stimulus][probe]:.2f}%", size=10.5,
                     anchor="middle", fill=text)
        svg.text(x0 - 18, y0 + stimulus * cell + 56, labels[stimulus],
                 size=13, anchor="end", fill=PAPER["dark"])
    for probe in range(4):
        svg.text(x0 + probe * cell + 51, y0 - 14, labels[probe],
                 size=13, anchor="middle", fill=PAPER["dark"])
    svg.text(x0 + 2 * cell, y0 + 4 * cell + 38, "Probe color PA[17:16]",
             size=13, anchor="middle", fill=PAPER["dark"])
    svg.text(x0 - 70, y0 + 2 * cell, "Stimulus color PA[17:16]",
             size=13, anchor="middle", rotate=-90, fill=PAPER["dark"])
    svg.text(360, 568, "Cell: median mean latency (cycles) / pooled success rate",
             size=10, anchor="middle", fill=PAPER["gray"])
    svg_path = outdir / "sf_pa17_16_color_matrix.svg"
    svg.save(svg_path)
    svg_to_pdf(svg_path, outdir / "sf_pa17_16_color_matrix.pdf", 4.7)
    write_rows(outdir / "fig3_color_matrix_plot_data.csv", summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    item, bits, matrix = collect(args.input_dir)
    plot_item(item, args.output_dir)
    plot_bits(bits, args.output_dir)
    plot_matrix(matrix, args.output_dir)


if __name__ == "__main__":
    main()
