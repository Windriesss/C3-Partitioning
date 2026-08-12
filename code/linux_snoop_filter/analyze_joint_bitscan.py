#!/usr/bin/env python3
"""Summarize scan_order=5 probe-only versus joint candidate/probe flips."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


VARIANTS = (
    "original_matching",
    "probe_only_flipped",
    "joint_group_flipped",
)


def percentage(rows: list[dict[str, str]]) -> float:
    successes = sum(int(row["successes"]) for row in rows)
    repetitions = sum(int(row["repetitions"]) for row in rows)
    return 100.0 * successes / repetitions if repetitions else float("nan")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--stable-percent", type=float, default=95.0)
    parser.add_argument("--probe-max-percent", type=float, default=20.0)
    args = parser.parse_args()

    with args.csv_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(line for line in stream if not line.startswith("#")))

    tests: dict[int, dict[str, list[dict[str, str]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    unavailable: dict[int, dict[str, str]] = {}
    for row in rows:
        if row["record"] != "joint_bit_test":
            continue
        bit = int(row["test_bit"])
        if row["available"] != "1":
            unavailable[bit] = row
        elif row["variant"] in VARIANTS:
            tests[bit][row["variant"]].append(row)

    print(
        "bit,availability,passes,original_percent,probe_only_percent,"
        "joint_percent,classification"
    )
    for bit in sorted(set(tests) | set(unavailable)):
        variants = tests.get(bit, {})
        if not all(variants.get(name) for name in VARIANTS):
            row = unavailable.get(bit, {})
            status = row.get("lookup_status", "incomplete_measurement")
            missing = row.get("test_bit2", "")
            print(f"{bit},{status},0,,,,missing_member_{missing}")
            continue

        original = percentage(variants["original_matching"])
        probe_only = percentage(variants["probe_only_flipped"])
        joint = percentage(variants["joint_group_flipped"])
        passes = len({row["scan_pass"] for row in variants["original_matching"]})
        per_pass_classes = []
        pass_ids = sorted({row["scan_pass"] for row in variants["original_matching"]})
        for pass_id in pass_ids:
            values = {
                name: percentage(
                    [row for row in variants[name] if row["scan_pass"] == pass_id]
                )
                for name in VARIANTS
            }
            if values["original_matching"] < args.stable_percent:
                per_pass_classes.append("invalid_original_drift")
            elif values["probe_only_flipped"] > args.probe_max_percent:
                per_pass_classes.append("probe_only_not_isolated")
            elif values["joint_group_flipped"] >= args.stable_percent:
                per_pass_classes.append("selector_rescue")
            else:
                per_pass_classes.append("joint_not_restored_inconclusive")

        classification = (
            per_pass_classes[0]
            if len(set(per_pass_classes)) == 1
            else "cross_pass_inconsistent:" + "|".join(per_pass_classes)
        )
        print(
            f"{bit},owned_exact,{passes},{original:.3f},{probe_only:.3f},"
            f"{joint:.3f},{classification}"
        )


if __name__ == "__main__":
    main()
