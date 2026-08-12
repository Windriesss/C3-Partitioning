#!/usr/bin/env python3

import argparse
import csv
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path


NAME_RE = re.compile(
    r"^sf_(?P<condition>.+)_mb(?P<match_bits>\d+)_ai"
    r"(?P<access_items>\d+)(?:_wu(?P<prefill_wait_us>\d+))?"
    r"_r(?P<repetition>\d+)\.csv$"
)


def binomial_two_sided(successes: int, trials: int) -> float:
    if trials == 0:
        return 1.0
    tail = sum(
        math.comb(trials, value)
        for value in range(0, min(successes, trials - successes) + 1)
    )
    return min(1.0, 2.0 * tail / (2**trials))


def load_cross_rows(path: Path):
    metadata = {}
    data_lines = []
    with path.open("r", encoding="utf-8") as csv_file:
        for line in csv_file:
            if line.startswith("#"):
                key, _, value = line[1:].strip().partition(",")
                metadata[key] = value
            else:
                data_lines.append(line)

    reader = csv.DictReader(data_lines)
    required = {
        "type",
        "index",
        "probe_ticks",
        "l2d_refill",
        "probe_pa",
    }
    if reader.fieldnames is None or not required.issubset(reader.fieldnames):
        raise ValueError(f"{path}: missing PMU or probe columns")

    rows = {
        mode: {"baseline": {}, "candidate": {}}
        for mode in (
            "same",
            "cross_idle",
            "cross",
            "same_wait_prefill",
            "cross_wait_prefill",
        )
    }
    for row in reader:
        kind = row["type"]
        matched = re.match(
            r"^(same_wait_prefill|cross_wait_prefill|"
            r"same|cross_idle|cross)_(baseline|candidate)$",
            kind,
        )
        if not matched:
            continue
        mode, group = matched.groups()
        rows[mode][group][int(row["index"])] = {
            "ticks": int(row["probe_ticks"]),
            "l1": int(row.get("l1d_refill", 0)),
            "l2": int(row["l2d_refill"]),
            "bus": int(row.get("bus_access", 0)),
            "ll": (
                int(row["ll_cache_rd"])
                if "ll_cache_rd" in row
                else -1
            ),
            "ll_miss": (
                int(row["ll_cache_miss_rd"])
                if "ll_cache_miss_rd" in row
                else -1
            ),
            "stimulus_l2_inval": int(
                row["stimulus_l2d_inval"]
                if "stimulus_l2d_inval" in row
                else -1
            ),
            "probe": int(row["probe_pa"], 16),
        }

    if metadata.get("probe_pmu_events_available") != "true":
        raise ValueError(f"{path}: probe PMU events unavailable")
    return metadata, rows


def paired_records(path: Path):
    metadata, rows = load_cross_rows(path)
    indexes = sorted(
        set(rows["cross"]["baseline"]) & set(rows["cross"]["candidate"])
    )
    if not indexes:
        raise ValueError(f"{path}: no paired cross samples")

    records = []
    for index in indexes:
        baseline = rows["cross"]["baseline"][index]
        candidate = rows["cross"]["candidate"][index]
        idle_baseline = rows["cross_idle"]["baseline"][index]
        idle_candidate = rows["cross_idle"]["candidate"][index]
        same_baseline = rows["same"]["baseline"][index]
        same_candidate = rows["same"]["candidate"][index]
        same_wait_baseline = rows["same_wait_prefill"]["baseline"].get(index)
        same_wait_candidate = rows["same_wait_prefill"]["candidate"].get(index)
        cross_wait_baseline = rows["cross_wait_prefill"]["baseline"].get(index)
        cross_wait_candidate = rows["cross_wait_prefill"]["candidate"].get(index)
        records.append(
            {
                "probe_match": baseline["probe"] == candidate["probe"],
                "tick_delta": candidate["ticks"] - baseline["ticks"],
                "l1_base": baseline["l1"],
                "l1_cand": candidate["l1"],
                "l2_base": baseline["l2"],
                "l2_cand": candidate["l2"],
                "bus_base": baseline["bus"],
                "bus_cand": candidate["bus"],
                "ll_base": baseline["ll"],
                "ll_cand": candidate["ll"],
                "ll_miss_base": baseline["ll_miss"],
                "ll_miss_cand": candidate["ll_miss"],
                "stimulus_l2_inval_base": baseline[
                    "stimulus_l2_inval"
                ],
                "stimulus_l2_inval_cand": candidate[
                    "stimulus_l2_inval"
                ],
                "idle_l2_base": idle_baseline["l2"],
                "idle_l2_cand": idle_candidate["l2"],
                "same_l2_base": same_baseline["l2"],
                "same_l2_cand": same_candidate["l2"],
                "same_wait_tick_delta": (
                    same_wait_candidate["ticks"] - same_wait_baseline["ticks"]
                    if same_wait_baseline and same_wait_candidate
                    else 0
                ),
                "cross_wait_tick_delta": (
                    cross_wait_candidate["ticks"]
                    - cross_wait_baseline["ticks"]
                    if cross_wait_baseline and cross_wait_candidate
                    else 0
                ),
            }
        )
    return metadata, records


def summarize(records):
    count = len(records)
    cand_only_l2 = sum(
        row["l2_cand"] > 0 and row["l2_base"] == 0 for row in records
    )
    base_only_l2 = sum(
        row["l2_base"] > 0 and row["l2_cand"] == 0 for row in records
    )
    discordant = cand_only_l2 + base_only_l2

    def optional_rate(field, predicate):
        values = [row[field] for row in records if row[field] >= 0]
        if not values:
            return math.nan
        return statistics.fmean(predicate(value) for value in values)

    return {
        "samples": count,
        "probe_mismatches": sum(not row["probe_match"] for row in records),
        "tick_delta_mean": statistics.fmean(
            row["tick_delta"] for row in records
        ),
        "tick_delta_median": statistics.median(
            row["tick_delta"] for row in records
        ),
        "l1_miss_rate_baseline": statistics.fmean(
            row["l1_base"] > 0 for row in records
        ),
        "l1_miss_rate_candidate": statistics.fmean(
            row["l1_cand"] > 0 for row in records
        ),
        "l2_miss_rate_baseline": statistics.fmean(
            row["l2_base"] > 0 for row in records
        ),
        "l2_miss_rate_candidate": statistics.fmean(
            row["l2_cand"] > 0 for row in records
        ),
        "idle_l2_miss_rate_baseline": statistics.fmean(
            row["idle_l2_base"] > 0 for row in records
        ),
        "idle_l2_miss_rate_candidate": statistics.fmean(
            row["idle_l2_cand"] > 0 for row in records
        ),
        "same_l2_miss_rate_baseline": statistics.fmean(
            row["same_l2_base"] > 0 for row in records
        ),
        "same_l2_miss_rate_candidate": statistics.fmean(
            row["same_l2_cand"] > 0 for row in records
        ),
        "same_wait_tick_delta_mean": statistics.fmean(
            row["same_wait_tick_delta"] for row in records
        ),
        "same_wait_tick_delta_median": statistics.median(
            row["same_wait_tick_delta"] for row in records
        ),
        "cross_wait_tick_delta_mean": statistics.fmean(
            row["cross_wait_tick_delta"] for row in records
        ),
        "cross_wait_tick_delta_median": statistics.median(
            row["cross_wait_tick_delta"] for row in records
        ),
        "l2_candidate_only": cand_only_l2,
        "l2_baseline_only": base_only_l2,
        "l2_mcnemar_p": binomial_two_sided(cand_only_l2, discordant),
        "bus_mean_baseline": statistics.fmean(
            row["bus_base"] for row in records
        ),
        "bus_mean_candidate": statistics.fmean(
            row["bus_cand"] for row in records
        ),
        "ll_access_rate_baseline": optional_rate(
            "ll_base", lambda value: value > 0
        ),
        "ll_access_rate_candidate": optional_rate(
            "ll_cand", lambda value: value > 0
        ),
        "ll_miss_rate_baseline": optional_rate(
            "ll_miss_base", lambda value: value > 0
        ),
        "ll_miss_rate_candidate": optional_rate(
            "ll_miss_cand", lambda value: value > 0
        ),
        "l2_refill_ll_hit_rate_candidate": (
            statistics.fmean(
                row["l2_cand"] > 0 and row["ll_miss_cand"] == 0
                for row in records
            )
            if all(row["ll_miss_cand"] >= 0 for row in records)
            else math.nan
        ),
        "stimulus_l2_inval_rate_baseline": optional_rate(
            "stimulus_l2_inval_base", lambda value: value > 0
        ),
        "stimulus_l2_inval_rate_candidate": optional_rate(
            "stimulus_l2_inval_cand", lambda value: value > 0
        ),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Summarize paired private-cache misses from SF experiments"
    )
    parser.add_argument("result_dir", type=Path)
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Summary CSV (default: RESULT_DIR/sf_summary.csv)",
    )
    args = parser.parse_args()
    output = args.out or args.result_dir / "sf_summary.csv"

    groups = defaultdict(list)
    errors = []
    for path in sorted(args.result_dir.glob("sf_*.csv")):
        match = NAME_RE.match(path.name)
        if not match:
            continue
        try:
            _metadata, records = paired_records(path)
        except ValueError as error:
            errors.append(str(error))
            continue
        key = (
            match.group("condition"),
            int(match.group("match_bits")),
            int(match.group("access_items")),
            int(
                match.group("prefill_wait_us")
                or _metadata.get("prefill_wait_us", "0")
            ),
        )
        groups[key].extend(records)

    fields = [
        "condition",
        "match_bits",
        "access_items",
        "prefill_wait_us",
        "samples",
        "probe_mismatches",
        "tick_delta_mean",
        "tick_delta_median",
        "l1_miss_rate_baseline",
        "l1_miss_rate_candidate",
        "l2_miss_rate_baseline",
        "l2_miss_rate_candidate",
        "idle_l2_miss_rate_baseline",
        "idle_l2_miss_rate_candidate",
        "same_l2_miss_rate_baseline",
        "same_l2_miss_rate_candidate",
        "same_wait_tick_delta_mean",
        "same_wait_tick_delta_median",
        "cross_wait_tick_delta_mean",
        "cross_wait_tick_delta_median",
        "l2_candidate_only",
        "l2_baseline_only",
        "l2_mcnemar_p",
        "bus_mean_baseline",
        "bus_mean_candidate",
        "ll_access_rate_baseline",
        "ll_access_rate_candidate",
        "ll_miss_rate_baseline",
        "ll_miss_rate_candidate",
        "l2_refill_ll_hit_rate_candidate",
        "stimulus_l2_inval_rate_baseline",
        "stimulus_l2_inval_rate_candidate",
    ]
    with output.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fields)
        writer.writeheader()
        for (
            condition,
            match_bits,
            access_items,
            prefill_wait_us,
        ), records in sorted(
            groups.items()
        ):
            summary = summarize(records)
            row = {
                "condition": condition,
                "match_bits": match_bits,
                "access_items": access_items,
                "prefill_wait_us": prefill_wait_us,
                **summary,
            }
            writer.writerow(row)
            print(
                f"{condition:12s} low{match_bits} n={access_items:2d} "
                f"wait={prefill_wait_us:4d}us "
                f"L2 miss base={summary['l2_miss_rate_baseline']:.3f} "
                f"cand={summary['l2_miss_rate_candidate']:.3f} "
                f"idle={summary['idle_l2_miss_rate_baseline']:.3f}/"
                f"{summary['idle_l2_miss_rate_candidate']:.3f} "
                f"cand-only/base-only="
                f"{summary['l2_candidate_only']}/"
                f"{summary['l2_baseline_only']} "
                f"p={summary['l2_mcnemar_p']:.3g} "
                f"LLmiss={summary['ll_miss_rate_baseline']:.3f}/"
                f"{summary['ll_miss_rate_candidate']:.3f} "
                f"L2inv={summary['stimulus_l2_inval_rate_baseline']:.3f}/"
                f"{summary['stimulus_l2_inval_rate_candidate']:.3f} "
                f"tick_delta_med={summary['tick_delta_median']:.1f}"
            )

    print(f"Wrote {output}")
    if errors:
        print("Skipped files:")
        for error in errors:
            print(f"  {error}")


if __name__ == "__main__":
    main()
