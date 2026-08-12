#!/usr/bin/env python3

import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path

from analyze_sf_evidence import NAME_RE, paired_records, summarize


CONDITION_RE = re.compile(
    r"^(?P<phase>map|occupancy|access)_fb(?P<filler_bits>\d+)"
    r"_fi(?P<filler_items>\d+)_t(?P<target>[0-9a-fA-F]+)$"
)


def main():
    parser = argparse.ArgumentParser(
        description="Aggregate deterministic SF filler experiments"
    )
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()
    output = args.out or args.result_dir / "sf_mechanism_summary.csv"

    groups = defaultdict(list)
    targets = defaultdict(set)
    errors = []
    for path in sorted(args.result_dir.glob("sf_*.csv")):
        name = NAME_RE.match(path.name)
        if not name:
            continue
        condition = CONDITION_RE.match(name.group("condition"))
        if not condition:
            continue
        try:
            _metadata, records = paired_records(path)
        except ValueError as error:
            errors.append(str(error))
            continue
        key = (
            condition.group("phase"),
            int(condition.group("filler_bits")),
            int(condition.group("filler_items")),
            int(name.group("access_items")),
        )
        groups[key].extend(records)
        targets[key].add(condition.group("target").lower())

    fields = [
        "phase",
        "filler_match_bits",
        "filler_items",
        "access_items",
        "target_count",
        "samples",
        "probe_mismatches",
        "cross_wait_tick_delta_mean",
        "l2_miss_rate_baseline",
        "l2_miss_rate_candidate",
        "l2_candidate_only",
        "l2_baseline_only",
        "l2_mcnemar_p",
        "ll_access_rate_candidate",
        "ll_miss_rate_candidate",
        "l2_refill_ll_hit_rate_candidate",
        "stimulus_l2_inval_rate_baseline",
        "stimulus_l2_inval_rate_candidate",
        "tick_delta_mean",
        "tick_delta_median",
    ]
    with output.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fields)
        writer.writeheader()
        for key, records in sorted(groups.items()):
            phase, filler_bits, filler_items, access_items = key
            summary = summarize(records)
            row = {
                "phase": phase,
                "filler_match_bits": filler_bits,
                "filler_items": filler_items,
                "access_items": access_items,
                "target_count": len(targets[key]),
                **{field: summary[field] for field in fields if field in summary},
            }
            writer.writerow(row)
            print(
                f"{phase:9s} fb={filler_bits:2d} fi={filler_items:2d} "
                f"ai={access_items:2d} targets={len(targets[key])} "
                f"n={summary['samples']:5d} "
                f"L2={summary['l2_miss_rate_baseline']:.3f}/"
                f"{summary['l2_miss_rate_candidate']:.3f} "
                f"LLmiss(c)={summary['ll_miss_rate_candidate']:.3f} "
                f"L2refill+LLhit(c)="
                f"{summary['l2_refill_ll_hit_rate_candidate']:.3f} "
                f"L2inval={summary['stimulus_l2_inval_rate_baseline']:.3f}/"
                f"{summary['stimulus_l2_inval_rate_candidate']:.3f}"
            )

    print(f"Wrote {output}")
    if errors:
        print("Skipped files:")
        for error in errors:
            print(f"  {error}")


if __name__ == "__main__":
    main()
