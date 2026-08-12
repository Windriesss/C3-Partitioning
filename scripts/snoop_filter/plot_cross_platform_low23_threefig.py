#!/usr/bin/env python3
"""Plot RK3588 and RK3568 together for the three SF experiments."""

from __future__ import annotations

import argparse
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.lines import Line2D

from sf_plot_common import (PAPER, configure_matplotlib, median_range,
                            print_outputs, project_paths, read_result,
                            run_name, save_figure, style_axis, write_rows)


PLATFORMS = {
    "RK3588": {"color": PAPER["blue"], "mask": "0x1ffffff",
               "target": "0x138e900", "matrix_bits": (16, 17)},
    "RK3568": {"color": PAPER["orange"], "mask": "0x7fffff",
               "target": "0x38e900", "matrix_bits": (13, 14)},
}
LOCALITY = {
    "same_candidate": {"label": "Same core", "linestyle": (0, (3.2, 2.0)),
                       "marker": "o", "filled": False},
    "cross_candidate": {"label": "Cross core", "linestyle": "-",
                        "marker": "s", "filled": True},
}
BIT_VARIANTS = {
    "original_matching": {"label": "Original", "linestyle": "-",
                          "marker": "s", "filled": True},
    "probe_only_flipped": {"label": "Probe-only flip",
                           "linestyle": (0, (3.2, 2.0)),
                           "marker": "o", "filled": False},
}


def validate(metadata, path, platform, *, matrix=False):
    spec = PLATFORMS[platform]
    expected = {"addrmask": spec["mask"],
                "candidate_target_masked": spec["target"]}
    if matrix:
        expected.update({"color_bit0": str(spec["matrix_bits"][0]),
                         "color_bit1": str(spec["matrix_bits"][1])})
    for key, wanted in expected.items():
        if metadata.get(key) != wanted:
            raise SystemExit(
                f"{path.name}: expected {key}={wanted}, got {metadata.get(key)!r}")


def collect(directory, platform):
    item = defaultdict(dict)
    bits = defaultdict(lambda: defaultdict(list))
    matrix = defaultdict(lambda: defaultdict(list))
    attribution_files = sorted(directory.glob("*_attribution.csv"))
    matrix_files = sorted(directory.glob("*_matrix.csv"))
    if len(attribution_files) != 5 or len(matrix_files) != 5:
        raise SystemExit(
            f"{platform}: expected 5 attribution and 5 matrix files, got "
            f"{len(attribution_files)} and {len(matrix_files)}")
    for path in attribution_files:
        metadata, rows = read_result(path)
        validate(metadata, path, platform)
        run = run_name(metadata)
        for row in rows:
            if row.get("available") != "1":
                continue
            if row["record"] == "item_sweep":
                item[(int(row["active_candidates"]), row["variant"])][run] = (
                    float(row["mean_ticks"]))
            elif row["record"] == "cross_bit_test":
                bit = int(row["test_bit"])
                if bit <= 22:
                    bits[(bit, row["variant"])][run].append(
                        float(row["mean_ticks"]))
    for path in matrix_files:
        metadata, rows = read_result(path)
        validate(metadata, path, platform, matrix=True)
        run = run_name(metadata)
        for row in rows:
            if row["record"] == "color_matrix" and row["available"] == "1":
                matrix[(int(row["test_bit"]), int(row["test_bit2"]))][run].append({
                    "mean": float(row["mean_ticks"]),
                    "successes": int(row["successes"]),
                    "repetitions": int(row["repetitions"]),
                })
    return {"item": item, "bits": bits, "matrix": matrix,
            "allocations": len(attribution_files)}


def direct_series(data, keys, variant, maximum, summary, platform, key_name):
    centers, lows, highs, counts = [], [], [], []
    for key in keys:
        runs = data[(int(key), variant)]
        if len(runs) != maximum:
            raise SystemExit(
                f"{platform} {key_name}={key} {variant}: "
                f"expected {maximum}, got {len(runs)}")
        center, low, high = median_range(runs.values())
        centers.append(center); lows.append(low); highs.append(high)
        counts.append(len(runs))
        summary.append({"platform": platform, key_name: int(key),
                        "variant": variant, "n_allocations": len(runs),
                        "median_run_mean_cycles": center,
                        "min_run_mean_cycles": low,
                        "max_run_mean_cycles": high})
    return tuple(np.asarray(v) for v in (centers, lows, highs)), counts


def pass_series(data, keys, variant, maximum, summary, platform):
    centers, lows, highs, counts = [], [], [], []
    for bit in keys:
        runs = data[(int(bit), variant)]
        if not 1 <= len(runs) <= maximum:
            raise SystemExit(f"{platform} bit={bit} {variant}: no measurements")
        if {len(v) for v in runs.values()} != {3}:
            raise SystemExit(f"{platform} bit={bit} {variant}: expected 3 passes")
        run_means = [statistics.mean(v) for v in runs.values()]
        center, low, high = median_range(run_means)
        centers.append(center); lows.append(low); highs.append(high)
        counts.append(len(runs))
        summary.append({"platform": platform, "bit": int(bit),
                        "variant": variant, "n_allocations": len(runs),
                        "passes_per_allocation": 3,
                        "median_run_mean_cycles": center,
                        "min_run_mean_cycles": low,
                        "max_run_mean_cycles": high})
    return tuple(np.asarray(v) for v in (centers, lows, highs)), counts


def draw(ax, x, values, platform, variant_spec):
    center, low, high = values
    color = PLATFORMS[platform]["color"]
    ax.fill_between(x, low, high, color=color, alpha=0.065,
                    linewidth=0, zorder=1)
    ax.plot(x, center, color=color, linestyle=variant_spec["linestyle"],
            marker=variant_spec["marker"], markersize=3.9,
            markerfacecolor=color if variant_spec["filled"] else "white",
            markeredgecolor=color, markeredgewidth=0.8,
            linewidth=1.6, zorder=3)


def legends(ax, variant_specs, counts):
    platform_handles = [
        Line2D([0], [0], color=PLATFORMS[p]["color"], linewidth=2,
               label=f"{p} ({counts[p]})") for p in ("RK3588", "RK3568")]
    variant_handles = [
        Line2D([0], [0], color=PAPER["dark"], linewidth=1.6,
               linestyle=variant_specs[v]["linestyle"],
               marker=variant_specs[v]["marker"], markersize=4,
               markerfacecolor=(PAPER["dark"] if variant_specs[v]["filled"]
                                 else "white"), label=variant_specs[v]["label"])
        for v in variant_specs]
    first = ax.legend(handles=platform_handles, frameon=False, ncol=2,
                      loc="lower left", bbox_to_anchor=(0, 1.005),
                      borderaxespad=0, columnspacing=1.3, handlelength=2.2)
    ax.add_artist(first)
    ax.legend(handles=variant_handles, frameon=False, ncol=2,
              loc="lower right", bbox_to_anchor=(1, 1.005),
              borderaxespad=0, columnspacing=1.3, handlelength=2.2)


def item_legend(ax):
    """Compact in-panel legend for the four item-sweep encodings."""
    platform_handles = [
        Line2D([0], [0], color=PLATFORMS[p]["color"], linewidth=2.2,
               label=p) for p in ("RK3588", "RK3568")]
    locality_handles = [
        Line2D([0], [0], color=PAPER["dark"], linewidth=1.7,
               linestyle=LOCALITY[v]["linestyle"],
               marker=LOCALITY[v]["marker"], markersize=4.4,
               markerfacecolor=(PAPER["dark"] if LOCALITY[v]["filled"]
                                 else "white"),
               markeredgecolor=PAPER["dark"],
               label=LOCALITY[v]["label"])
        for v in ("same_candidate", "cross_candidate")]
    ax.legend(handles=platform_handles + locality_handles,
              loc="upper left", ncol=2, frameon=False,
              borderaxespad=0.45, columnspacing=1.35,
              handlelength=2.3, handletextpad=0.55)


def bit_legend(ax):
    """Compact in-panel legend for the single-bit scan encodings."""
    platform_handles = [
        Line2D([0], [0], color=PLATFORMS[p]["color"], linewidth=2.2,
               label=p) for p in ("RK3588", "RK3568")]
    variant_handles = [
        Line2D([0], [0], color=PAPER["dark"], linewidth=1.7,
               linestyle=BIT_VARIANTS[v]["linestyle"],
               marker=BIT_VARIANTS[v]["marker"], markersize=4.4,
               markerfacecolor=(PAPER["dark"] if BIT_VARIANTS[v]["filled"]
                                 else "white"),
               markeredgecolor=PAPER["dark"],
               label=BIT_VARIANTS[v]["label"])
        for v in ("original_matching", "probe_only_flipped")]
    ax.legend(handles=platform_handles + variant_handles,
              loc="upper left", ncol=2, frameon=False,
              borderaxespad=0.45, columnspacing=1.35,
              handlelength=2.3, handletextpad=0.55)


def matrix_summary(dataset, platform, output):
    latency = np.zeros((4, 4))
    success = np.zeros((4, 4))
    for stimulus in range(4):
        for probe in range(4):
            runs = dataset[(stimulus, probe)]
            if len(runs) != 5 or {len(v) for v in runs.values()} != {3}:
                raise SystemExit(f"{platform} matrix {stimulus},{probe}: incomplete")
            run_means, pooled = [], []
            for rows in runs.values():
                repetitions = sum(r["repetitions"] for r in rows)
                run_means.append(sum(r["mean"] * r["repetitions"]
                                     for r in rows) / repetitions)
                pooled.extend(rows)
            center, low, high = median_range(run_means)
            pooled_reps = sum(r["repetitions"] for r in pooled)
            pooled_successes = sum(r["successes"] for r in pooled)
            latency[stimulus, probe] = center
            success[stimulus, probe] = 100 * pooled_successes / pooled_reps
            output.append({
                "platform": platform,
                "color_bit0": PLATFORMS[platform]["matrix_bits"][0],
                "color_bit1": PLATFORMS[platform]["matrix_bits"][1],
                "stimulus_color": format(stimulus, "02b"),
                "probe_color": format(probe, "02b"),
                "n_allocations": len(runs), "passes_per_allocation": 3,
                "median_run_mean_cycles": center,
                "min_run_mean_cycles": low, "max_run_mean_cycles": high,
                "pooled_success_percent": success[stimulus, probe],
                "pooled_successes": pooled_successes,
                "pooled_repetitions": pooled_reps})
    return latency


def save_matrix(latency, platform, output_dir, cmap):
    """Save one title-free matrix for LaTeX-side subfigure layout."""
    fig = plt.figure(figsize=(3.5, 3.0))
    ax = fig.add_axes([0.17, 0.15, 0.70, 0.817])
    ax.imshow(latency, cmap=cmap, vmin=20, vmax=310,
              interpolation="nearest", aspect="equal")
    labels = ["00", "01", "10", "11"]
    ax.set_xticks(range(4), labels=labels)
    ax.set_yticks(range(4), labels=labels)
    bit0, bit1 = PLATFORMS[platform]["matrix_bits"]
    ax.set_xlabel(f"Probe PA[{bit1}:{bit0}]", fontsize=7, labelpad=3)
    ax.set_ylabel(f"Stimulus-set PA[{bit1}:{bit0}]", fontsize=7, labelpad=4)
    ax.tick_params(which="major", length=0, labelsize=6.8, pad=2)
    ax.set_xticks(np.arange(-0.5, 4, 1), minor=True)
    ax.set_yticks(np.arange(-0.5, 4, 1), minor=True)
    ax.grid(which="minor", color="white", linewidth=0.45)
    ax.tick_params(which="minor", bottom=False, left=False)
    for spine in ax.spines.values():
        spine.set_color("#4A4A4A")
        spine.set_linewidth(0.55)
    for stimulus in range(4):
        for probe in range(4):
            text_color = ("white" if latency[stimulus, probe] > 180
                          else PAPER["dark"])
            ax.text(probe, stimulus, f"{latency[stimulus, probe]:.0f}",
                    ha="center", va="center", color=text_color,
                    fontsize=7, fontweight="semibold")
    bit0, bit1 = PLATFORMS[platform]["matrix_bits"]
    stem = f"sf_{platform.lower()}_pa{bit1}_{bit0}_color_matrix"
    return save_figure(fig, output_dir, stem, tight=True)


def main():
    defaults = project_paths(__file__)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rk3588", type=Path, default=defaults["rk3588"])
    parser.add_argument(
        "--rk3568", type=Path,
        default=defaults["rk3568"])
    parser.add_argument(
        "--output", type=Path,
        default=defaults["cross_output"])
    parser.add_argument("--rk3588-output", type=Path,
                        default=defaults["cross_output"])
    parser.add_argument("--rk3568-output", type=Path,
                        default=defaults["cross_output"])
    args = parser.parse_args()
    datasets = {p: collect(path, p) for p, path in
                (("RK3588", args.rk3588), ("RK3568", args.rk3568))}
    configure_matplotlib()
    outputs = []

    # Figure 1: same/cross item sweep.
    item_rows, item_series = [], {}
    x = np.arange(21)
    for platform in ("RK3588", "RK3568"):
        for variant in LOCALITY:
            item_series[(platform, variant)], _ = direct_series(
                datasets[platform]["item"], x, variant, 5, item_rows,
                platform, "stimulus_items")
    fig, ax = plt.subplots(figsize=(7.15, 3.55))
    style_axis(ax)
    for platform in ("RK3588", "RK3568"):
        for variant, spec in LOCALITY.items():
            draw(ax, x, item_series[(platform, variant)], platform, spec)
    ax.set_xlim(-0.4, 20.4); ax.set_ylim(15, 625)
    ax.set_xticks(np.arange(0, 21, 2))
    ax.set_xlabel("Number of stimulus addresses, X")
    ax.set_ylabel("Mean probe latency (cycles)")
    item_legend(ax)
    fig.tight_layout(pad=0.5)
    outputs.extend(save_figure(fig, args.output,
                               "sf_item_sweep_rk3588_rk3568"))
    outputs.append(write_rows(
        args.output / "sf_item_sweep_rk3588_rk3568_data.csv", item_rows))

    # Figure 2: cross-only bit scan on the common PA[0:22] domain.
    bit_rows, bit_series, bit_counts = [], {}, {}
    bit_x = np.arange(23)
    for platform in ("RK3588", "RK3568"):
        platform_counts = []
        for variant in BIT_VARIANTS:
            bit_series[(platform, variant)], counts = pass_series(
                datasets[platform]["bits"], bit_x, variant, 5,
                bit_rows, platform)
            platform_counts.extend(counts)
        bit_counts[platform] = ("n=5" if min(platform_counts) == 5 else
                                f"n={min(platform_counts)}–{max(platform_counts)}")
    fig, ax = plt.subplots(figsize=(7.15, 3.55))
    style_axis(ax)
    for platform in ("RK3588", "RK3568"):
        for variant, spec in BIT_VARIANTS.items():
            draw(ax, bit_x, bit_series[(platform, variant)], platform, spec)
    ax.set_xlim(-0.45, 22.45); ax.set_ylim(15, 330)
    ax.set_xticks(bit_x)
    ax.set_xlabel("Flipped physical-address bit, b")
    ax.set_ylabel("Mean cross-core probe latency (cycles)")
    bit_legend(ax)
    fig.tight_layout(pad=0.5)
    outputs.extend(save_figure(fig, args.output,
                               "sf_single_bit_rk3588_rk3568"))
    outputs.append(write_rows(
        args.output / "sf_single_bit_rk3588_rk3568_data.csv", bit_rows))

    # Figures 3a/3b: separate title-free matrices for LaTeX subfigures.
    matrix_rows = []
    matrices = {p: matrix_summary(datasets[p]["matrix"], p, matrix_rows)
                for p in ("RK3588", "RK3568")}
    cmap = LinearSegmentedColormap.from_list(
        "paper_latency", [PAPER["pale"], "#DFCACA", PAPER["red"]])
    matrix_outputs = {
        "RK3588": args.rk3588_output,
        "RK3568": args.rk3568_output,
    }
    for platform in ("RK3588", "RK3568"):
        output_dir = matrix_outputs[platform]
        outputs.extend(save_matrix(matrices[platform], platform,
                                   output_dir, cmap))
        bit0, bit1 = PLATFORMS[platform]["matrix_bits"]
        outputs.append(write_rows(
            output_dir / f"sf_{platform.lower()}_pa{bit1}_{bit0}_color_matrix_data.csv",
            [row for row in matrix_rows if row["platform"] == platform]))
    print_outputs(outputs)


if __name__ == "__main__":
    main()
