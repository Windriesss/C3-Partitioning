#!/usr/bin/env python3
"""Compact run-level audit for a received SF motivation v16 bundle."""

from __future__ import annotations

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def read_csv(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def stats(values):
    values = list(values)
    if not values:
        return None
    return {
        "n_runs": len(values),
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def main():
    root = Path(sys.argv[1])
    attr_files = sorted(root.glob("*_attribution.csv"))
    matrix_files = sorted(root.glob("*_matrix.csv"))

    item = defaultdict(list)
    bits = defaultdict(list)
    joint = defaultdict(list)
    matrix = defaultdict(list)
    unavailable = []

    for path in attr_files:
        metadata, rows = read_csv(path)
        run = metadata["experiment_run_id"].split("_cpu", 1)[0]
        for row in rows:
            record = row["record"]
            available = row["available"] == "1"
            if record == "item_sweep" and available:
                item[(int(row["test_bit"]), row["variant"])].append(
                    (run, float(row["mean_ticks"]), int(row["successes"]),
                     int(row["repetitions"]), int(row["probe_ll_cache_miss_samples"])))
            elif record == "cross_bit_test":
                bit = int(row["test_bit"])
                if available:
                    bits[(bit, row["variant"])].append(
                        (run, int(row["scan_pass"]), float(row["mean_ticks"]),
                         int(row["successes"]), int(row["repetitions"])))
                elif row["variant"] == "probe_only_flipped":
                    unavailable.append({"run": run, "bit": bit, "status": row["lookup_status"]})
            elif record == "joint_bit_test" and available:
                joint[(int(row["test_bit"]), row["variant"])].append(
                    (run, int(row["scan_pass"]), float(row["mean_ticks"]),
                     int(row["successes"]), int(row["repetitions"])))

    for path in matrix_files:
        metadata, rows = read_csv(path)
        run = metadata["experiment_run_id"].split("_cpu", 1)[0]
        for row in rows:
            if row["record"] == "color_matrix" and row["available"] == "1":
                matrix[(int(row["test_bit"]), int(row["test_bit2"]))].append(
                    (run, int(row["scan_pass"]), float(row["mean_ticks"]),
                     int(row["successes"]), int(row["repetitions"])))

    item_summary = {}
    for n in range(21):
        item_summary[n] = {}
        for variant in ("same_baseline", "same_candidate", "cross_baseline", "cross_candidate"):
            rows = item[(n, variant)]
            item_summary[n][variant] = {
                "mean_ticks": stats(r[1] for r in rows),
                "success_pct": stats(100 * r[2] / r[3] for r in rows),
                "ll_miss_incidence_pct": stats(100 * r[4] / r[3] for r in rows),
            }

    bit_summary = {}
    for bit in range(25):
        bit_summary[bit] = {}
        for variant in ("original_matching", "probe_only_flipped"):
            rows = bits[(bit, variant)]
            by_run = defaultdict(list)
            for run, _, mean, successes, repetitions in rows:
                by_run[run].append((mean, successes, repetitions))
            means = [statistics.mean(v[0] for v in vals) for vals in by_run.values()]
            rates = [100 * sum(v[1] for v in vals) / sum(v[2] for v in vals)
                     for vals in by_run.values()]
            bit_summary[bit][variant] = {
                "mean_ticks": stats(means),
                "success_pct": stats(rates),
            }

    joint_summary = {}
    for bit in (16, 17):
        rows = joint[(bit, "joint_group_flipped")]
        by_run = defaultdict(list)
        for run, _, mean, successes, repetitions in rows:
            by_run[run].append((mean, successes, repetitions))
        joint_summary[bit] = {
            "mean_ticks": stats(statistics.mean(v[0] for v in vals) for vals in by_run.values()),
            "success_pct": stats(100 * sum(v[1] for v in vals) / sum(v[2] for v in vals)
                                 for vals in by_run.values()),
        }

    diagonal_rows = [entry for (src, dst), entries in matrix.items() if src == dst for entry in entries]
    off_rows = [entry for (src, dst), entries in matrix.items() if src != dst for entry in entries]

    def matrix_run_stats(rows):
        by_run = defaultdict(list)
        for run, _, mean, successes, repetitions in rows:
            by_run[run].append((mean, successes, repetitions))
        return {
            "mean_ticks": stats(statistics.mean(v[0] for v in vals) for vals in by_run.values()),
            "success_pct": stats(100 * sum(v[1] for v in vals) / sum(v[2] for v in vals)
                                 for vals in by_run.values()),
            "pooled_successes": sum(v[1] for vals in by_run.values() for v in vals),
            "pooled_repetitions": sum(v[2] for vals in by_run.values() for v in vals),
        }

    output = {
        "attribution_files": len(attr_files),
        "matrix_files": len(matrix_files),
        "unavailable": unavailable,
        "item": item_summary,
        "bits": bit_summary,
        "joint": joint_summary,
        "matrix_diagonal": matrix_run_stats(diagonal_rows),
        "matrix_off_diagonal": matrix_run_stats(off_rows),
    }
    if len(sys.argv) > 2 and sys.argv[2] == "--compact":
        print("ITEM n same_mean[min,max] cross_mean[min,max] cross_success[min,max] cross_llmiss[min,max]")
        for n in range(21):
            same = item_summary[n]["same_candidate"]["mean_ticks"]
            cross = item_summary[n]["cross_candidate"]["mean_ticks"]
            success = item_summary[n]["cross_candidate"]["success_pct"]
            miss = item_summary[n]["cross_candidate"]["ll_miss_incidence_pct"]
            print(f"{n:2d} {same['median']:.1f}[{same['min']:.1f},{same['max']:.1f}] "
                  f"{cross['median']:.1f}[{cross['min']:.1f},{cross['max']:.1f}] "
                  f"{success['median']:.2f}[{success['min']:.2f},{success['max']:.2f}] "
                  f"{miss['median']:.2f}[{miss['min']:.2f},{miss['max']:.2f}]")
        print("BIT b n original_mean[min,max] flipped_mean[min,max] flipped_success[min,max]")
        for bit in range(25):
            original = bit_summary[bit]["original_matching"]["mean_ticks"]
            flipped = bit_summary[bit]["probe_only_flipped"]["mean_ticks"]
            success = bit_summary[bit]["probe_only_flipped"]["success_pct"]
            print(f"{bit:2d} {flipped['n_runs']} {original['median']:.1f}[{original['min']:.1f},{original['max']:.1f}] "
                  f"{flipped['median']:.1f}[{flipped['min']:.1f},{flipped['max']:.1f}] "
                  f"{success['median']:.2f}[{success['min']:.2f},{success['max']:.2f}]")
        print("JOINT", json.dumps(joint_summary, separators=(",", ":")))
        print("MATRIX_DIAGONAL", json.dumps(output["matrix_diagonal"], separators=(",", ":")))
        print("MATRIX_OFF_DIAGONAL", json.dumps(output["matrix_off_diagonal"], separators=(",", ":")))
        print("UNAVAILABLE", json.dumps(unavailable, separators=(",", ":")))
    else:
        print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
