#!/usr/bin/env python3
"""Summarize scan_order=6 cache/SF attribution CSV output."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


def rate(rows: list[dict[str, str]], field: str = "successes") -> float:
    total = sum(int(row[field]) for row in rows)
    repetitions = sum(int(row["repetitions"]) for row in rows)
    return 100.0 * total / repetitions if repetitions else float("nan")


def sample_rate(
    rows: list[dict[str, str]], field: str, available: bool = True
) -> float:
    if not available or not rows or field not in rows[0]:
        return float("nan")
    refill_samples = sum(int(row[field]) for row in rows)
    repetitions = sum(int(row["repetitions"]) for row in rows)
    return 100.0 * refill_samples / repetitions if repetitions else float("nan")


def weighted_median_ticks(rows: list[dict[str, str]]) -> float:
    repetitions = sum(int(row["repetitions"]) for row in rows)
    if not repetitions:
        return float("nan")
    return sum(
        float(row["median_ticks"]) * int(row["repetitions"]) for row in rows
    ) / repetitions


def weighted_value(rows: list[dict[str, str]], field: str) -> float:
    repetitions = sum(int(row["repetitions"]) for row in rows)
    if not repetitions:
        return float("nan")
    return sum(float(row[field]) * int(row["repetitions"]) for row in rows) / repetitions


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path", type=Path)
    args = parser.parse_args()
    lines = args.csv_path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    ll_events_available = metadata.get("ll_cache_events_available") == "true"

    print(
        "[pmu] ll_cache_events_available="
        f"{metadata.get('ll_cache_events_available', 'not_recorded')}"
    )

    print("[item_sweep]")
    print(
        "fill_rounds,items,same_baseline_pct,same_candidate_pct,"
        "cross_baseline_pct,cross_candidate_pct,same_candidate_median_ticks,"
        "cross_candidate_median_ticks,same_mean,same_p90,cross_mean,cross_p90,"
        "same_l1_refill_pct,same_l2_refill_pct,cross_l1_refill_pct,"
        "cross_l2_refill_pct,same_ll_read_pct,same_ll_miss_pct,"
        "cross_ll_read_pct,cross_ll_miss_pct,"
        "same_bin0_pct,same_bin1_pct,same_bin2_pct,same_bin3_pct,"
        "cross_bin0_pct,cross_bin1_pct,cross_bin2_pct,cross_bin3_pct"
    )
    sweeps: dict[tuple[int, int], dict[str, list[dict[str, str]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for row in rows:
        if row["record"] == "item_sweep":
            key = (int(row["fill_rounds"]), int(row["test_bit"]))
            sweeps[key][row["variant"]].append(row)
    for fill_rounds, items in sorted(sweeps):
        values = sweeps[(fill_rounds, items)]
        print(
            f"{fill_rounds},{items},"
            f"{rate(values['same_baseline']):.3f},"
            f"{rate(values['same_candidate']):.3f},"
            f"{rate(values['cross_baseline']):.3f},"
            f"{rate(values['cross_candidate']):.3f},"
            f"{weighted_median_ticks(values['same_candidate']):.3f},"
            f"{weighted_median_ticks(values['cross_candidate']):.3f},"
            f"{weighted_value(values['same_candidate'], 'mean_ticks'):.3f},"
            f"{weighted_value(values['same_candidate'], 'p90_ticks'):.3f},"
            f"{weighted_value(values['cross_candidate'], 'mean_ticks'):.3f},"
            f"{weighted_value(values['cross_candidate'], 'p90_ticks'):.3f},"
            f"{sample_rate(values['same_candidate'], 'probe_l1d_refill_samples'):.3f},"
            f"{sample_rate(values['same_candidate'], 'probe_l2d_refill_samples'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'probe_l1d_refill_samples'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'probe_l2d_refill_samples'):.3f},"
            f"{sample_rate(values['same_candidate'], 'probe_ll_cache_rd_samples', ll_events_available):.3f},"
            f"{sample_rate(values['same_candidate'], 'probe_ll_cache_miss_samples', ll_events_available):.3f},"
            f"{sample_rate(values['cross_candidate'], 'probe_ll_cache_rd_samples', ll_events_available):.3f},"
            f"{sample_rate(values['cross_candidate'], 'probe_ll_cache_miss_samples', ll_events_available):.3f},"
            f"{sample_rate(values['same_candidate'], 'latency_lt_bin1'):.3f},"
            f"{sample_rate(values['same_candidate'], 'latency_bin1_bin2'):.3f},"
            f"{sample_rate(values['same_candidate'], 'latency_bin2_bin3'):.3f},"
            f"{sample_rate(values['same_candidate'], 'latency_ge_bin3'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'latency_lt_bin1'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'latency_bin1_bin2'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'latency_bin2_bin3'):.3f},"
            f"{sample_rate(values['cross_candidate'], 'latency_ge_bin3'):.3f}"
        )

    print("\n[locality_bit_scan]")
    print(
        "bit,cross_original,cross_probe_only,cross_joint,"
        "same_original,same_probe_only,same_joint,classification"
    )
    cross: dict[int, dict[str, list[dict[str, str]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    same: dict[int, dict[str, list[dict[str, str]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    classifications: dict[int, list[str]] = defaultdict(list)
    unavailable: dict[int, str] = {}
    for row in rows:
        bit = int(row["test_bit"]) if row.get("test_bit") else 0
        if row["record"] == "joint_bit_test":
            if row["available"] != "1":
                unavailable[bit] = row["lookup_status"]
            else:
                cross[bit][row["variant"]].append(row)
        elif row["record"] == "locality_bit_test":
            same[bit][row["variant"]].append(row)
        elif row["record"] == "locality_bit_classification":
            classifications[bit].append(row["variant"])

    all_bits = sorted(set(cross) | set(same) | set(unavailable))
    for bit in all_bits:
        if bit in unavailable and bit not in cross:
            print(f"{bit},,,,,,,unavailable:{unavailable[bit]}")
            continue
        cv = cross[bit]
        sv = same[bit]
        pass_classes = classifications.get(bit, [])
        classification = (
            pass_classes[0]
            if pass_classes and len(set(pass_classes)) == 1
            else "cross_pass_inconsistent:" + "|".join(pass_classes)
        )
        print(
            f"{bit},"
            f"{rate(cv['original_matching']):.3f},"
            f"{rate(cv['probe_only_flipped']):.3f},"
            f"{rate(cv['joint_group_flipped']):.3f},"
            f"{rate(sv['same_original_matching']):.3f},"
            f"{rate(sv['same_probe_only_flipped']):.3f},"
            f"{rate(sv['same_joint_group_flipped']):.3f},"
            f"{classification}"
        )


if __name__ == "__main__":
    main()
