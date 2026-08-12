#!/usr/bin/env python3
"""Validate one or more v16 SF motivation campaign runs before analysis."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def read_result(path: Path):
    metadata = {}
    lines = path.read_text(encoding="utf-8").splitlines()
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def validate_attribution(path: Path):
    metadata, rows = read_result(path)
    errors = []
    expected_bits = set(range(25))
    expected_items = set(range(21))
    if metadata.get("schema_version") != "16":
        errors.append("schema_version is not 16")
    if metadata.get("experiment") != "owned_same_cross_sf_attribution":
        errors.append("wrong experiment type")
    if metadata.get("item_sweep_first") != "0" or metadata.get("item_sweep_last") != "20":
        errors.append("item sweep is not 0..20")
    if metadata.get("item_sweep_order") != "descending_from_item_sweep_last_to_item_sweep_first":
        errors.append("item sweep is not descending")
    if metadata.get("bit_first") != "0" or metadata.get("bit_last") != "24":
        errors.append("bit scan is not 0..24")
    if metadata.get("attribution_cross_only_bits") != "true":
        errors.append("per-bit scan is not cross-only")
    if metadata.get("joint_rescue_mask", "").lower() != "0x30000":
        errors.append("joint rescue mask is not PA[16]/PA[17]")

    item_rows = [row for row in rows if row.get("record") == "item_sweep"
                 and row.get("fill_rounds") == "1000"]
    variants = {"same_baseline", "same_candidate", "cross_baseline", "cross_candidate"}
    for variant in variants:
        present = {int(row["test_bit"]) for row in item_rows if row["variant"] == variant}
        if present != expected_items:
            errors.append(f"item_sweep {variant} missing={sorted(expected_items - present)} extra={sorted(present - expected_items)}")

    cross_rows = [row for row in rows if row.get("record") == "cross_bit_test"
                  and row.get("available") == "1"]
    for variant in ("original_matching", "probe_only_flipped"):
        counts = {bit: 0 for bit in expected_bits}
        for row in cross_rows:
            if row.get("variant") == variant:
                counts[int(row["test_bit"])] += 1
        bad = {bit: count for bit, count in counts.items() if count != 3}
        if bad:
            errors.append(f"cross_bit_test {variant} expected 3 passes: {bad}")

    if any(row.get("record") == "locality_bit_test" for row in rows):
        errors.append("unexpected same-core per-bit locality rows")

    joint_rows = [row for row in rows if row.get("record") == "joint_bit_test"
                  and row.get("variant") == "joint_group_flipped"
                  and row.get("available") == "1"]
    joint_counts = {bit: 0 for bit in (16, 17)}
    for row in joint_rows:
        bit = int(row["test_bit"])
        if bit in joint_counts:
            joint_counts[bit] += 1
    if any(count != 3 for count in joint_counts.values()):
        errors.append(f"joint rescue expected 3 passes for PA[16]/PA[17]: {joint_counts}")
    return metadata, errors


def validate_matrix(path: Path):
    metadata, rows = read_result(path)
    errors = []
    if metadata.get("schema_version") != "16":
        errors.append("schema_version is not 16")
    if metadata.get("experiment") != "owned_address_color_matrix":
        errors.append("wrong experiment type")
    cells = {(int(row["test_bit"]), int(row["test_bit2"]), int(row["scan_pass"]))
             for row in rows if row.get("record") == "color_matrix"
             and row.get("available") == "1"}
    expected = {(stimulus, probe, scan_pass)
                for stimulus in range(4) for probe in range(4)
                for scan_pass in range(1, 4)}
    if cells != expected:
        errors.append(f"matrix cells missing={len(expected - cells)} extra={len(cells - expected)}")
    return metadata, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--expected-runs", type=int, default=5)
    args = parser.parse_args()
    attribution = sorted(args.directory.glob("*_attribution.csv"))
    matrices = sorted(args.directory.glob("*_matrix.csv"))
    failed = False
    if len(attribution) != args.expected_runs or len(matrices) != args.expected_runs:
        print(f"ERROR: expected {args.expected_runs} attribution and matrix files; got {len(attribution)} and {len(matrices)}")
        failed = True
    for path in attribution:
        metadata, errors = validate_attribution(path)
        print(f"{path.name}: {'OK' if not errors else 'FAILED'} run={metadata.get('experiment_run_id', '')}")
        for error in errors:
            print(f"  - {error}")
        failed |= bool(errors)
    for path in matrices:
        metadata, errors = validate_matrix(path)
        print(f"{path.name}: {'OK' if not errors else 'FAILED'} run={metadata.get('experiment_run_id', '')}")
        for error in errors:
            print(f"  - {error}")
        failed |= bool(errors)
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
