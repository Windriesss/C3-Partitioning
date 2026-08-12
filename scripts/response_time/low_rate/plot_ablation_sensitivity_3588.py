"""Plot the RK3588 ablation experiment and partition-ratio sensitivity.

The layout and visual encoding follow
plotting_tools/ablation_oee_rtos_stress/
plot_ablation_sensitivity_oee_rtos_stress.py.  Data are parsed directly from
the complete 100/10 Hz runs in the bundled RK3588 low-rate data. For every
condition and working-set point, summaries are searched newest-first and the
first usable complete run is plotted. Missing complete runs are fatal.
"""

from __future__ import annotations

import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter
import numpy as np

import plot_3588_all_conditions as base


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
MAIN_RESULTS_DIR = MATERIAL_ROOT / "data" / "response_time" / "low_rate" / "rk3588" / "results"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "appendix" / "response_time"
OUTPUT_BASENAME = "rk3588_100_10Hz_ablation_sensitivity_tresp"
FREQUENCIES_HZ = (100, 10)

CONFIGS = (
    {
        "label": "Baseline",
        "csv_label": "Baseline",
        "group": "Baseline",
        "ratio": "",
        "condition_prefix": "Native_rtos_stress",
    },
    {
        "label": "SP",
        "csv_label": "SP",
        "group": "Set Partition",
        "ratio": "",
        "condition_prefix": "SP_rtos_stress",
    },
    {
        "label": "TaskColoring 1/4",
        "csv_label": "TaskColoring 1/4",
        "group": "TaskColoring",
        "ratio": "1/4",
        "condition_prefix": "TaskColoring_4-16_rtos_stress",
    },
    {
        "label": "TaskColoring 2/4",
        "csv_label": "TaskColoring 2/4",
        "group": "TaskColoring",
        "ratio": "2/4",
        "condition_prefix": "TaskColoring_8-16_rtos_stress",
    },
    {
        "label": "TaskColoring 3/4",
        "csv_label": "TaskColoring 3/4",
        "group": "TaskColoring",
        "ratio": "3/4",
        "condition_prefix": "TaskColoring_12-16_rtos_stress",
    },
    {
        "label": r"$C^3$-Partitioning 1/4",
        "csv_label": "C3-Partitioning 1/4",
        "group": "C3",
        "ratio": "1/4",
        "condition_prefix": "HCP_4-16_rtos_stress",
    },
    {
        "label": r"$C^3$-Partitioning 2/4",
        "csv_label": "C3-Partitioning 2/4",
        "group": "C3",
        "ratio": "2/4",
        "condition_prefix": "HCP_8-16_rtos_stress",
    },
    {
        "label": r"$C^3$-Partitioning 3/4",
        "csv_label": "C3-Partitioning 3/4",
        "group": "C3",
        "ratio": "3/4",
        "condition_prefix": "HCP_12-16_rtos_stress",
    },
)

METRICS = (
    ("t2_max", r"$T_{\mathrm{resp}}^{\max}$ ($\mu$s)"),
    ("avg_l2_inval", "Avg. L2D invalidations"),
    ("avg_l2_refill", "Avg. L2D cache refills"),
    ("avg_ll_miss_rd", "Avg. L3 read misses"),
)

STYLE = {
    "Baseline": {
        "color": "#222222", "marker": "o", "linestyle": "-",
        "linewidth": 1.75, "markerface": "#222222", "zorder": 20,
    },
    "WP": {
        "color": "#B36B2C", "marker": "X", "linestyle": "-.",
        "linewidth": 1.55, "markerface": "#B36B2C", "zorder": 18,
    },
    "SP": {
        "color": "#4A8C5A", "marker": "^", "linestyle": "-",
        "linewidth": 1.75, "markerface": "#4A8C5A", "zorder": 19,
    },
    "TaskColoring 1/4": {
        "color": "#C7C0E3", "marker": "v", "linestyle": "--",
        "linewidth": 1.25, "markerface": "white", "zorder": 10,
    },
    "TaskColoring 2/4": {
        "color": "#8174B7", "marker": "s", "linestyle": "--",
        "linewidth": 1.35, "markerface": "white", "zorder": 11,
    },
    "TaskColoring 3/4": {
        "color": "#4E3293", "marker": "P", "linestyle": "--",
        "linewidth": 1.45, "markerface": "white", "zorder": 12,
    },
    r"$C^3$-Partitioning 1/4": {
        "color": "#A7CBEA", "marker": "D", "linestyle": "-",
        "linewidth": 1.35, "markerface": "#A7CBEA", "zorder": 14,
    },
    r"$C^3$-Partitioning 2/4": {
        "color": "#4E9AD4", "marker": "<", "linestyle": "-",
        "linewidth": 1.45, "markerface": "#4E9AD4", "zorder": 15,
    },
    r"$C^3$-Partitioning 3/4": {
        "color": "#0C5A9E", "marker": "*", "linestyle": "-",
        "linewidth": 1.55, "markerface": "#0C5A9E", "zorder": 16,
    },
}

VISIBLE_X_LABELS = {"128K", "512K", "2M", "8M", "32M", "128M", "512M", "1G"}


def setup_matplotlib() -> None:
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 9.5,
        "axes.labelsize": 9.5,
        "axes.titlesize": 9.5,
        "xtick.labelsize": 8.5,
        "ytick.labelsize": 8.5,
        "legend.fontsize": 8.5,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02,
    })


def condition_name(config: dict, frequency_hz: int) -> str:
    return f"{config['condition_prefix']}_{frequency_hz}Hz"


def scan_complete_records(
    results_dir: Path,
) -> tuple[list[dict], list[dict], list[tuple[str, str]]]:
    """Select one usable complete run per condition and working-set point."""
    selected_conditions = {
        condition_name(config, frequency_hz)
        for config in CONFIGS
        for frequency_hz in FREQUENCIES_HZ
    }
    results_dir = results_dir.resolve()
    vm_labels = set()
    for condition in selected_conditions:
        condition_dir = results_dir / condition
        if not condition_dir.is_dir():
            continue
        for vm_dir in condition_dir.glob("vm_*"):
            if not vm_dir.is_dir():
                continue
            label = vm_dir.name.removeprefix("vm_")
            try:
                base.vm_to_bytes(label)
            except ValueError:
                continue
            vm_labels.add(label)

    records = []
    issues = []
    missing = []
    for condition in sorted(selected_conditions):
        for vmbytes in sorted(vm_labels, key=base.vm_to_bytes):
            selected_record = None
            vm_dir = results_dir / condition / f"vm_{vmbytes}"
            if vm_dir.is_dir():
                # Reverse lexical order checks timestamped summaries newest-first.
                for summary_path in sorted(
                    vm_dir.glob("*_summary.json"), reverse=True
                ):
                    record, reason = base.parse_run(
                        summary_path, condition, vmbytes
                    )
                    if record is not None:
                        selected_record = record
                        break
                    issues.append(
                        {
                            "condition": condition,
                            "vmbytes": vmbytes,
                            "file": base.relative_source_path(summary_path),
                            "reason": reason,
                        }
                    )
            if selected_record is not None:
                records.append(selected_record)
            else:
                missing.append((condition, vmbytes))
                issues.append(
                    {
                        "condition": condition,
                        "vmbytes": vmbytes,
                        "file": "-",
                        "reason": "no usable complete run in main results",
                    }
                )
    return records, issues, missing


def aggregate_selected(records: list[dict]) -> list[dict]:
    selected_conditions = {
        condition_name(config, frequency_hz)
        for config in CONFIGS
        for frequency_hz in FREQUENCIES_HZ
    }
    grouped: dict[tuple[str, str, str], list[float]] = defaultdict(list)
    for record in records:
        if record["condition"] not in selected_conditions:
            continue
        for metric, _ in METRICS:
            value = float(record.get(metric, math.nan))
            if math.isfinite(value):
                grouped[(record["condition"], record["vmbytes"], metric)].append(value)

    stats = []
    for (condition, vmbytes, metric), values in grouped.items():
        array = np.asarray(values, dtype=float)
        stats.append({
            "condition": condition,
            "vmbytes": vmbytes,
            "metric": metric,
            "n": len(array),
            "median": float(np.median(array)),
            "min": float(np.min(array)),
            "max": float(np.max(array)),
        })
    return stats


def metric_lookup(stats: list[dict], metric: str) -> dict[tuple[str, str], dict]:
    return {
        (item["condition"], item["vmbytes"]): item
        for item in stats
        if item["metric"] == metric
    }


def clean_axis(ax, vm_labels: list[str]) -> None:
    ax.set_xticks([base.vm_x(label) for label in vm_labels])
    ax.set_xticklabels(
        [label if label in VISIBLE_X_LABELS else "" for label in vm_labels],
        rotation=48,
        ha="right",
        rotation_mode="anchor",
    )
    ax.grid(axis="y", linestyle=":", linewidth=0.55, alpha=0.45, color="#9A9A9A")
    ax.grid(axis="x", visible=False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(axis="both", direction="out", length=3, width=0.7)
    ax.ticklabel_format(style="plain", axis="y")
    ax.margins(x=0.02)


def plot_series(
    ax,
    lookup: dict,
    config: dict,
    frequency_hz: int,
    vm_labels: list[str],
) -> bool:
    condition = condition_name(config, frequency_hz)
    labels = [label for label in vm_labels if (condition, label) in lookup]
    if not labels:
        return False

    points = [lookup[(condition, label)] for label in labels]
    x_values = np.asarray([base.vm_x(label) for label in labels])
    medians = np.asarray([point["median"] for point in points])
    minima = np.asarray([point["min"] for point in points])
    maxima = np.asarray([point["max"] for point in points])
    style = STYLE[config["label"]]

    if any(point["n"] > 1 for point in points):
        ax.fill_between(
            x_values,
            minima,
            maxima,
            color=style["color"],
            alpha=0.11,
            linewidth=0,
            zorder=style["zorder"] - 2,
        )
    ax.plot(
        x_values,
        medians,
        marker=style["marker"],
        linestyle=style["linestyle"],
        linewidth=style["linewidth"],
        markersize=4.0,
        markeredgewidth=0.7,
        markeredgecolor=style["color"],
        markerfacecolor=style["markerface"],
        label=config["label"],
        color=style["color"],
        zorder=style["zorder"],
    )
    return True


def save_values(stats: list[dict], vm_labels: list[str]) -> Path:
    lookup_by_metric = {
        metric: metric_lookup(stats, metric)
        for metric, _ in METRICS
    }
    output_path = OUTPUT_DIR / f"{OUTPUT_BASENAME}_values.csv"
    with output_path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            ["frequency_hz", "method", "group", "ratio", "gpos_vmbytes", "runs"]
            + [metric for metric, _ in METRICS]
        )
        for frequency_hz in FREQUENCIES_HZ:
            for config in CONFIGS:
                condition = condition_name(config, frequency_hz)
                for vm_label in vm_labels:
                    points = [
                        lookup_by_metric[metric].get((condition, vm_label))
                        for metric, _ in METRICS
                    ]
                    if not any(points):
                        continue
                    runs = max((point["n"] for point in points if point), default=0)
                    writer.writerow(
                        [
                            frequency_hz,
                            config["csv_label"],
                            config["group"],
                            config["ratio"],
                            vm_label,
                            runs,
                        ]
                        + ["" if point is None else point["median"] for point in points]
                    )
    return output_path


def plot(stats: list[dict], vm_labels: list[str]) -> tuple[Path, Path]:
    fig, axes = plt.subplots(
        len(FREQUENCIES_HZ),
        len(METRICS),
        figsize=(10.4, 8.0),
        sharex=True,
        squeeze=False,
    )
    for row, frequency_hz in enumerate(FREQUENCIES_HZ):
        for column, (metric, ylabel) in enumerate(METRICS):
            ax = axes[row, column]
            lookup = metric_lookup(stats, metric)
            for config in CONFIGS:
                if not plot_series(ax, lookup, config, frequency_hz, vm_labels):
                    print(
                        f"[Missing] {frequency_hz} Hz / "
                        f"{config['label']} / {metric}"
                    )
            if row == 0:
                ax.set_title(ylabel, pad=5)
            clean_axis(ax, vm_labels)
            if metric == "avg_l2_inval":
                formatter = ScalarFormatter(useMathText=True)
                formatter.set_scientific(True)
                formatter.set_powerlimits((3, 3))
                ax.yaxis.set_major_formatter(formatter)
                ax.yaxis.get_offset_text().set_fontsize(8.5)
        axes[row, -1].text(
            1.04,
            0.5,
            f"{frequency_hz} Hz",
            transform=axes[row, -1].transAxes,
            rotation=-90,
            ha="left",
            va="center",
            fontsize=10,
            fontweight="bold",
        )

    handles_by_label = {}
    for ax in axes.flat:
        ax_handles, ax_labels = ax.get_legend_handles_labels()
        for handle, label in zip(ax_handles, ax_labels):
            handles_by_label.setdefault(label, handle)
    labels = [
        config["label"]
        for config in CONFIGS
        if config["label"] in handles_by_label
    ]
    handles = [handles_by_label[label] for label in labels]
    display_labels = [
        label.replace(r"$C^3$-Partitioning", r"$C^3$")
        for label in labels
    ]
    fig.legend(
        handles,
        display_labels,
        loc="lower center",
        bbox_to_anchor=(0.5, 0.006),
        ncol=len(labels),
        frameon=False,
        fontsize=8.4,
        handlelength=1.15,
        columnspacing=0.75,
        handletextpad=0.28,
        labelspacing=0.0,
        borderaxespad=0.0,
    )
    fig.supxlabel("Aggregate GPOS working-set size", fontsize=10, y=0.052)
    fig.subplots_adjust(
        top=0.955,
        bottom=0.145,
        left=0.055,
        right=0.965,
        hspace=0.28,
        wspace=0.25,
    )

    png_path = OUTPUT_DIR / f"{OUTPUT_BASENAME}.png"
    pdf_path = OUTPUT_DIR / f"{OUTPUT_BASENAME}.pdf"
    fig.savefig(png_path, dpi=300)
    fig.savefig(pdf_path)
    plt.close(fig)
    return png_path, pdf_path


def plot_frequency(
    stats: list[dict], vm_labels: list[str], frequency_hz: int
) -> tuple[Path, Path]:
    """Create one compact 1x4 appendix figure for a single frequency."""
    fig, axes = plt.subplots(1, len(METRICS), figsize=(10.4, 3.75), sharex=True)

    for ax, (metric, ylabel) in zip(axes, METRICS):
        lookup = metric_lookup(stats, metric)
        for config in CONFIGS:
            if not plot_series(ax, lookup, config, frequency_hz, vm_labels):
                print(f"[Missing] {frequency_hz} Hz / {config['label']} / {metric}")
        ax.set_ylabel(ylabel)
        clean_axis(ax, vm_labels)
        if metric == "avg_l2_inval":
            formatter = ScalarFormatter(useMathText=True)
            formatter.set_scientific(True)
            formatter.set_powerlimits((3, 3))
            ax.yaxis.set_major_formatter(formatter)
            ax.yaxis.get_offset_text().set_fontsize(8.5)

    handles_by_label = {}
    for ax in axes:
        ax_handles, ax_labels = ax.get_legend_handles_labels()
        for handle, label in zip(ax_handles, ax_labels):
            handles_by_label.setdefault(label, handle)
    labels = [
        config["label"]
        for config in CONFIGS
        if config["label"] in handles_by_label
    ]
    display_labels = [
        label.replace(r"$C^3$-Partitioning", r"$C^3$")
        for label in labels
    ]
    fig.legend(
        [handles_by_label[label] for label in labels],
        display_labels,
        loc="lower center",
        bbox_to_anchor=(0.5, 0.006),
        ncol=len(labels),
        frameon=False,
        fontsize=8.4,
        handlelength=1.15,
        columnspacing=0.75,
        handletextpad=0.28,
        labelspacing=0.0,
        borderaxespad=0.0,
    )
    fig.supxlabel("Aggregate GPOS working-set size", fontsize=10, y=0.085)
    fig.subplots_adjust(
        top=0.975,
        bottom=0.29,
        left=0.06,
        right=0.995,
        wspace=0.30,
    )

    stem = f"rk3588_{frequency_hz}Hz_ablation_sensitivity_tresp_1x4"
    png_path = OUTPUT_DIR / f"{stem}.png"
    pdf_path = OUTPUT_DIR / f"{stem}.pdf"
    fig.savefig(png_path, dpi=300)
    fig.savefig(pdf_path)
    plt.close(fig)
    return png_path, pdf_path


def print_summary(stats: list[dict]) -> None:
    lookup = metric_lookup(stats, "t2_max")
    print("\n[RK3588 ablation sensitivity: maximum median Tresp by frequency]")
    print("frequency_hz\tmethod\tmax_Tresp_us\tworking_set")
    for frequency_hz in FREQUENCIES_HZ:
        for config in CONFIGS:
            condition = condition_name(config, frequency_hz)
            values = [
                (point["median"], vmbytes)
                for (item_condition, vmbytes), point in lookup.items()
                if item_condition == condition
            ]
            if values:
                value, vmbytes = max(values)
                print(
                    f"{frequency_hz}\t{config['csv_label']}\t"
                    f"{value:.6g}\t{vmbytes}"
                )


def main() -> int:
    setup_matplotlib()
    records, issues, missing = scan_complete_records(
        MAIN_RESULTS_DIR
    )
    if missing:
        print(
            "[Error] Missing usable complete RK3588 run(s) in main results; "
            "continuing without these points:"
        )
        for condition, vmbytes in missing:
            print(f"  - {condition} / vm_{vmbytes}")
    stats = aggregate_selected(records)
    if not stats:
        raise RuntimeError(
            "No complete RK3588 100/10 Hz ablation records were found"
        )

    vm_labels = base.available_vm_labels(stats)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    values_path = save_values(stats, vm_labels)
    figure_paths = [
        plot_frequency(stats, vm_labels, frequency_hz)
        for frequency_hz in FREQUENCIES_HZ
    ]
    print(
        f"[Loaded] complete_runs={len(records)}, vm_points={len(vm_labels)}, "
        f"excluded_candidates={len(issues)}"
    )
    print_summary(stats)
    print(f"[Saved] {values_path}")
    for png_path, pdf_path in figure_paths:
        print(f"[Saved] {png_path}")
        print(f"[Saved] {pdf_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
