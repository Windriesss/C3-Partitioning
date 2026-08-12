#!/usr/bin/env python3
"""Compare same- and cross-probe distributions from every CSV in a folder."""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt


DEFAULT_TYPES = (
    "same_baseline",
    "same_candidate",
    "cross_baseline",
    "cross_candidate",
)


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def load_csv(csv_path):
    """Return {sample_type: [probe_ticks, ...]} and comment metadata."""
    metadata = {}
    data_lines = []
    with csv_path.open("r", encoding="utf-8-sig", newline="") as csv_file:
        for line in csv_file:
            if line.startswith("#"):
                key, separator, value = line[1:].strip().partition(",")
                if separator:
                    metadata[key.strip()] = value.strip()
            elif line.strip():
                data_lines.append(line)

    reader = csv.DictReader(data_lines)
    if not reader.fieldnames or not {"type", "probe_ticks"}.issubset(
        reader.fieldnames
    ):
        raise ValueError("required columns 'type' and 'probe_ticks' were not found")

    samples = {}
    for line_number, row in enumerate(reader, start=2):
        sample_type = (row.get("type") or "").strip()
        raw_ticks = (row.get("probe_ticks") or "").strip()
        if not sample_type or not raw_ticks:
            continue
        try:
            ticks = float(raw_ticks)
        except ValueError as error:
            raise ValueError(
                f"invalid probe_ticks at data row {line_number}: {raw_ticks!r}"
            ) from error
        if not math.isfinite(ticks):
            raise ValueError(
                f"non-finite probe_ticks at data row {line_number}: {raw_ticks!r}"
            )
        samples.setdefault(sample_type, []).append(ticks)
    return samples, metadata


def find_csv_files(folder, recursive):
    pattern = "**/*.csv" if recursive else "*.csv"
    return sorted(path for path in folder.glob(pattern) if path.is_file())


def make_label(csv_path, folder, recursive):
    if recursive:
        return str(csv_path.relative_to(folder).with_suffix(""))
    return csv_path.stem


def write_summary(output_path, datasets, sample_types):
    summary_path = output_path.with_name(f"{output_path.stem}_summary.csv")
    with summary_path.open("w", encoding="utf-8", newline="") as csv_file:
        fieldnames = [
            "file",
            "type",
            "n",
            "mean",
            "median",
            "p95",
            "minimum",
            "maximum",
            "counter",
            "access_items",
            "fill_rounds",
            "match_bits",
            "probe_cpu",
            "stimulus_cpu",
        ]
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for dataset in datasets:
            for sample_type in sample_types:
                values = dataset["samples"].get(sample_type, [])
                if not values:
                    continue
                metadata = dataset["metadata"]
                writer.writerow(
                    {
                        "file": dataset["label"],
                        "type": sample_type,
                        "n": len(values),
                        "mean": f"{statistics.fmean(values):.6g}",
                        "median": f"{statistics.median(values):.6g}",
                        "p95": f"{percentile(values, 0.95):.6g}",
                        "minimum": f"{min(values):.6g}",
                        "maximum": f"{max(values):.6g}",
                        "counter": metadata.get("counter", ""),
                        "access_items": metadata.get("access_items", ""),
                        "fill_rounds": metadata.get("fill_rounds", ""),
                        "match_bits": metadata.get("match_bits", ""),
                        "probe_cpu": metadata.get("probe_cpu", ""),
                        "stimulus_cpu": metadata.get("stimulus_cpu", ""),
                    }
                )
    return summary_path


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Compare same- and cross-probe probe_ticks distributions across "
            "all CSV files in a folder."
        )
    )
    parser.add_argument("folder", type=Path, help="Folder containing CSV files")
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help=(
            "Output PNG path (default: "
            "<folder>/crossprobe_folder_distribution.png)"
        ),
    )
    parser.add_argument("--bins", type=int, default=80, help="Histogram bins")
    parser.add_argument(
        "--types",
        nargs="+",
        default=list(DEFAULT_TYPES),
        metavar="TYPE",
        help=(
            "Sample types to compare (default: same_baseline same_candidate "
            "cross_baseline cross_candidate)"
        ),
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Also scan CSV files in subfolders",
    )
    parser.add_argument(
        "--title",
        default="Same-CPU and cross-CPU latency distribution comparison",
        help="Figure title",
    )
    args = parser.parse_args()

    if args.bins <= 0:
        parser.error("--bins must be positive")
    folder = args.folder.resolve()
    if not folder.is_dir():
        parser.error(f"folder does not exist or is not a directory: {folder}")
    output_path = (
        args.out.resolve()
        if args.out is not None
        else folder / "crossprobe_folder_distribution.png"
    )

    csv_paths = find_csv_files(folder, args.recursive)
    datasets = []
    for csv_path in csv_paths:
        # Do not ingest a summary created by an earlier run.
        if csv_path.name == f"{output_path.stem}_summary.csv":
            continue
        try:
            samples, metadata = load_csv(csv_path)
        except (OSError, ValueError) as error:
            print(f"Warning: skipped {csv_path}: {error}", file=sys.stderr)
            continue
        selected = {
            sample_type: samples[sample_type]
            for sample_type in args.types
            if samples.get(sample_type)
        }
        if not selected:
            print(
                f"Warning: skipped {csv_path}: none of the requested types exist",
                file=sys.stderr,
            )
            continue
        datasets.append(
            {
                "path": csv_path,
                "label": make_label(csv_path, folder, args.recursive),
                "samples": selected,
                "metadata": metadata,
            }
        )

    if not datasets:
        parser.error("no usable CSV files containing the requested sample types")

    all_values = [
        value
        for dataset in datasets
        for values in dataset["samples"].values()
        for value in values
    ]
    lower, upper = min(all_values), max(all_values)
    if lower == upper:
        lower -= 0.5
        upper += 0.5
    edges = [
        lower + (upper - lower) * index / args.bins
        for index in range(args.bins + 1)
    ]

    column_count = 2 if len(args.types) > 1 else 1
    row_count = math.ceil(len(args.types) / column_count)
    figure, axes = plt.subplots(
        row_count,
        column_count,
        figsize=(14, max(4.8, 4.5 * row_count)),
        sharex=True,
        sharey=True,
        squeeze=False,
    )
    colors = plt.get_cmap("tab10")
    flat_axes = list(axes.flat)
    for axis, sample_type in zip(flat_axes, args.types):
        plotted = 0
        for dataset_index, dataset in enumerate(datasets):
            values = dataset["samples"].get(sample_type)
            if not values:
                continue
            median = statistics.median(values)
            label = f"{dataset['label']} (n={len(values)}, med={median:.1f})"
            axis.hist(
                values,
                bins=edges,
                density=True,
                histtype="step",
                linewidth=1.7,
                color=colors(dataset_index % 10),
                label=label,
            )
            plotted += 1
        axis.set_title(sample_type.replace("_", " "))
        axis.set_yscale("log")
        axis.set_ylabel("Density (log scale)")
        axis.grid(alpha=0.2)
        if plotted:
            axis.legend(fontsize=8)
        else:
            axis.text(
                0.5,
                0.5,
                "No samples of this type",
                ha="center",
                va="center",
                transform=axis.transAxes,
            )

    for axis in flat_axes[len(args.types) :]:
        axis.axis("off")

    counters = {
        dataset["metadata"].get("counter", "")
        for dataset in datasets
        if dataset["metadata"].get("counter")
    }
    counter = next(iter(counters)) if len(counters) == 1 else "counter_ticks"
    for axis in axes[-1, :]:
        if axis.axison:
            axis.set_xlabel(f"Single probe access latency ({counter})")
    figure.suptitle(args.title)
    figure.tight_layout()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output_path, dpi=180, bbox_inches="tight")
    plt.close(figure)
    summary_path = write_summary(output_path, datasets, args.types)

    print(f"Loaded {len(datasets)} CSV file(s) from: {folder}")
    print(f"Saved plot to: {output_path}")
    print(f"Saved summary to: {summary_path}")
    for dataset in datasets:
        for sample_type in args.types:
            values = dataset["samples"].get(sample_type)
            if values:
                print(
                    f"{dataset['label']} / {sample_type}: "
                    f"n={len(values)}, "
                    f"mean={statistics.fmean(values):.2f}, "
                    f"median={statistics.median(values):.2f}, "
                    f"p95={percentile(values, 0.95):.2f}"
                )


if __name__ == "__main__":
    main()
