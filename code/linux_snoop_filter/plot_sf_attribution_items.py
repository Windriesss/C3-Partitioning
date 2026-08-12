#!/usr/bin/env python3
"""Plot v14 latency quantiles, deep-latency bins, and optional LL misses."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def percentage(row: dict[str, str], field: str) -> float:
    repetitions = int(row["repetitions"])
    return 100.0 * int(row.get(field, 0)) / repetitions if repetitions else 0.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--fill-rounds", type=int, default=1000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.csv_path.with_suffix(".item_latency.png")

    lines = args.csv_path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = [
        row for row in csv.DictReader(line for line in lines if not line.startswith("#"))
        if row["record"] == "item_sweep"
        and int(row["fill_rounds"]) == args.fill_rounds
    ]
    variants = {
        variant: sorted(
            (row for row in rows if row["variant"] == variant),
            key=lambda row: int(row["test_bit"]),
        )
        for variant in ("same_candidate", "cross_candidate")
    }

    fig, axes = plt.subplots(1, 3, figsize=(16, 4.8), sharex=True)
    colors = {"mean_ticks": "#4C78A8", "median_ticks": "#F58518", "p90_ticks": "#54A24B"}
    for axis, variant, title in (
        (axes[0], "same_candidate", "Same-core latency distribution summary"),
        (axes[1], "cross_candidate", "Cross-core latency distribution summary"),
    ):
        points = variants[variant]
        items = [int(row["test_bit"]) for row in points]
        for field, label in (("mean_ticks", "mean"), ("median_ticks", "median"), ("p90_ticks", "p90")):
            axis.plot(items, [float(row[field]) for row in points], marker="o",
                      markersize=3, label=label, color=colors[field])
        axis.set_title(title)
        axis.set_ylabel("ticks")
        axis.set_xlabel("stimulus items (probe excluded)")
        axis.grid(True, alpha=0.25)
        axis.legend()

    same = variants["same_candidate"]
    cross = variants["cross_candidate"]
    items = [int(row["test_bit"]) for row in same]
    axes[2].plot(items, [percentage(row, "latency_ge_bin3") for row in same],
                 marker="o", label="same highest latency bin")
    axes[2].plot(items, [percentage(row, "latency_ge_bin3") for row in cross],
                 marker="o", label="cross highest latency bin")
    if metadata.get("ll_cache_events_available") == "true":
        axes[2].plot(items, [percentage(row, "probe_ll_cache_miss_samples") for row in same],
                     linestyle="--", label="same LL miss")
        axes[2].plot(items, [percentage(row, "probe_ll_cache_miss_samples") for row in cross],
                     linestyle="--", label="cross LL miss")
    else:
        axes[2].text(0.02, 0.96, "LL PMU event unavailable", transform=axes[2].transAxes,
                     ha="left", va="top", fontsize=9)
    axes[2].set_title("Deep-latency bin and LL-cache miss")
    axes[2].set_ylabel("trials (%)")
    axes[2].set_xlabel("stimulus items (probe excluded)")
    axes[2].set_ylim(-2, 102)
    axes[2].grid(True, alpha=0.25)
    axes[2].legend(fontsize=8)

    fig.suptitle(f"RK3588 attribution item sweep, refill rounds={args.fill_rounds}")
    fig.tight_layout()
    fig.savefig(output, dpi=180)
    print(output)


if __name__ == "__main__":
    main()
